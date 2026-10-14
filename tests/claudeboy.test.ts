import { expect, mock, test } from 'claude-code/testing'
import type { Engine } from 'claude-code/testing'
import type { On } from 'claude-code'

type MockClock = ReturnType<typeof mock.clock>

const ROM = '/home/ada/.local/share/claudeboy/roms/Pokemon Yellow.gb'
const INFO = '{"title":"POKEMON YELLOW","key":"pokemon-yellow-7d527d62","color":true}'
// A frame of 100 by 45 cells, each an upper half block, as the engine writes
// them: 0x2580 then two colors, little-endian, base64. Every cell is the same,
// so the twelve bytes of one encode to sixteen characters, repeated.
const CELLS = 'gCUAAFjo+ABQWDAA'.repeat(100 * 45)

// Lines the fake engine prints, handed out as the test pushes them, until it
// exits. While it runs, the kit takes about a second to settle each step, as
// it does for any child that's still running.
function channel() {
  const lines: string[] = []
  let wake: (() => void) | null = null
  let isClosed = false
  const nudge = () => {
    wake?.()
    wake = null
  }
  return {
    push(line: string) {
      lines.push(line + '\n')
      nudge()
    },
    exit() {
      isClosed = true
      nudge()
    },
    async *drain(): AsyncGenerator<string> {
      for (;;) {
        while (lines.length > 0) yield lines.shift() as string
        if (isClosed) return
        await new Promise<void>((resolve) => {
          wake = resolve
        })
      }
    },
  }
}

const SLOW = { timeoutMs: 30_000 }

type World = {
  opens: number
  openColumns: (number | undefined)[]
  // Whether each open lets Esc close the pane
  closeOnEscape: (boolean | undefined)[]
  closes: number
  spawns: (readonly string[])[]
  controls: string[]
  blits: unknown[]
  toasts: string[]
  engine: ReturnType<typeof channel>
  // The newest line the mod wrote to the engine's control file
  control: () => string
}

// Answers everything the mod asks of Claude Code, and plays the engine: it
// answers each command it finds in the control file, as the real one does.
function stubClaudeCode(
  on: On,
  clock: MockClock,
  {
    store = { isOn: true } as Record<string, unknown>,
    env = {} as Record<string, string>,
    isPlaced = (n: number) => n > 0,
    cells = '100 45 1\n' + CELLS,
    // Another session holds the game's saves, so the engine is turned away
    isBusy = false,
  } = {},
): World {
  const world: World = {
    opens: 0,
    openColumns: [],
    closeOnEscape: [],
    closes: 0,
    spawns: [],
    controls: [],
    blits: [],
    toasts: [],
    engine: channel(),
    control: () => world.controls[world.controls.length - 1] ?? '',
  }
  const answered = new Set<string>()
  mock.store(on, store)
  mock.env(on, { HOME: '/home/ada', XDG_RUNTIME_DIR: '/run/user/1000', TERM: 'xterm-256color', ...env })
  on('command.register', () => ({ value: undefined }) as never)
  on('session.start', () => ({ cwd: '/work' }) as never)
  on('session.surfaces', () => ({ value: ['terminal'] }))
  on('ui.open', ($, e) => {
    world.opens += 1
    world.openColumns.push(e.columns)
    world.closeOnEscape.push(e.closeOnEscape)
    return { value: isPlaced(world.opens) ? { isPlaced: true } : { isPlaced: false, reason: 'narrow' } }
  })
  on('ui.close', () => {
    world.closes += 1
    return { value: undefined }
  })
  // What Claude Code itself draws in the band, under anything the mod adds
  on('ui.render', () => ({ type: 'Text', props: {}, children: [''] }) as never)
  on('ui.toast', ($, e) => {
    world.toasts.push(e.text)
    return { value: undefined }
  })
  on('ui.log', () => ({ value: undefined }))
  on('ui.blit', ($, e) => {
    world.blits.push(e)
    return { value: {} }
  })
  on('fs.exists', () => ({ value: true }))
  on('fs.list', ($, e) => ({
    value: e.path.endsWith('/claudeboy/roms')
      ? [{ name: 'Pokemon Yellow.gb', kind: 'file' as const, size: 1048576, mtimeMs: 0, isLink: false }]
      : [],
  }))
  on('fs.read', () => ({ value: cells }))
  let isPlayAsked = false
  on('fs.write', ($, e) => {
    world.controls.push(e.text)
    // While another session plays, asking to play gets told so, once
    const isPlay = e.text.startsWith('play=1')
    if (isBusy && isPlay && !isPlayAsked) world.engine.push('@busy')
    isPlayAsked = isPlay
    for (const command of /cmds=(\S+)/.exec(e.text)?.[1]?.split(',') ?? []) {
      if (answered.has(command)) continue
      answered.add(command)
      const [id, name, arg] = command.split(':')
      if (name === 'save') world.engine.push('@saved ' + arg + ' 1000')
      world.engine.push('@done ' + id + ' ok ' + name + (arg ? ' ' + arg : ''))
      if (name === 'quit') {
        world.engine.push('@bye')
        world.engine.exit()
      }
    }
    return { value: undefined }
  })
  on('process.run', () => ({
    value: { exitCode: 0, stdout: INFO, stderr: '', isStdoutTruncated: false, isStderrTruncated: false },
  }))
  on('process.spawn', async function* ($, e) {
    world.spawns.push(e.argv)
    world.engine.push('@ready pokemon-yellow-7d527d62 1 POKEMON YELLOW')
    for await (const text of world.engine.drain()) yield { stream: 'stdout' as const, text }
    return { value: { code: 0, signal: null } } as never
  })
  on('turn.start', ($, e) => ({ turnId: e.turnId }) as never)
  on('turn.complete', () => ({ text: '' }))
  on('tool.check', () => ({ decision: 'ask' as const }))
  // No settings hook decides: the dialog shows
  on('classic.PermissionRequest', () => ({}) as never)
  on('tool.call', () => ({ result: 'ok' }) as never)
  return world
}

async function startSession($: Engine) {
  await $.session.start({ surface: 'terminal', isInteractive: true, cwd: '/work' } as never)
}

function finishTurn($: Engine, fields: Record<string, unknown> = {}) {
  return $.turn.complete({
    turnId: 't1',
    answer: '',
    durationMs: 1000,
    isAborted: false,
    reason: 'answer',
    ...fields,
  } as never)
}

// The dialog that asks the person, as the engine raises it before showing it
const askPermission = ($: Engine) =>
  $.classic.PermissionRequest({ tool_name: 'Bash', tool_input: { command: 'rm -rf build' } } as never)

const GAME = {
  plugin: 'claudeboy',
  component: 'Pane',
  requestId: 'claudeboy',
  surface: 'terminal',
  viewport: { columns: 200, rows: 50 },
  props: {
    title: 'Pokemon Yellow',
    isFocused: true,
    bodyColumns: 100,
    placement: 'dock',
    scroll: { offset: 0, bodyRows: 47 },
    view: {},
  },
} as const

const BAND = {
  plugin: 'claudeboy',
  component: 'AbovePrompt',
  requestId: 'band',
  surface: 'terminal',
  viewport: { columns: 100, rows: 30 },
  props: { hasSurvey: false, isWorking: true, maxRows: 4, bodyColumns: 100, scroll: { offset: 0, bodyRows: 4 }, view: {} },
} as const

test('drops in once Claude has worked for two seconds, on the ROM it found', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(1999)
  expect(world.opens).toBe(0)
  await clock.advance(1)
  expect(world.opens).toBe(1)
  await clock.settle()
  const argv = world.spawns[0] ?? []
  expect(argv.slice(1, 3)).toEqual(['run', ROM])
  expect(argv[3]).toBe('/home/ada/.local/share/claudeboy/saves/pokemon-yellow-7d527d62')
  expect(argv[4]).toMatch(/^\/run\/user\/1000\/claudeboy-\w+$/)
  expect(world.control()).toMatch(/^play=1 /)
})

test('stays out while ClaudeBoy is off', async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { store: { isOn: false } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(5000)
  expect(world.opens).toBe(0)
  expect(world.spawns.length).toBe(0)
})

test('stays out of a turn that ends before the delay', async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'hi' })
  await clock.advance(1000)
  await finishTurn($)
  await clock.advance(5000)
  expect(world.opens).toBe(0)
})

test('counts down three seconds when Claude finishes, then pauses the game, which saves it', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await finishTurn($)
  await clock.advance(2999)
  expect(world.closes).toBe(0)
  expect(world.control()).toMatch(/^play=1 /)
  await clock.advance(1)
  expect(world.closes).toBe(1)
  expect(world.control()).toMatch(/^play=0 /)

  // The engine saves on pausing, and the toast says so
  world.engine.push('@saved auto 5000')
  world.engine.push('@paused')
  await clock.settle()
  expect(world.toasts).toContain("Claude's done · Pokemon Yellow saved")
})

test('hands back at once when Claude asks for permission, and drops in again after', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'clean up' })
  await clock.advance(2000)
  await askPermission($)
  expect(world.closes).toBe(1)
  expect(world.control()).toMatch(/^play=0 /)
  await $.tool.call({ tool: 'Bash', command: 'rm -rf build' } as never)
  await clock.advance(2000)
  expect(world.opens).toBe(2)
  // The same engine plays on
  expect(world.spawns.length).toBe(1)
})

test("keeps playing while auto mode's classifier answers an ask", SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'clean up' })
  await clock.advance(2000)
  // An ask that no dialog follows
  await $.tool.check({ tool: 'Bash', input: { command: 'rm -rf build' }, tool_use_id: 'u1' } as never)
  await $.tool.call({ tool: 'Bash', command: 'rm -rf build' } as never)
  await clock.advance(1000)
  expect(world.closes).toBe(0)
  expect(world.control()).toMatch(/^play=1 /)
})

test('hands back at once when Claude asks a question', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'plan it' })
  await clock.advance(2000)
  void $.tool.call({ tool: 'AskUserQuestion', questions: [] } as never)
  await clock.settle()
  expect(world.closes).toBe(1)
})

test('keeps playing when a subagent finishes', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await finishTurn($, { turnId: 't2', agentId: 'a1' })
  await clock.advance(5000)
  expect(world.closes).toBe(0)
})

test('hands back at once when the person interrupts Claude', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await finishTurn($, { isAborted: true, reason: 'aborted' })
  expect(world.closes).toBe(1)
})

test('draws the newest frame as half-block cells, sized to the pane', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  const game = await $.ui.mount(GAME)
  expect((await game.find({ text: /Starting Pokemon Yellow/ }))).toBeDefined()
  // 100 columns hold the screen at 100 by 45 cells, two pixels a cell
  expect(world.control()).toContain('view=cells:100x45')

  world.engine.push('@cells 1 100 45')
  await clock.settle()
  const view = await game.find({ key: 'view' })
  expect(view?.type).toBe('Raster')
  expect(view?.props.cells).toBe(CELLS)
  expect(view?.props.columns).toBe(100)
  expect(view?.props.rows).toBe(45)

  // Frames after the first are painted in place
  world.engine.push('@cells 2 100 45')
  await clock.settle()
  expect(world.blits).toEqual([{ requestId: 'claudeboy', key: 'view', cells: CELLS }])
})

test('in kitty, frames come as pictures in shared memory, drawn at the size shown', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { TERM: 'xterm-kitty' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  // The terminal's cells are 10 by 22 pixels
  world.engine.push('@term 200 50 2000 1100')
  await clock.settle()
  const game = await $.ui.mount(GAME)
  // 100 columns of 10 pixels, and 41 rows of 22 hold 1000 by 900 at the
  // screen's shape, a 2-pixel strip to spare
  expect(world.control()).toContain('view=image:1000x902')
  world.engine.push('@frame /claudeboy-7-0 1000 902')
  await clock.settle()
  const view = await game.find({ key: 'view' })
  expect(view?.props.source).toEqual({ shm: '/claudeboy-7-0', format: 'rgba', width: 1000, height: 902 })
  expect([view?.props.columns, view?.props.rows]).toEqual([100, 41])
})

test('the picture follows the pane as it resizes, in the middle at the screen\'s shape', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { TERM: 'xterm-kitty' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  world.engine.push('@term 200 50 2000 1100')
  await clock.settle()
  const game = await $.ui.mount(GAME)
  world.engine.push('@frame /claudeboy-7-0 1000 902')
  await clock.settle()

  // Narrower: the width decides
  await game.redraw({ ...GAME.props, bodyColumns: 60 })
  expect(world.control()).toContain('view=image:600x550')
  // Wide and short: the height decides, and the picture sits in the middle
  await game.redraw({ ...GAME.props, bodyColumns: 200, scroll: { offset: 0, bodyRows: 20 } })
  expect(world.control()).toContain('view=image:440x396')
  const view = await game.find({ key: 'view' })
  expect([view?.props.columns, view?.props.rows]).toEqual([44, 18])

  // A bigger font: the engine sees the terminal change, and pictures come at
  // the new size, the same cells now twice the pixels
  world.engine.push('@term 100 25 2000 1100')
  await clock.settle()
  expect(world.control()).toContain('view=image:880x792')
})

test('the gamepad: Z holds A, and S then a digit saves to that slot', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  const game = await $.ui.mount(GAME)
  world.engine.push('@cells 1 100 45')
  await clock.settle()
  expect(await game.find({ text: /Click the game to play/ })).toBeDefined()

  await game.key({ key: 'z', in: 'pad' })
  await game.advance(16)
  expect(world.control()).toMatch(/ keys=1 taps=1,0,0,0,0,0,0,0 /)
  // X is B
  await game.key({ key: 'x', in: 'pad' })
  await game.advance(16)
  expect(world.control()).toMatch(/ keys=3 taps=1,1,0,0,0,0,0,0 /)
  // A lone press is a tap: let go a moment later
  await game.advance(160)
  expect(world.control()).toMatch(/ keys=0 /)

  await game.key({ key: 's', in: 'pad' })
  await game.advance(16)
  expect(await game.find({ text: /Save to which slot\?/ })).toBeDefined()
  await game.key({ key: '2', in: 'pad' })
  await game.advance(16)
  await clock.settle()
  expect(world.controls.some((line) => /cmds=\d+:save:2/.test(line))).toBe(true)
  expect(await game.find({ text: 'Saved to slot 2' })).toBeDefined()
})

test('a held key counts as held while its repeats keep coming', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  const game = await $.ui.mount(GAME)
  world.engine.push('@cells 1 100 45')
  await clock.settle()

  // A press, the terminal's repeat delay, then repeats every 32 ms
  await game.key({ key: 'right', in: 'pad' })
  await game.advance(496)
  for (let i = 0; i < 4; i++) {
    await game.key({ key: 'right', in: 'pad' })
    await game.advance(32)
    expect(world.control()).toMatch(/ keys=16 /)
  }
  // The right arrow was tapped twice: the press and the first repeat
  expect(world.control()).toMatch(/ taps=0,0,0,0,2,0,0,0 /)
  await game.advance(160)
  await clock.settle()
  expect(world.control()).toMatch(/ keys=0 /)
})

test('/claudeboy save saves a slot while nobody plays', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  const saving = $.command.run({ command: 'claudeboy', args: 'save 3' } as never)
  await clock.settle()
  const result = await saving
  expect(result.text).toBe('Saved to slot 3 of Pokemon Yellow.')
  expect(world.spawns.length).toBe(1)
  // Started for the command, the engine stays paused
  expect(world.control()).toMatch(/^play=0 /)
})

test('offers a key in a narrow terminal, and pressing it drops in', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  // The pane can't open by itself, but opens when the person asks
  const world = stubClaudeCode(on, clock, { isPlaced: (n) => n > 1 })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  const band = await $.ui.mount(BAND)
  await band.press({ key: 'play' })
  expect(world.opens).toBe(2)
  await finishTurn($)
  await clock.advance(3000)
  expect(world.closes).toBe(2)
})

test('/claudeboy off hands back and stops the engine', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await clock.settle()
  const stopping = $.command.run({ command: 'claudeboy', args: 'off' } as never)
  await clock.advance(2000)
  expect((await stopping).text).toBe('ClaudeBoy is off. Your game is saved.')
  expect(world.closes).toBe(1)
  expect(world.controls.some((line) => /cmds=\d+:quit/.test(line))).toBe(true)
  // Off stays off
  await $.turn.start({ turnId: 't2', text: 'again' })
  await clock.advance(5000)
  expect(world.opens).toBe(1)
})

test('asks whether to play here when another session is playing the game; 1 asks that session for it', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { isBusy: true })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await clock.settle()
  const game = await $.ui.mount(GAME)
  expect(await game.find({ text: /Pokemon Yellow is being played in another Claude Code session/ })).toBeDefined()
  expect(await game.find({ text: /Would you like to play it here\?/ })).toBeDefined()
  expect(world.closes).toBe(0)

  await game.press({ key: 'takeover-yes' })
  await clock.settle()
  expect(world.controls.some((line) => /cmds=\d+:takeover/.test(line))).toBe(true)
  expect(await game.find({ text: /Asking the other session for Pokemon Yellow/ })).toBeDefined()

  // That session let go: the game is here now
  world.engine.push('@playing')
  await clock.settle()
  expect(await game.find({ text: /Would you like to play it here\?/ })).toBeUndefined()
  expect(world.control()).toMatch(/^play=1 /)
})

test('0 stays out of the game for the turn, and the next turn asks again', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { isBusy: true })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await clock.settle()
  const game = await $.ui.mount(GAME)
  await game.press({ key: 'takeover-no' })
  await clock.settle()
  expect(world.closes).toBe(1)
  expect(world.control()).toMatch(/^play=0 /)
  const opens = world.opens
  await $.tool.call({ tool: 'Bash', command: 'ls' } as never)
  await clock.advance(5000)
  expect(world.opens).toBe(opens)
  await finishTurn($)

  await $.turn.start({ turnId: 't2', text: 'again' })
  await clock.advance(2000)
  await clock.settle()
  expect(world.opens).toBeGreaterThan(opens)
})

test('a session asked for the game hands it over and says where it went', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  expect(world.control()).toMatch(/^play=1 /)
  world.engine.push('@saved auto 5000')
  world.engine.push('@yielded')
  await clock.settle()
  expect(world.toasts).toContain('Pokemon Yellow moved to another Claude Code session.')
  expect(world.control()).toMatch(/^play=0 /)
  // Not back for the rest of this turn
  await $.tool.call({ tool: 'Bash', command: 'ls' } as never)
  await clock.advance(5000)
  expect(world.control()).toMatch(/^play=0 /)
})

test('F steps the speed up to ×2, then ×4, then back to ×1', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  const game = await $.ui.mount(GAME)
  world.engine.push('@cells 1 100 45')
  await clock.settle()
  const speeds: string[] = []
  for (let i = 0; i < 3; i++) {
    await game.key({ key: 'f', in: 'pad' })
    await game.advance(16)
    speeds.push(/ speed=(\d+)/.exec(world.control())?.[1] ?? '?')
  }
  expect(speeds).toEqual(['2', '4', '1'])
})

// The window's title as the engine reads it out of the control line
function titleOf(line: string): string {
  const encoded = / title=(\S+)/.exec(line)?.[1] ?? ''
  return new TextDecoder().decode(Uint8Array.from(atob(encoded), (c) => c.charCodeAt(0)))
}

const HINT = 'S save · L load · M sound on · F ×2 · Esc back'
const WORKING = 'Pokemon Yellow · Claude is working\n' + HINT

test('in a terminal without pictures, the game plays in a drawer, the pane making room', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  await clock.settle()
  // The drawer holds the game and says what's going on; the pane only makes room
  expect(world.opens).toBe(1)
  expect(world.control()).toMatch(/^play=1 .*view=window /)
  expect(titleOf(world.control())).toBe('Pokemon Yellow · Click here to play\n' + HINT)
  world.engine.push('@focus 1')
  await clock.settle()
  expect(titleOf(world.control())).toBe(WORKING)

  await finishTurn($)
  await clock.advance(1000)
  expect(titleOf(world.control())).toMatch(/^Pokemon Yellow · Claude's done · back in 2 · Esc keeps playing\n/)
  await clock.advance(2000)
  expect(world.control()).toMatch(/^play=0 .*view=none /)
})

test('keys typed in the window save to a slot, the title saying so', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  world.engine.push('@focus 1')
  world.engine.push('@key s')
  await clock.settle()
  expect(titleOf(world.control())).toMatch(/^Pokemon Yellow · Save to which slot\? 1–9/)
  world.engine.push('@key 3')
  await clock.settle()
  expect(world.controls.some((line) => /cmds=\d+:save:3/.test(line))).toBe(true)
  await clock.settle()
  expect(titleOf(world.control())).toMatch(/^Pokemon Yellow · Saved to slot 3\n/)
})

test('closing the window pauses the game, and it stays closed for the turn', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  world.engine.push('@closed')
  await clock.settle()
  expect(world.control()).toMatch(/^play=0 .*view=none /)
  await $.tool.call({ tool: 'Bash', command: 'ls' } as never)
  await clock.advance(3000)
  expect(world.control()).toMatch(/^play=0 /)
  await finishTurn($)

  await $.turn.start({ turnId: 't2', text: 'again' })
  await clock.advance(2000)
  expect(world.control()).toMatch(/^play=1 .*view=window /)
})

test('where no window can open, the game plays in the pane', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  world.engine.push('@nowindow no X11 (libX11.so.6) to draw a window with')
  await clock.settle()
  // Once to make room for the drawer, then for the game itself
  expect(world.opens).toBe(2)
  await $.ui.mount(GAME)
  expect(world.control()).toContain('view=cells:100x45')
})

test('the pane opens as wide as the drawer, so the transcript moves aside', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  // A 120-column terminal across a desktop 1214 pixels wide, and a drawer of 512
  world.engine.push('@term 120 32 0 0')
  world.engine.push('@drawer 512 1214')
  await clock.settle()
  expect(world.openColumns.at(-1)).toBe(52)
  // Esc in the drawer hands back
  world.engine.push('@closed')
  await clock.settle()
  expect(world.control()).toMatch(/^play=0 /)
  expect(world.closes).toBeGreaterThan(0)
})

test('in a maximized terminal the console is laid over the pane, made wide enough for it', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  // 120 columns of 10 by 22 pixels, found on the screen by the engine
  world.engine.push('@term 120 32 0 0')
  world.engine.push('@drawer 464 1214')
  world.engine.push('@cell 10 22')
  await clock.settle()
  // The pane asks for room for the wide console at twice its 274 pixels
  expect(world.openColumns.at(-1)).toBe(56)
  const game = await $.ui.mount({ ...GAME, props: { ...GAME.props, bodyColumns: 56, scroll: { offset: 0, bodyRows: 25 } } })
  // The box: the pane's body from column 64, below its top row, all but the
  // two status rows
  expect(world.control()).toContain(' overlay=64,1,56,23 ')
  expect(await game.find({ text: /Click the game to play/ })).toBeDefined()
})

test('the countdown shows on the game; Esc on the console keeps playing, and the next Esc goes back', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock, { env: { DISPLAY: ':0' } })
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  world.engine.push('@focus 1')
  await finishTurn($)
  expect(world.control()).toContain(' countdown=3')
  await clock.advance(1000)
  expect(world.control()).toContain(' countdown=2')

  // Esc on the console: the count stops and the game goes on
  world.engine.push('@closed')
  await clock.settle()
  expect(world.control()).not.toContain('countdown=')
  expect(titleOf(world.control())).toMatch(/^Pokemon Yellow · Still playing · Esc goes back to Claude\n/)
  await clock.advance(5000)
  expect(world.control()).toMatch(/^play=1 .*view=window /)
  expect(world.closes).toBe(0)

  // Esc again: back to Claude
  world.engine.push('@closed')
  await clock.settle()
  expect(world.control()).toMatch(/^play=0 .*view=none /)
  expect(world.closes).toBe(1)
})

test('in the pane, Esc goes back to Claude, and the countdown shows on the game', SLOW, async ($, on) => {
  const clock = mock.clock(on)
  const world = stubClaudeCode(on, clock)
  await startSession($)

  await $.turn.start({ turnId: 't1', text: 'refactor auth' })
  await clock.advance(2000)
  // Esc closes the pane as the person's close; during the countdown the
  // ui.close hook refuses that and keeps playing
  expect(world.closeOnEscape.at(-1)).toBe(true)
  await finishTurn($)
  expect(world.control()).toContain(' countdown=3')
  await clock.advance(1000)
  expect(world.control()).toContain(' countdown=2')
  await clock.advance(2000)
  expect(world.control()).toMatch(/^play=0 /)
  expect(world.control()).not.toContain('countdown=')
})
