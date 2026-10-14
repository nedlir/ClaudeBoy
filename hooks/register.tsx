// ClaudeBoy: a Game Boy game in a pane while Claude works. You drop in once
// Claude has worked for a couple of seconds and are handed back when it's
// done or needs you. The game is saved each time you're handed back and picks
// up where you left it; S and L in the game save to and load from slots.
//
// The emulator is engine/claudeboy-engine.c, mGBA's core in a process of its
// own, all on this machine. It reads a control file this module rewrites (play
// or pause, the held buttons, how to draw, commands) and reports on stdout.
// Frames come as half-block cells drawn with a Raster, which any truecolor
// terminal shows, or in kitty and Ghostty as pictures in shared memory, drawn
// at the very size the terminal shows them. Either way the picture fills the
// pane at the screen's own shape and follows it as the pane or terminal resizes.
// In terminals without pictures, on a desktop, the game plays in a drawer of
// its own at the right edge of the screen instead, sharp, and the pane opens
// as wide as the drawer so the transcript moves aside rather than under it.

import type { EngineInterface, Register, Timer } from 'claude-code'

type Dollar = EngineInterface

const PANE = 'claudeboy'
const DROP_IN_DELAY_MS = 2000
const COUNTDOWN_SECONDS = 3
// After a hand-back the engine waits, paused, this long before it quits
const AWAY_STOP_MS = 10 * 60 * 1000
const COMMAND_TIMEOUT_MS = 5000
// After another session turned the engine away, stay out this long
const MESSAGE_MS = 2500
// The dock's width asked for when the pane opens; the person's own wins
const DOCK_COLUMNS = 100
const INLINE_ROWS = 40
const STATUS_ROWS = 2

const SCREEN_WIDTH = 160
const SCREEN_HEIGHT = 144
// The engine's wide console, in its own pixels (engine/console.c)
const WIDE_CONSOLE_WIDTH = 272
// A cell's size in pixels until the terminal says: most fonts are about
// twice as tall as wide
const DEFAULT_CELL = { width: 10, height: 20 }
const MAX_IMAGE_CELLS = 255
// F steps through these speeds
const SPEEDS = [1, 2, 4]
// Image blits refused in a row before the pane falls back to cells
const IMAGE_FAILURES_BEFORE_CELLS = 10

const ROM_EXTENSIONS = /\.(gb|gbc)$/i
const SLOTS = ['1', '2', '3', '4', '5', '6', '7', '8', '9']

const CONTROLS = '←↑↓→ move · Z is A · X is B · Enter is Start · Backspace is Select'

const HELP = [
  'ClaudeBoy plays a Game Boy game in a pane while Claude works, and saves it when you are handed back.',
  '',
  '  /claudeboy              turn it on',
  '  /claudeboy off          turn it off',
  '  /claudeboy play         play now, whether or not Claude is working',
  '  /claudeboy save <1-9>   save to a slot',
  '  /claudeboy load <1-9>   load a slot',
  '  /claudeboy slots        list the slots',
  '  /claudeboy rom [path]   list the ROMs found, or pick one by number or path',
  '  /claudeboy sound on|off',
  '  /claudeboy display auto|cells|image|window',
  '',
  'In the game: ' + CONTROLS + '. S saves to a slot, L loads one, M turns the sound on or off, F fast-forwards ×2, ×4, then back.',
].join('\n')

type Phase = 'idle' | 'waiting' | 'offered' | 'playing' | 'countdown'
type Display = 'auto' | 'cells' | 'image' | 'window'
type Game = { rom: string; key: string; title: string; isColor: boolean }
type Reply = { isOk: boolean; words: string }
type Command = { id: number; name: string; arg: string; resolve: (reply: Reply) => void; timer: Timer }
type Size = { columns: number; rows: number }
// Where the picture sits in the pane, in cells, and its size in pixels
type Room = Size & { left: number; width: number; height: number }
type Frame = { shm: string; format: 'rgba'; width: number; height: number }
type PadPost = { pad: string; held: number; taps: number[]; keys: { seq: number; key: string }[] }

// Settings, kept between sessions in $.store
let isOn = false
let romChoice: string | null = null
let isSoundOn = false
let display: Display = 'auto'

// Set at session start
let root = ''
let home = ''
let dataDir = ''
let runtimeDir = '/tmp'
let isImageTerminal = false
// There's a desktop to open the game's window on
let hasDesktop = false
// A cell's size in pixels, as the engine last read the terminal's; zero while
// the terminal doesn't say
let cell = { width: 0, height: 0 }
// The engine's estimate, from where it found the terminal on the screen
let isCellEstimated = false
// The terminal's width in cells, and the drawer's and the desktop's in pixels:
// how many columns the pane takes to make room for the drawer
let terminalColumns = 0
let drawer = { width: 0, workWidth: 0 }

let game: Game | null = null

// Where play stands:
//   idle      not playing, whether or not Claude is working
//   waiting   Claude is working; dropping in once the delay passes
//   offered   the terminal was too narrow for the pane to open by itself, so
//             the band above the prompt offers a key that opens it
//   playing   the pane is open and the game runs
//   countdown Claude is done; handing back when the count reaches zero, the
//             count on the game's screen. Esc keeps playing instead.
let phase: Phase = 'idle'
let isTurnRunning = false
// Closed by hand during this turn, so stay out until the next one
let isDismissed = false
// Opened with /claudeboy play: it stays open until closed by hand
let isOpenedByCommand = false
let isWelcomeOpen = false
let welcomeStatus: string | null = null
let timer: Timer | null = null
let countdown = 0

// The engine, the folder it and this module share, and what it was told
let engine: ReturnType<Dollar['process']['spawn']> | null = null
let engineExited: Promise<void> | null = null
let runDir: string | null = null
let engineError: string | null = null
// Another session has the game: asking whether to play it here
let isAskingTakeover = false
let awayTimer: Timer | null = null
let commands: Command[] = []
let nextCommandId = 1
let wanted = { isPlaying: false, keys: 0, taps: [0, 0, 0, 0, 0, 0, 0, 0], view: 'none', speed: 1, overlay: 'none', countdown: 0 }
let controlWrite: Promise<void> | null = null
let isControlDirty = false
// Set when play pauses, so the save that follows can be told
let toastWhenPaused: string | null = null
let isSavedSincePause = false

// The picture: the size the pane lays the game out at, the newest frame for
// it, and the size of the Raster or Image last drawn
let size: Size | null = null
let drawnSize: string | null = null
let cells: { size: string; text: string } | null = null
let frame: Frame | null = null
// The picture last handed to the terminal, and whether that's under way
let blittedFrame: Frame | null = null
let isBlitting = false
let announcedSeq = 0
let pulledSeq = 0
let isPulling = false
let imageFailures = 0
let isImageBroken = false
// The game's window couldn't open here, and whether it has the keyboard
let isWindowBroken = false
let hasWindowFocus = false

// The gamepad and the slot picker
let padId: string | null = null
let lastKeySeq = 0
let hasKeys = false
let picker: 'save' | 'load' | null = null
let slotTimes: Record<string, number> = {}
let message: string | null = null
let messageTimer: Timer | null = null
let now = 0

// ---------------------------------------------------------------------------
// Paths, the ROM and the engine's build

function enginePath(): string {
  return root + '/dist/claudeboy-engine'
}

function savesDir(of: Game): string {
  return dataDir + '/saves/' + of.key
}

function expandHome(path: string): string {
  return path === '~' || path.startsWith('~/') ? home + path.slice(1) : path
}

function prettyTitle(title: string): string {
  return title
    .toLowerCase()
    .split(' ')
    .filter(Boolean)
    .map((word) => word[0]?.toUpperCase() + word.slice(1))
    .join(' ')
}

async function findRoms($: Dollar): Promise<string[]> {
  const found: string[] = []
  for (const folder of [root, root + '/roms', dataDir + '/roms']) {
    try {
      const entries = await $.fs.list(folder)
      for (const entry of [...entries].sort((a, b) => a.name.localeCompare(b.name))) {
        if (entry.kind === 'file' && ROM_EXTENSIONS.test(entry.name)) found.push(folder + '/' + entry.name)
      }
    } catch {}
  }
  return found
}

// Reads the ROM's title and save key, building the engine first if need be.
// Returns what's wrong, or null when there's a game to play.
async function loadGame($: Dollar): Promise<string | null> {
  const built = await ensureEngine($)
  if (built) return built
  const choice = romChoice && (await $.fs.exists(romChoice)) ? romChoice : null
  const rom = choice ?? (await findRoms($))[0]
  if (!rom) {
    game = null
    return 'No ROM found. Put a .gb or .gbc file in ' + dataDir + '/roms, or pick one with /claudeboy rom <path>.'
  }
  if (game?.rom === rom) return null
  return readGame($, rom)
}

async function readGame($: Dollar, rom: string): Promise<string | null> {
  const { stdout } = await $.process.run([enginePath(), 'info', rom])
  try {
    const info = JSON.parse(stdout) as { title?: string; key?: string; color?: boolean; error?: string }
    if (info.error || !info.key) return "Can't play " + rom + ': ' + (info.error ?? 'unreadable')
    game = { rom, key: info.key, title: prettyTitle(info.title ?? '') || 'Game Boy', isColor: info.color === true }
    return null
  } catch {
    return "ClaudeBoy's emulator couldn't read " + rom
  }
}

let build: Promise<string | null> | null = null

// The engine is built from source once, by scripts/build.sh
function ensureEngine($: Dollar): Promise<string | null> {
  build ??= buildEngine($).finally(() => {
    build = null
  })
  return build
}

async function buildEngine($: Dollar): Promise<string | null> {
  if (await $.fs.exists(enginePath())) return null
  showWelcomeStatus($, 'Building the emulator, which happens once and takes about a minute…')
  try {
    const built = await $.process.run(['bash', root + '/scripts/build.sh'], { timeoutMs: 10 * 60 * 1000 })
    if (built.exitCode === 0 && (await $.fs.exists(enginePath()))) return null
    const lines = (built.stderr || built.stdout).trim().split('\n')
    return "Couldn't build the emulator: " + (lines.slice(-2).join(' ') || 'the build failed') + ' (see scripts/build.sh)'
  } catch (error) {
    return "Couldn't build the emulator: " + String(error)
  }
}

function showWelcomeStatus($: Dollar, text: string) {
  welcomeStatus = text
  redraw($)
}

// ---------------------------------------------------------------------------
// Dropping in and handing back

function cancelTimer() {
  timer?.cancel()
  timer = null
}

function armDropIn($: Dollar) {
  if (!isOn || !isTurnRunning || isDismissed || phase !== 'idle') return
  phase = 'waiting'
  timer = $.clock.after(DROP_IN_DELAY_MS, () => void dropIn($))
}

async function dropIn($: Dollar) {
  if (phase !== 'waiting') return
  timer = null
  const surfaces = await $.session.surfaces()
  if (!surfaces.includes('terminal') || (await loadGame($)) !== null || phase !== 'waiting') {
    if (phase === 'waiting') phase = 'idle'
    return
  }
  if (pictureMode() === 'window') {
    await openDrawerPane($)
    await startPlaying($)
    return
  }
  const opened = await openPane($)
  if (!opened.isPlaced) {
    // A waiting pane would pop up long after the moment has passed. One the
    // person opens appears at any width, so offer them a key instead.
    $.ui.log('claudeboy: the pane waits for a wider terminal: ' + opened.reason, { to: 'debug' })
    await $.ui.close({ id: PANE })
    phase = 'offered'
    redraw($)
    return
  }
  await startPlaying($)
}

function openPane($: Dollar) {
  return $.ui.open({ id: PANE, title: game?.title ?? 'ClaudeBoy', focus: true, closeOnEscape: true, columns: DOCK_COLUMNS, rows: INLINE_ROWS })
}

// The columns the drawer covers, when the terminal fills the desktop's width
function drawerColumns(): number {
  if (drawer.width <= 0 || drawer.workWidth <= 0 || terminalColumns <= 0) return DOCK_COLUMNS
  const columns = Math.ceil((drawer.width * terminalColumns) / drawer.workWidth) + 1
  // Where the engine lays the console over the pane instead (it found the
  // terminal's cells on the screen), wide enough for the wide console at twice
  // its size; the engine fits whichever shape shows the game largest
  if (!isCellEstimated) return columns
  return Math.max(columns, Math.ceil((2 * WIDE_CONSOLE_WIDTH) / cell.width) + 1)
}

// While the drawer is out, the pane opens beside the transcript as wide as
// the drawer, so the transcript moves aside rather than under it. It never
// takes the keys: they go to the drawer. Where it doesn't fit, it stays shut.
async function openDrawerPane($: Dollar, isFocused = false) {
  const opened = await $.ui.open({
    id: PANE,
    title: game?.title ?? 'ClaudeBoy',
    closeOnEscape: true,
    ...(isFocused ? { focus: true as const } : {}),
    columns: drawerColumns(),
    rows: STATUS_ROWS + 2,
  })
  if (!opened.isPlaced) await $.ui.close({ id: PANE })
}

async function acceptOffer($: Dollar) {
  if (phase !== 'offered') return
  const { isPlaced } = await openPane($)
  if (isPlaced) await startPlaying($)
}

async function startPlaying($: Dollar) {
  phase = 'playing'
  isWelcomeOpen = false
  picker = null
  awayTimer?.cancel()
  awayTimer = null
  wanted.isPlaying = true
  if (pictureMode() === 'window') {
    wanted.view = 'window'
    hasWindowFocus = false
  }
  redraw($)
  if (engine) scheduleControl($)
  else void runEngine($)
}

// Claude is done or needs the person before they took up the offer
function withdrawOffer($: Dollar) {
  cancelTimer()
  phase = 'idle'
  redraw($)
}

// Pauses the game, which saves it, and closes the pane; the toast says `why`
// once the game is saved. The ui.close hook doesn't hear this module's own
// closes, so the pausing happens here.
async function handBack($: Dollar, why: string) {
  goAway($, why)
  await $.ui.close({ id: PANE })
}

function startCountdown($: Dollar) {
  phase = 'countdown'
  countdown = COUNTDOWN_SECONDS
  wanted.countdown = countdown
  redraw($)
  scheduleControl($)
  timer = $.clock.every(1000, () => {
    countdown -= 1
    wanted.countdown = Math.max(0, countdown)
    if (countdown > 0) {
      redraw($)
      scheduleControl($)
    } else void handBack($, "Claude's done")
  })
}

// Esc during the countdown: stay in the game. The next Esc goes back to
// Claude, and the next turn's end counts down again.
function keepPlaying($: Dollar) {
  cancelTimer()
  phase = 'playing'
  wanted.countdown = 0
  // Not when a queued prompt started a turn: Claude is working again
  if (!isTurnRunning) showMessage($, 'Still playing · Esc goes back to Claude')
  redraw($)
  scheduleControl($)
}

// Leaving the pane, whoever closes it: pause, which saves the game
function goAway($: Dollar, why: string) {
  isAskingTakeover = false
  const wasPlaying = phase === 'playing' || phase === 'countdown'
  if (!wasPlaying && !isWelcomeOpen && !wanted.isPlaying) {
    cancelTimer()
    phase = 'idle'
    return
  }
  cancelTimer()
  phase = 'idle'
  isWelcomeOpen = false
  isOpenedByCommand = false
  picker = null
  forgetPicture()
  hasWindowFocus = false
  wanted = { ...wanted, isPlaying: false, keys: 0, view: 'none', speed: 1, countdown: 0 }
  if (!engine) return
  if (wasPlaying) {
    toastWhenPaused = why
    isSavedSincePause = false
  }
  scheduleControl($)
  awayTimer?.cancel()
  awayTimer = $.clock.after(AWAY_STOP_MS, () => void stopEngine($))
}

// Claude is about to ask the person something, so they must see the prompt
async function needsYou($: Dollar) {
  if (phase === 'waiting' || phase === 'offered') withdrawOffer($)
  else if (phase === 'playing' || phase === 'countdown') await handBack($, 'Claude needs you')
}

// ---------------------------------------------------------------------------
// The engine

async function runEngine($: Dollar) {
  if (engine || !game) return
  const playing = game
  const id = Math.random().toString(36).slice(2, 8)
  runDir = runtimeDir + '/claudeboy-' + id
  engineError = null
  announcedSeq = 0
  pulledSeq = 0
  let markExited = () => {}
  engineExited = new Promise((resolve) => {
    markExited = resolve
  })
  // Set before anything is awaited, so a caller finds it at once. The engine
  // stays paused until its control file says otherwise, and reads it every frame.
  const child = $.process.spawn({ argv: [enginePath(), 'run', playing.rom, savesDir(playing), runDir] })
  engine = child
  scheduleControl($)
  let pending = ''
  let stderr = ''
  try {
    for await (const { stream, text } of child) {
      if (stream === 'stderr') {
        stderr = (stderr + text).slice(-2000)
        continue
      }
      const lines = (pending + text).split('\n')
      pending = lines.pop() ?? ''
      for (const line of lines) onEngineLine($, line)
    }
  } catch (error) {
    engineError ??= "ClaudeBoy couldn't start its emulator"
    $.ui.log('claudeboy: the engine failed: ' + String(error), { to: 'debug' })
  } finally {
    if (engine === child) engine = null
    if (stderr) $.ui.log('claudeboy: the engine said: ' + stderr.trim(), { to: 'debug' })
    for (const command of commands) {
      command.timer.cancel()
      command.resolve({ isOk: false, words: 'the emulator stopped' })
    }
    commands = []
    const folder = runDir
    runDir = null
    markExited()
    if (folder?.startsWith(runtimeDir + '/claudeboy-')) void $.process.run(['rm', '-rf', folder]).catch(() => {})
  }
  // Ending on its own while someone plays means it failed or was refused;
  // while they're away, the next drop-in simply starts it again. It also ends
  // when this module unloads, and then there's nothing to close.
  if (phase === 'playing' || phase === 'countdown') {
    try {
      $.ui.toast(engineError ?? "ClaudeBoy's emulator stopped", { timeoutMs: 8000 })
      await handBack($, '')
    } catch {}
  }
}

function onEngineLine($: Dollar, line: string) {
  const [tag = '', ...words] = line.trim().split(' ')
  switch (tag) {
    case '@cells':
      announcedSeq = Math.max(announcedSeq, Number(words[0]) || 0)
      void pullCells($)
      break
    case '@frame':
      if (words[0]) showImageFrame($, { shm: words[0], format: 'rgba', width: Number(words[1]), height: Number(words[2]) })
      break
    case '@term': {
      // Resized, or another font: the picture is laid out anew
      const [columns = 0, rows = 0, width = 0, height = 0] = words.map(Number)
      // A terminal that doesn't say its cells' size keeps the engine's estimate
      if (columns > 0 && rows > 0 && width > 0 && height > 0) cell = { width: width / columns, height: height / rows }
      terminalColumns = columns
      redraw($)
      break
    }
    case '@cell':
      // Where the terminal fills the desktop, the engine tells its cells' size
      cell = { width: Number(words[0]) || 0, height: Number(words[1]) || 0 }
      isCellEstimated = cell.width > 0
      if (wanted.view === 'window' && (phase === 'playing' || phase === 'countdown')) void openDrawerPane($)
      redraw($)
      break
    case '@drawer':
      drawer = { width: Number(words[0]) || 0, workWidth: Number(words[1]) || 0 }
      // Sized now the drawer is: the pane makes room to match
      if (wanted.view === 'window' && (phase === 'playing' || phase === 'countdown')) void openDrawerPane($)
      break
    case '@done': {
      const index = commands.findIndex((command) => command.id === Number(words[0]))
      if (index < 0) break
      const [command] = commands.splice(index, 1)
      command?.timer.cancel()
      command?.resolve({ isOk: words[1] === 'ok', words: words.slice(2).join(' ') })
      break
    }
    case '@saved':
      if (words[0] === 'auto') isSavedSincePause = true
      if (words[0]) slotTimes[words[0]] = Number(words[1]) || 0
      break
    case '@paused':
      if (toastWhenPaused !== null) {
        const parts = [toastWhenPaused, isSavedSincePause ? (game?.title ?? 'Your game') + ' saved' : '']
        const text = parts.filter(Boolean).join(' · ')
        if (text) $.ui.toast(text)
        toastWhenPaused = null
      }
      break
    case '@busy':
      void onBusy($)
      break
    case '@playing':
      if (isAskingTakeover) {
        isAskingTakeover = false
        redraw($)
      }
      break
    case '@yielded':
      void leaveGame($, (game?.title ?? 'The game') + ' moved to another Claude Code session.')
      break
    case '@notyielded':
      void leaveGame($, "Couldn't take " + (game?.title ?? 'the game') + ' from the other session.')
      break
    case '@key':
      // Typed in the game's window: what a key over the pane's picture does
      if (words[0] && (phase === 'playing' || phase === 'countdown')) void onKey($, words[0])
      break
    case '@focus':
      hasWindowFocus = words[0] === '1'
      redraw($)
      break
    case '@closed':
      // Esc on the console during the countdown keeps playing; otherwise, or
      // when it was closed, as closing the pane
      if (phase === 'countdown') {
        keepPlaying($)
        break
      }
      if (isTurnRunning) isDismissed = true
      void handBack($, '')
      break
    case '@nowindow':
      void noWindow($, words.join(' '))
      break
    case '@error':
      engineError = words.join(' ')
      break
    case '@log':
      $.ui.log('claudeboy engine: ' + words.join(' '), { to: 'debug' })
      break
  }
}

// No window can open here (no desktop, no X11): the pane draws cells instead
async function noWindow($: Dollar, reason: string) {
  isWindowBroken = true
  $.ui.log('claudeboy: no game window (' + reason + '); playing in the pane', { to: 'debug' })
  if (phase !== 'playing' && phase !== 'countdown') return
  wanted.view = 'none'
  const opened = await openPane($)
  if (opened.isPlaced) redraw($)
  else await handBack($, '')
}

// Another session is playing this game right now: it has the saves until it
// pauses. The pane asks whether to play it here instead, with the keys, so 1
// and 0 or the arrows and Enter answer. Should that session pause first, the
// engine takes the game and the question goes.
async function onBusy($: Dollar) {
  if (phase !== 'playing' && phase !== 'countdown') return
  isAskingTakeover = true
  if (pictureMode() === 'window') await openDrawerPane($, true)
  else await openPane($)
  redraw($)
}

// Yes: take it. The other session saves and lets go at once.
async function takeOver($: Dollar) {
  isAskingTakeover = false
  redraw($)
  await sendCommand($, 'takeover')
}

// No: stay out for the rest of the turn
async function declineTakeover($: Dollar) {
  isAskingTakeover = false
  if (isTurnRunning) isDismissed = true
  await handBack($, '')
}

// The game moved: another session took it, or it couldn't be taken
async function leaveGame($: Dollar, toast: string) {
  isAskingTakeover = false
  if (isTurnRunning) isDismissed = true
  $.ui.toast(toast, { timeoutMs: 6000 })
  await handBack($, '')
  // Saved already, and no pause will come to say so
  toastWhenPaused = null
}

// What the engine reads every frame: one line, rewritten whole. Commands stay
// in it until the engine answers them.
function controlLine(): string {
  const fields = [
    'play=' + (wanted.isPlaying ? 1 : 0),
    'keys=' + wanted.keys,
    'taps=' + wanted.taps.join(','),
    'view=' + wanted.view,
    'fps=' + (wanted.view.startsWith('image') ? 60 : 30),
    'sound=' + (isSoundOn ? 1 : 0),
    'speed=' + wanted.speed,
  ]
  // The engine draws the count over the game's screen
  if (wanted.countdown > 0) fields.push('countdown=' + wanted.countdown)
  if (wanted.view === 'window') {
    fields.push('overlay=' + wanted.overlay)
    fields.push('title=' + base64(new TextEncoder().encode(windowTitle())))
  }
  if (commands.length > 0) {
    fields.push('cmds=' + commands.map((command) => command.id + ':' + command.name + (command.arg ? ':' + command.arg : '')).join(','))
  }
  return fields.join(' ') + '\n'
}

// One write at a time, the newest state last. Before the engine has its
// folder there's nothing to write to: the change waits for runEngine.
function scheduleControl($: Dollar) {
  isControlDirty = true
  if (controlWrite || !runDir) return
  controlWrite = writeControl($).finally(() => {
    controlWrite = null
    // A change that came in as the last write was finishing
    if (isControlDirty && runDir) scheduleControl($)
  })
}

async function writeControl($: Dollar) {
  while (isControlDirty && runDir) {
    isControlDirty = false
    try {
      await $.fs.write(runDir + '/control', controlLine())
    } catch (error) {
      $.ui.log("claudeboy: couldn't write the control file: " + String(error), { to: 'debug' })
    }
  }
}

function sendCommand($: Dollar, name: string, arg = ''): Promise<Reply> {
  if (!engine) return Promise.resolve({ isOk: false, words: "the emulator isn't running" })
  return new Promise((resolve) => {
    const id = nextCommandId++
    const timer = $.clock.after(COMMAND_TIMEOUT_MS, () => {
      commands = commands.filter((command) => command.id !== id)
      resolve({ isOk: false, words: "the emulator didn't answer" })
    })
    commands.push({ id, name, arg, resolve, timer })
    scheduleControl($)
  })
}

// Starts the engine, paused, for a command given while nobody plays
async function ensureRunning($: Dollar): Promise<string | null> {
  const problem = await loadGame($)
  if (problem) return problem
  if (!engine) void runEngine($)
  return engine ? null : (engineError ?? "ClaudeBoy's emulator didn't start")
}

async function stopEngine($: Dollar) {
  awayTimer?.cancel()
  awayTimer = null
  const child = engine
  if (!child) return
  // It saves and says goodbye on its own; ending the loop is the last resort
  void sendCommand($, 'quit')
  await Promise.race([engineExited, $.clock.sleep(1500)])
  if (engine === child) await child.return({ code: null, signal: null }).catch(() => {})
}

// ---------------------------------------------------------------------------
// The picture

function forgetPicture() {
  wanted.overlay = 'none'
  size = null
  drawnSize = null
  cells = null
  frame = null
  blittedFrame = null
  hasKeys = false
}

// Pictures in the pane where the terminal shows them; elsewhere the game's
// own window, on a desktop; text cells in the pane where neither works
function pictureMode(): 'cells' | 'image' | 'window' {
  if (display === 'cells' || display === 'image') return display
  if (display === 'window' && !isWindowBroken) return 'window'
  if (isImageTerminal && !isImageBroken) return 'image'
  return hasDesktop && !isWindowBroken ? 'window' : 'cells'
}

// Draws what changed: the pane, and the window's title through the engine
function redraw($: Dollar) {
  $.ui.invalidate('ui.render')
  if (wanted.view === 'window') scheduleControl($)
}

// What's going on, for the line under the picture or the window's title
function statusLine(): { text: string; isStrong: boolean } {
  if (phase === 'countdown') return { text: "Claude's done · back in " + countdown + ' · Esc keeps playing', isStrong: true }
  if (picker) return { text: (picker === 'save' ? 'Save to' : 'Load') + ' which slot? 1–9, any other key cancels', isStrong: true }
  if (message) return { text: message, isStrong: false }
  if (wanted.view === 'window' ? !hasWindowFocus : !hasKeys) {
    return { text: wanted.view === 'window' ? 'Click here to play' : 'Click the game to play · Esc goes back to Claude', isStrong: true }
  }
  const doing = isOpenedByCommand ? 'Playing' : isTurnRunning ? 'Claude is working' : "Claude's done · Esc goes back"
  return { text: doing + ' · ' + CONTROLS, isStrong: false }
}

function hintLine(): string {
  if (picker) return SLOTS.map((slot) => slot + ' ' + (slotTimes[slot] ? ago(slotTimes[slot]) : '–')).join(' · ')
  if (wanted.view === 'window') {
    // The drawer's line is short: X's font is wide
    return 'S save · L load · M sound ' + (isSoundOn ? 'off' : 'on') + ' · F ' + (nextSpeed() > 1 ? '×' + nextSpeed() : '×1') + ' · Esc back'
  }
  return 'S save · L load · M sound ' + (isSoundOn ? 'off' : 'on') + ' · F ' + (nextSpeed() > 1 ? 'speed ×' + nextSpeed() : 'normal speed')
}

// The engine's run folder's name, drawn in the pane under the game window:
// the engine finds the terminal tab whose screen shows it, to show the window
// only while that tab is in front
function marker(): string {
  return runDir ? runDir.slice(runDir.lastIndexOf('/') + 1) : ''
}

// The drawer's two lines under the game: what's going on, then what keys do
function windowTitle(): string {
  const status = statusLine().text.replace(' · ' + CONTROLS, '')
  return (game?.title ?? 'ClaudeBoy') + ' · ' + status + '\n' + hintLine()
}

// The biggest picture that fits above the status lines at the screen's own
// shape, in the middle of the pane. Its shape in cells follows from a cell's
// in pixels: a half-block cell holds two of the screen's pixels, one above the
// other, and a picture is drawn at the box's size, its leftover strip clear.
function layout(bodyColumns: number, bodyRows: number, mode: 'cells' | 'image'): Room {
  const { width: cellWidth, height: cellHeight } = cell.width > 0 ? cell : DEFAULT_CELL
  const maxColumns = Math.max(8, Math.min(bodyColumns, mode === 'image' ? MAX_IMAGE_CELLS : SCREEN_WIDTH))
  const maxRows = Math.max(4, Math.min(bodyRows - STATUS_ROWS, MAX_IMAGE_CELLS))
  // Rows the picture takes for a given width, at the screen's shape
  const rowsFor = (columns: number) => (columns * cellWidth * SCREEN_HEIGHT) / (SCREEN_WIDTH * cellHeight)
  let columns = Math.min(maxColumns, Math.floor((maxRows * cellHeight * SCREEN_WIDTH) / (SCREEN_HEIGHT * cellWidth)))
  columns = Math.max(8, columns)
  const rows = Math.max(4, Math.min(maxRows, mode === 'image' ? Math.ceil(rowsFor(columns)) : Math.round(rowsFor(columns))))
  return {
    columns,
    rows,
    left: Math.max(0, Math.floor((bodyColumns - columns) / 2)),
    width: Math.round(columns * cellWidth),
    height: Math.round(rows * cellHeight),
  }
}

// The engine announces each new cells file; this reads the newest and paints
// it, skipping any that came and went while a paint was under way
async function pullCells($: Dollar) {
  if (isPulling) return
  isPulling = true
  try {
    while (pulledSeq < announcedSeq && runDir) {
      pulledSeq = announcedSeq
      let text: string
      try {
        text = await $.fs.read(runDir + '/cells')
      } catch {
        break
      }
      const newline = text.indexOf('\n')
      const [columns, rows, seq] = text.slice(0, newline).split(' ').map(Number)
      pulledSeq = Math.max(pulledSeq, seq ?? 0)
      if (!size || columns !== size.columns || rows !== size.rows) continue
      cells = { size: 'cells:' + columns + 'x' + rows, text: text.slice(newline + 1) }
      if (drawnSize === cells.size) {
        await $.ui.blit({ requestId: PANE, key: 'view', cells: cells.text }).catch(() => ({}))
      } else {
        redraw($)
      }
    }
  } finally {
    isPulling = false
  }
}

function showImageFrame($: Dollar, next: Frame) {
  frame = next
  if (drawnSize !== 'image') {
    redraw($)
    return
  }
  void blitFrames($)
}

// Hands the terminal the newest picture, one at a time: a frame that came and
// went while one was under way is skipped, so a terminal that falls behind
// gets the present, not a backlog. The engine removes frames nobody read.
async function blitFrames($: Dollar) {
  if (isBlitting) return
  isBlitting = true
  try {
    while (frame && frame !== blittedFrame && drawnSize === 'image') {
      const next = frame
      blittedFrame = next
      const result = await $.ui.blit({ requestId: PANE, key: 'view', source: next }).catch(() => ({ deny: 'failed' }))
      if (!('deny' in result) || !result.deny) {
        imageFailures = 0
        continue
      }
      imageFailures += 1
      // This terminal shows the alt text, not pictures: draw cells instead
      if (imageFailures >= IMAGE_FAILURES_BEFORE_CELLS && display === 'auto') {
        isImageBroken = true
        $.ui.log('claudeboy: pictures refused (' + result.deny + '); drawing cells', { to: 'debug' })
        redraw($)
        break
      }
    }
  } finally {
    isBlitting = false
  }
}

const BASE64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

// Padded base64, as a Raster's cells are written
function base64(bytes: Uint8Array): string {
  let text = ''
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i] ?? 0
    const b = bytes[i + 1] ?? 0
    const c = bytes[i + 2] ?? 0
    text += BASE64[a >> 2]
    text += BASE64[((a & 3) << 4) | (b >> 4)]
    text += i + 1 < bytes.length ? BASE64[((b & 15) << 2) | (c >> 6)] : '='
    text += i + 2 < bytes.length ? BASE64[c & 63] : '='
  }
  return text
}

// Spaces in the terminal's own colors, for a pane resized before its next frame
function blankCells({ columns, rows }: Size): string {
  const words = new Uint32Array(columns * rows * 3)
  for (let i = 0; i < words.length; i += 3) {
    words[i] = 0x20
    words[i + 1] = 0x01000000
    words[i + 2] = 0x01000000
  }
  return base64(new Uint8Array(words.buffer))
}

// ---------------------------------------------------------------------------
// Keys and slots

function showMessage($: Dollar, text: string) {
  message = text
  messageTimer?.cancel()
  messageTimer = $.clock.after(MESSAGE_MS, () => {
    message = null
    redraw($)
  })
  redraw($)
}

async function readSlotTimes($: Dollar): Promise<Record<string, number>> {
  const times: Record<string, number> = {}
  if (!game) return times
  try {
    for (const entry of await $.fs.list(savesDir(game))) {
      const match = /^(?:slot([1-9])|(auto))\.state$/.exec(entry.name)
      if (match) times[match[1] ?? 'auto'] = entry.mtimeMs
    }
  } catch {}
  return times
}

function ago(at: number): string {
  const seconds = Math.max(0, Math.round((now - at) / 1000))
  if (seconds < 60) return 'just now'
  if (seconds < 3600) return Math.floor(seconds / 60) + 'm ago'
  if (seconds < 86400) return Math.floor(seconds / 3600) + 'h ago'
  return Math.floor(seconds / 86400) + 'd ago'
}

async function onKey($: Dollar, key: string) {
  if (picker) {
    const action = picker
    picker = null
    if (SLOTS.includes(key)) void useSlot($, action, key)
    else redraw($)
    return
  }
  if (key === 's' || key === 'l') {
    picker = key === 's' ? 'save' : 'load'
    slotTimes = await readSlotTimes($)
    now = await $.clock.now()
    redraw($)
  } else if (key === 'm') {
    isSoundOn = !isSoundOn
    await $.store.set('sound', isSoundOn)
    scheduleControl($)
    showMessage($, isSoundOn ? 'Sound on' : 'Sound off')
  } else if (key === 'f') {
    wanted.speed = nextSpeed()
    scheduleControl($)
    showMessage($, wanted.speed > 1 ? 'Fast-forward ×' + wanted.speed : 'Normal speed')
  }
}

// The speed after this one: ×1, ×2, ×4, then ×1 again
function nextSpeed(): number {
  return SPEEDS[(SPEEDS.indexOf(wanted.speed) + 1) % SPEEDS.length] ?? 1
}

async function useSlot($: Dollar, action: 'save' | 'load', slot: string) {
  const reply = await sendCommand($, action, slot)
  if (reply.isOk) showMessage($, action === 'save' ? 'Saved to slot ' + slot : 'Loaded slot ' + slot)
  else showMessage($, "Couldn't " + action + ' slot ' + slot + ': ' + reply.words)
}

// ---------------------------------------------------------------------------
// The command

async function turnOn($: Dollar) {
  isOn = true
  await $.store.set('isOn', true)
  if (phase === 'idle') {
    isWelcomeOpen = true
    welcomeStatus = null
    await $.ui.open({ id: PANE, title: 'ClaudeBoy' })
  }
  void (async () => {
    const problem = await loadGame($)
    showWelcomeStatus($, problem ?? 'Ready to play ' + (game?.title ?? 'your game') + '.')
  })()
  return {}
}

async function turnOff($: Dollar) {
  isOn = false
  await $.store.set('isOn', false)
  if (phase === 'waiting' || phase === 'offered') withdrawOffer($)
  else if (phase !== 'idle' || isWelcomeOpen) await handBack($, '')
  await stopEngine($)
  return { text: 'ClaudeBoy is off. Your game is saved.' }
}

async function playNow($: Dollar) {
  const problem = await loadGame($)
  if (problem) return { text: problem }
  isOpenedByCommand = true
  if (phase === 'playing' || phase === 'countdown') {
    cancelTimer()
    phase = 'playing'
    redraw($)
    return {}
  }
  cancelTimer()
  phase = 'idle'
  if (pictureMode() === 'window') {
    isWelcomeOpen = false
    await openDrawerPane($)
    await startPlaying($)
    return {}
  }
  isWelcomeOpen = false
  const opened = await openPane($)
  if (!opened.isPlaced) {
    isOpenedByCommand = false
    return { text: "There's no room for the pane: " + opened.reason }
  }
  await startPlaying($)
  return {}
}

async function slotCommand($: Dollar, action: 'save' | 'load', slot: string | undefined) {
  if (!slot || !SLOTS.includes(slot)) return { text: 'Which slot? /claudeboy ' + action + ' <1-9>' }
  const problem = await ensureRunning($)
  if (problem) return { text: problem }
  const reply = await sendCommand($, action, slot)
  if (!reply.isOk) return { text: "Couldn't " + action + ' slot ' + slot + ': ' + reply.words + '.' }
  // Loaded while nobody plays: that's where the next drop-in starts
  return { text: (action === 'save' ? 'Saved to slot ' : 'Loaded slot ') + slot + ' of ' + game?.title + '.' }
}

async function listSlots($: Dollar) {
  const problem = await loadGame($)
  if (problem || !game) return { text: problem ?? 'No game.' }
  const times = await readSlotTimes($)
  now = await $.clock.now()
  const lines = SLOTS.map((slot) => '  ' + slot + '  ' + (times[slot] ? ago(times[slot]) : 'empty'))
  const auto = times.auto ? 'autosaved ' + ago(times.auto) : 'not autosaved yet'
  return { text: game.title + ' (' + auto + '):\n' + lines.join('\n') + '\n\nIn the game, S saves to a slot and L loads one.' }
}

async function chooseRom($: Dollar, choice: string) {
  const built = await ensureEngine($)
  if (built) return { text: built }
  const roms = await findRoms($)
  if (!choice) {
    const current = game?.rom ?? romChoice
    const lines = roms.map((rom, i) => '  ' + (i + 1) + '  ' + rom + (rom === current ? '  (playing)' : ''))
    return {
      text:
        (roms.length ? 'ROMs found:\n' + lines.join('\n') : 'No ROMs found.') +
        '\n\nPick one with /claudeboy rom <number or path>. ClaudeBoy looks in ' +
        dataDir +
        '/roms and the plugin folder.',
    }
  }
  const rom = /^\d+$/.test(choice) ? roms[Number(choice) - 1] : expandHome(choice)
  if (!rom || !(await $.fs.exists(rom))) return { text: 'No ROM at ' + (rom ?? 'number ' + choice) + '.' }
  // The game being played is saved before another takes its place
  if (phase !== 'idle') await handBack($, '')
  await stopEngine($)
  const before = game
  const problem = await readGame($, rom)
  if (problem) {
    game = before
    return { text: problem }
  }
  romChoice = rom
  await $.store.set('rom', rom)
  return { text: 'ClaudeBoy plays ' + game?.title + ' now.' }
}

async function setSound($: Dollar, value: string) {
  if (value !== 'on' && value !== 'off') return { text: 'Sound is ' + (isSoundOn ? 'on' : 'off') + '. /claudeboy sound on|off' }
  isSoundOn = value === 'on'
  await $.store.set('sound', isSoundOn)
  scheduleControl($)
  return { text: 'Sound ' + value + '.' }
}

async function setDisplay($: Dollar, value: string) {
  if (value !== 'auto' && value !== 'cells' && value !== 'image' && value !== 'window') {
    return { text: 'Display is ' + display + ' (' + pictureMode() + ' here). /claudeboy display auto|cells|image|window' }
  }
  display = value
  isImageBroken = false
  isWindowBroken = false
  await $.store.set('display', display)
  redraw($)
  return { text: 'Display ' + value + '.' }
}

// ---------------------------------------------------------------------------

export const register: Register = (on) => {
  on('session.start', async ($, e, next) => {
    root = $.plugin.root
    home = (await $.env.get('HOME')) ?? ''
    // Saves and ROMs: CLAUDEBOY_DATA_DIR, or claudeboy in the XDG data folder
    dataDir = (await $.env.get('CLAUDEBOY_DATA_DIR')) || ((await $.env.get('XDG_DATA_HOME')) || home + '/.local/share') + '/claudeboy'
    runtimeDir = (await $.env.get('XDG_RUNTIME_DIR')) || (await $.env.get('TMPDIR')) || '/tmp'
    // kitty and Ghostty show pictures; tmux passes none through
    const term = (await $.env.get('TERM')) ?? ''
    const program = (await $.env.get('TERM_PROGRAM')) ?? ''
    isImageTerminal =
      (await $.env.get('CLAUDE_CODE_FORCE_TERMINAL_IMAGES')) === '1' ||
      (!(await $.env.get('TMUX')) &&
        (term === 'xterm-kitty' || term === 'xterm-ghostty' || program === 'ghostty' || !!(await $.env.get('KITTY_WINDOW_ID'))))
    hasDesktop = !!(await $.env.get('DISPLAY'))
    isOn = (await $.store.get('isOn')) === true
    const rom = await $.store.get('rom')
    romChoice = typeof rom === 'string' ? rom : null
    // Off until the person turns it on: the game opens unasked, often at work
    isSoundOn = (await $.store.get('sound')) === true
    const shown = await $.store.get('display')
    display = shown === 'cells' || shown === 'image' || shown === 'window' ? shown : 'auto'
    await $.command.register({
      name: 'claudeboy',
      description: 'Play a Game Boy game while Claude works',
      argumentHint: '[play|off|save <1-9>|load <1-9>|slots|rom [path]|sound on|off|display auto|cells|image|help]',
      immediate: true,
    })
    return next(e)
  })

  on('session.end', async ($, e, next) => {
    wanted.isPlaying = false
    await stopEngine($)
    return next(e)
  })

  on('command.run', { command: 'claudeboy' }, async ($, e) => {
    const args = e.args.trim()
    const [verb = '', first] = args.split(/\s+/)
    const rest = args.slice(verb.length).trim()
    switch (verb.toLowerCase()) {
      case '':
      case 'on':
        return turnOn($)
      case 'off':
        return turnOff($)
      case 'play':
        return playNow($)
      case 'save':
      case 'load':
        return slotCommand($, verb.toLowerCase() as 'save' | 'load', first)
      case 'slots':
        return listSlots($)
      case 'rom':
        return chooseRom($, rest)
      case 'sound':
        return setSound($, (first ?? '').toLowerCase())
      case 'display':
        return setDisplay($, (first ?? '').toLowerCase())
      default:
        return { text: HELP }
    }
  })

  on('turn.start', async ($, e, next) => {
    // A subagent's turns come and go inside the main one
    if (e.agentId) return next(e)
    isTurnRunning = true
    isDismissed = false
    // On or off as /claudeboy last left it in any session
    isOn = (await $.store.get('isOn')) === true
    if (isWelcomeOpen) {
      isWelcomeOpen = false
      await $.ui.close({ id: PANE })
    }
    if (phase === 'countdown') {
      // A queued prompt started straight away, so keep playing
      keepPlaying($)
    }
    armDropIn($)
    return next(e)
  })

  on('turn.complete', async ($, e, next) => {
    if (e.agentId) return next(e)
    isTurnRunning = false
    if (phase === 'waiting' || phase === 'offered') {
      withdrawOffer($)
    } else if (phase === 'playing' && !isOpenedByCommand) {
      if (e.isAborted) await handBack($, '')
      else startCountdown($)
    }
    return next(e)
  })

  // A permission dialog is about to ask the person. In auto mode most asks go
  // to the classifier instead, and this never fires for those: play goes on.
  on('classic.PermissionRequest', async ($, e, next) => {
    await needsYou($)
    return next(e)
  })

  on('tool.call', async ($, e, next) => {
    // These put a question or a plan to the person whatever the mode
    if (e.tool === 'AskUserQuestion' || e.tool === 'ExitPlanMode') await needsYou($)
    const result = await next(e)
    // Once an answered prompt lets Claude carry on, drop back in
    armDropIn($)
    return result
  })

  // The person closed the pane (this module's own closes go through handBack)
  on('ui.close', async ($, e, next) => {
    if (e.id !== PANE) return next(e)
    // Esc during the countdown keeps playing: the pane stays open
    if (e.origin?.kind === 'person' && phase === 'countdown') {
      keepPlaying($)
      // Opened again, so the next Esc closes it: a refused close spends it
      if (pictureMode() === 'window') await openDrawerPane($)
      else await openPane($)
      return { value: undefined }
    }
    if (e.origin?.kind !== 'person') {
      goAway($, '')
      return next(e)
    }
    // The person's close (Esc, the close mark) is refused and the pane closed
    // as this plugin's own: Claude Code places a pane opened unasked on a
    // terminal under 144 columns only if the person hasn't closed it by hand,
    // so a close of theirs would stop the game opening by itself
    if (isTurnRunning && !isWelcomeOpen) isDismissed = true
    isWelcomeOpen = false
    goAway($, '')
    $.clock.after(0, () => void $.ui.close({ id: PANE }))
    return { value: undefined }
  })

  on('ui.message', async ($, e) => {
    if (e.element !== 'pad') return {}
    const post = e.data as PadPost
    if (post.pad !== padId) {
      // A new gamepad counts its keys from one again
      padId = post.pad
      lastKeySeq = 0
    }
    if (!hasKeys && (post.held !== 0 || post.keys.length > 0 || post.taps.some((count) => count > 0))) {
      hasKeys = true
      redraw($)
    }
    wanted.keys = phase === 'playing' || phase === 'countdown' ? post.held : 0
    wanted.taps = post.taps
    scheduleControl($)
    for (const { seq, key } of post.keys) {
      if (seq <= lastKeySeq) continue
      lastKeySeq = seq
      await onKey($, key)
    }
    return {}
  })

  on('ui.render', { component: 'AbovePrompt' }, async ($, e, next) => {
    if (phase !== 'offered') return next(e)
    const { Box, Button } = $.ui.resolve(e)
    // Keep whatever other mods show in the band
    const others = await next(e)
    return (
      <Box flexDirection="column">
        <Button
          key="play"
          label={'Play ' + (game?.title ?? 'ClaudeBoy') + ' while Claude works'}
          hotkey="1"
          plain
          onPress={() => acceptOffer($)}
        />
        {others}
      </Box>
    )
  })

  on('ui.render', { component: 'Pane', requestId: PANE }, async ($, e) => {
    if (e.surface !== 'terminal') {
      const { Text } = $.ui.resolve(e)
      return <Text>ClaudeBoy plays in the terminal.</Text>
    }
    const { Box, Text, Button, Raster, Image, Client } = $.ui.resolve(e)

    if (isWelcomeOpen) {
      return (
        <Box flexDirection="column" gap={1}>
          <Text bold>ClaudeBoy is on</Text>
          <Text>
            When Claude has been working for two seconds, your Game Boy game opens: here in kitty or Ghostty, in a window
            of its own in other terminals. You're handed back when Claude is done or needs you, and the game is saved
            each time.
          </Text>
          <Text>{CONTROLS}. S saves to a slot, L loads one, M turns the sound on or off, F fast-forwards ×2, ×4, then back.</Text>
          {welcomeStatus ? <Text color={welcomeStatus.startsWith('Ready') ? 'green' : undefined}>{welcomeStatus}</Text> : null}
          <Text dimColor>/claudeboy play opens it now · /claudeboy help lists the rest · /claudeboy off turns it off</Text>
          <Button
            key="got-it"
            label="Got it"
            autoFocus
            onPress={async () => {
              isWelcomeOpen = false
              await $.ui.close({ id: PANE })
            }}
          />
        </Box>
      )
    }

    if (engineError && !engine) return <Text>{engineError}</Text>

    if (isAskingTakeover) {
      return (
        <Box flexDirection="column" gap={1}>
          <Text>{(game?.title ?? 'This game') + ' is being played in another Claude Code session.'}</Text>
          <Text bold>Would you like to play it here?</Text>
          <Box flexDirection="column">
            <Button key="takeover-yes" label="Yes" hotkey="1" plain autoFocus onPress={() => takeOver($)} />
            <Button key="takeover-no" label="No" hotkey="0" plain onPress={() => declineTakeover($)} />
          </Box>
          <Text dimColor>1 or 0, or ↑↓ and Enter</Text>
        </Box>
      )
    }

    const mode = pictureMode()
    if (mode === 'window') {
      // The engine lays the console over this pane's body above the status,
      // so it looks part of the terminal; the pane draws the frame and the
      // status. Where it can't (another layout, a terminal not filling the
      // desktop) the console is in the drawer at the screen's edge.
      const boxRows = Math.max(4, e.props.scroll.bodyRows - STATUS_ROWS)
      const overlay =
        e.props.placement === 'dock' && terminalColumns > 0
          ? [terminalColumns - e.props.bodyColumns, 1, e.props.bodyColumns, boxRows].join(',')
          : 'none'
      if (overlay !== wanted.overlay) {
        wanted.overlay = overlay
        scheduleControl($)
      }
      const { text, isStrong } = statusLine()
      return (
        <Box flexDirection="column">
          {overlay !== 'none' ? (
            <Box height={boxRows} alignItems="center" justifyContent="center">
              <Text dimColor>{marker()}</Text>
            </Box>
          ) : (
            <Text dimColor>{(game?.title ?? 'The game') + ' is in the drawer → ' + marker()}</Text>
          )}
          <Box flexDirection="column" alignItems={overlay !== 'none' ? 'center' : 'flex-start'}>
            <Text bold={isStrong} wrap="truncate-end">
              {text === 'Click here to play' ? 'Click the game to play' : text}
            </Text>
            <Text dimColor wrap="truncate-end">
              {hintLine()}
            </Text>
          </Box>
        </Box>
      )
    }
    const room = layout(e.props.bodyColumns, e.props.scroll.bodyRows, mode)
    const view = mode === 'image' ? 'image:' + room.width + 'x' + room.height : 'cells:' + room.columns + 'x' + room.rows
    size = room
    // A new view makes the engine send a frame at once, still picture or not
    if (wanted.view !== view) {
      wanted.view = view
      scheduleControl($)
    }

    // Until a frame of the new size comes, a resized pane shows a blank one
    const picture =
      mode === 'image'
        ? frame && <Image key="view" source={frame} columns={room.columns} rows={room.rows} alt={game?.title ?? 'Game Boy'} />
        : cells && (
            <Raster key="view" columns={room.columns} rows={room.rows} cells={cells.size === view ? cells.text : blankCells(room)} />
          )
    drawnSize = picture ? (mode === 'image' ? 'image' : view) : null
    if (!picture) return <Text dimColor>Starting {game?.title ?? 'the game'}…</Text>

    const { text: status, isStrong } = statusLine()
    const hint = hintLine()

    return (
      <Box flexDirection="column">
        <Box marginLeft={room.left}>{picture}</Box>
        {/* Laid over the picture, so keys and clicks land on the game */}
        <Box position="absolute" top={0} left={room.left}>
          <Client key="pad" module="./gamepad.tsx" width={room.columns} height={room.rows} />
        </Box>
        <Box marginLeft={room.left} flexDirection="column">
          <Text bold={isStrong} wrap="truncate-end">
            {status}
          </Text>
          <Text dimColor wrap="truncate-end">
            {hint}
          </Text>
        </Box>
      </Box>
    )
  })
}
