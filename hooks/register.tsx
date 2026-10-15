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
// In terminals without pictures, on a desktop, the engine draws the game on a
// console of its own instead, sharp: laid over the pane where the terminal
// fills the desktop, else in a drawer at the screen's right edge, the pane
// opening as wide so the transcript moves aside rather than under it.

import type { EngineInterface, Register, Timer } from 'claude-code'

type Dollar = EngineInterface

const PANE = 'claudeboy'
const DROP_IN_DELAY_MS = 2000
const COUNTDOWN_SECONDS = 3
// After a hand-back the engine waits, paused, this long before it quits
const AWAY_STOP_MS = 10 * 60 * 1000
const COMMAND_TIMEOUT_MS = 5000
// How long a message ("Saved to slot 2") shows in the status line
const MESSAGE_MS = 2500
// The dock's width asked for when the pane opens; the person's own wins
const DOCK_COLUMNS = 100
const INLINE_ROWS = 40
// The status line, then up to three rows of hints
const STATUS_ROWS = 4

const SCREEN_WIDTH = 160
const SCREEN_HEIGHT = 144
// The engine's console, in its own pixels: CONSOLE_WIDTH in engine/console.h
const CONSOLE_WIDTH = 272
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

// The Game Boy's buttons. Shift works only on the console: terminals don't
// report Shift alone.
const CONTROLS = 'the arrows, Z for A, X for B, Enter for Start, Backspace (or Shift on the console) for Select'
// ClaudeBoy's own keys, for the help and the welcome
const GAME_KEYS = 'S: save · L: load · O: open · G: game library · M: sound · F: speed ×1, ×2, ×4 · Esc: exit'

const HELP = [
  'ClaudeBoy plays a Game Boy game in a pane while Claude works, and saves it when you are handed back.',
  '',
  '  /claudeboy              turn it on',
  '  /claudeboy off          turn it off',
  '  /claudeboy play         play now, whether or not Claude is working',
  '  /claudeboy save <1-9>   save to a slot',
  '  /claudeboy load <1-9>   load a slot',
  '  /claudeboy slots        list the slots',
  '  /claudeboy open [path]  open a game (.gb, .gbc); without a path, pick one',
  '  /claudeboy games [n]    list your game library, or play game n',
  '  /claudeboy sound on|off',
  '  /claudeboy display auto|cells|image|window',
  '  /claudeboy help         list these',
  '',
  'In the game: ' + GAME_KEYS + '.',
  "The Game Boy's buttons: " + CONTROLS + '.',
].join('\n')

type Phase = 'idle' | 'waiting' | 'offered' | 'playing' | 'countdown'
type Display = 'auto' | 'cells' | 'image' | 'window'
type Game = { rom: string; key: string; title: string }
// A game the person opened, in their library
type LibraryGame = { rom: string; title: string }
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
// The games opened so far, in the order opened; whether the store kept one
let library: LibraryGame[] = []
let isLibraryKept = false
// Whether ClaudeBoy was used before the library: its ROM goes in it
let wasUsed = false
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
// The engine found the terminal's grid of cells on the screen (and `cell`
// is its estimate): it lays the console over the pane
let hasGrid = false
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
// The engine stopEngine is ending: its exit is no failure, and it isn't
// played on any more
let quittingEngine: ReturnType<Dollar['process']['spawn']> | null = null
// stopEngine's own end: the engine gone, or given up on
let engineStopped: Promise<void> | null = null
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
let drawnView: string | null = null
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
// The gamepad's taps, all buttons together, as last posted
let lastTaps = 0
let hasKeys = false
let picker: 'save' | 'load' | null = null
// The pane's menus over the game, paused meanwhile: opening a game (a path,
// or one found on this computer) or the library; what went wrong in it; and
// whether to play once a game is opened (/claudeboy play with none)
let menu: 'open' | 'games' | null = null
let menuError: string | null = null
let foundRoms: string[] = []
let isPlayAfterOpen = false
// Claude finished while a menu was up over the game: counted down after
let isTurnOverUnderMenu = false
// The slots' times for the picker, and when they were read
let slotTimes: Record<string, number> = {}
let slotsReadAt = 0
let message: string | null = null
let messageTimer: Timer | null = null

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

// The Game Boy games in these folders, each folder's in name order, the
// library's left out
async function romsIn($: Dollar, folders: string[], isLibraryLeftOut = false): Promise<string[]> {
  const found: string[] = []
  for (const folder of folders) {
    try {
      const entries = await $.fs.list(folder)
      for (const entry of [...entries].sort((a, b) => a.name.localeCompare(b.name))) {
        const rom = folder + '/' + entry.name
        if (entry.kind !== 'file' || !ROM_EXTENSIONS.test(entry.name) || found.includes(rom)) continue
        if (!isLibraryLeftOut || !library.some((kept) => kept.rom === rom)) found.push(rom)
      }
    } catch {}
  }
  return found
}

// Where ClaudeBoy found its game before there was a library
function findRoms($: Dollar): Promise<string[]> {
  return romsIn($, [root, root + '/roms', dataDir + '/roms'])
}

const NO_GAME = 'No game yet: run /claudeboy open to pick one, or /claudeboy open <path to a .gb or .gbc file>.'

// The game to play: the one last chosen from the library, read for its title
// and save key, the engine built first if need be. Returns what's wrong, or
// null when there's a game to play.
async function loadGame($: Dollar): Promise<string | null> {
  const built = await ensureEngine($)
  if (built) return built
  await keepFirstGame($)
  const entry = library.find((kept) => kept.rom === romChoice) ?? library[0]
  if (!entry) {
    game = null
    return NO_GAME
  }
  if (game?.rom === entry.rom) return null
  const read = await readGame($, entry.rom)
  if (typeof read === 'string') return read
  game = read
  return null
}

// From before the library: the game ClaudeBoy played becomes its first entry
let firstGame: Promise<void> | null = null

function keepFirstGame($: Dollar): Promise<void> {
  firstGame ??= moveFirstGame($).catch(() => {})
  return firstGame
}

async function moveFirstGame($: Dollar) {
  if (isLibraryKept) return
  isLibraryKept = true
  if (!wasUsed) return
  const rom = romChoice && (await $.fs.exists(romChoice)) ? romChoice : (await findRoms($))[0]
  if (!rom) return
  const read = await readGame($, rom)
  if (typeof read === 'string') return
  library = [{ rom, title: read.title }]
  await $.store.set('library', library)
}

async function readGame($: Dollar, rom: string): Promise<Game | string> {
  try {
    const { stdout } = await $.process.run([enginePath(), 'info', rom])
    const info = JSON.parse(stdout) as { title?: string; key?: string; error?: string }
    if (info.error || !info.key) return "Can't play " + rom + ': ' + (info.error ?? 'unreadable')
    return { rom, key: info.key, title: prettyTitle(info.title ?? '') || 'Game Boy' }
  } catch {
    return "ClaudeBoy's emulator couldn't read " + rom
  }
}

// Opens a game: added to the library, and played from here on, each game
// with saves of its own. The one being played is saved first.
async function switchGame($: Dollar, path: string): Promise<string | null> {
  const built = await ensureEngine($)
  if (built) return built
  let rom = expandHome(path.trim().replace(/^(['"])(.*)\1$/, '$2'))
  // Kept in the library as the file it names now, wherever a later session runs
  if (!rom.startsWith('/')) rom = (await $.session.cwd()).replace(/\/$/, '') + '/' + rom
  if (!ROM_EXTENSIONS.test(rom)) return 'That isn\'t a Game Boy game: open a .gb or .gbc file.'
  if (!(await $.fs.exists(rom))) return 'No file at ' + rom + '.'
  const read = await readGame($, rom)
  if (typeof read === 'string') return read
  await keepFirstGame($)
  if (!library.some((kept) => kept.rom === rom)) {
    library = [...library, { rom, title: read.title }]
    await $.store.set('library', library)
  }
  romChoice = rom
  await $.store.set('rom', rom)
  if (game?.rom === rom) return null
  // The engine plays one game: the old one saves as it ends, a new one starts
  await stopEngine($)
  forgetPicture()
  game = read
  return null
}

let build: Promise<string | null> | null = null

// The engine is built from source by scripts/build.sh: the first time, and
// again when its source is newer
function ensureEngine($: Dollar): Promise<string | null> {
  build ??= buildEngine($).finally(() => {
    build = null
  })
  return build
}

// An update that wouldn't build is tried once a session, not at every drop-in
let isUpdateFailed = false
// Why a drop-in couldn't play, as last told
let toldProblem: string | null = null

async function buildEngine($: Dollar): Promise<string | null> {
  const hasEngine = await $.fs.exists(enginePath())
  if (hasEngine && (isUpdateFailed || !(await isEngineOlderThanSource($)))) return null
  showWelcomeStatus(
    $,
    hasEngine ? 'Updating the emulator…' : 'Building the emulator, which happens once and takes about a minute…',
  )
  let problem: string
  try {
    const built = await $.process.run(['bash', root + '/scripts/build.sh'], { timeoutMs: 10 * 60 * 1000 })
    if (built.exitCode === 0 && (await $.fs.exists(enginePath()))) return null
    const lines = (built.stderr || built.stdout).trim().split('\n')
    problem = "Couldn't build the emulator: " + (lines.slice(-2).join(' ') || 'the build failed') + ' (see scripts/build.sh)'
  } catch (error) {
    problem = "Couldn't build the emulator: " + String(error)
  }
  // An update that won't build leaves the emulator that was: it still plays
  if (!hasEngine) return problem
  isUpdateFailed = true
  $.ui.log('claudeboy: ' + problem + '; playing on the emulator built before', { to: 'debug' })
  return null
}

// The engine's source (engine/, the build script) changed since it was built,
// as after an update: it's built again
async function isEngineOlderThanSource($: Dollar): Promise<boolean> {
  try {
    const built = (await $.fs.list(root + '/dist')).find((entry) => entry.name === 'claudeboy-engine')
    if (!built) return false
    const sources = [...(await $.fs.list(root + '/engine')), ...(await $.fs.list(root + '/scripts'))]
    return sources.some((entry) => entry.kind === 'file' && entry.mtimeMs > built.mtimeMs)
  } catch {
    return false
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

// The tool calls running, by id, with their tool and arguments; and those
// whose dialog (a permission, a question, a plan) is open: no drop-in until
// they're answered (their call ends), however other calls end meanwhile.
let runningCalls = new Map<string, { tool: string; input: string }>()
let askingCalls = new Set<string>()
// For a call that comes without an id
let nextCallId = 1

// The same JSON whatever order the keys came in
function stableJson(value: unknown): string {
  if (Array.isArray(value)) return '[' + value.map(stableJson).join(',') + ']'
  if (value && typeof value === 'object') {
    const entries = Object.entries(value as Record<string, unknown>).sort(([a], [b]) => a.localeCompare(b))
    return '{' + entries.map(([key, inner]) => JSON.stringify(key) + ':' + stableJson(inner)).join(',') + '}'
  }
  return JSON.stringify(value) ?? 'null'
}

// A permission dialog names a tool and its input, not the call: the running
// call they match, or the only one of that tool; none for Claude Code's own
// prompts
function callAsked(tool: string, input: unknown): string | null {
  const calls = [...runningCalls].filter(([, call]) => call.tool === tool)
  const wanted = stableJson(input)
  const exact = calls.find(([, call]) => call.input === wanted)
  if (exact) return exact[0]
  return calls.length === 1 ? (calls[0]?.[0] ?? null) : null
}

// In the game: playing, or counting down to the hand-back
function isInGame(): boolean {
  return phase === 'playing' || phase === 'countdown'
}

function armDropIn($: Dollar) {
  if (!isOn || !isTurnRunning || isDismissed || phase !== 'idle' || menu || askingCalls.size) return
  phase = 'waiting'
  timer = $.clock.after(DROP_IN_DELAY_MS, () => void dropIn($))
}

async function dropIn($: Dollar) {
  if (phase !== 'waiting') return
  timer = null
  if (menu) {
    phase = 'idle'
    return
  }
  const surfaces = await $.session.surfaces()
  const problem = surfaces.includes('terminal') ? await loadGame($) : 'no terminal'
  if (problem || phase !== 'waiting') {
    if (phase === 'waiting') phase = 'idle'
    // Turned on but unable to play: said once a session, not at every turn
    if (problem && problem !== 'no terminal' && problem !== toldProblem) {
      toldProblem = problem
      $.ui.toast('ClaudeBoy: ' + problem, { timeoutMs: 8000 })
    }
    return
  }
  if (pictureMode() === 'window') {
    await openDrawerPane($)
    if (phase === 'waiting') await startPlaying($)
    else closeIfUnused($)
    return
  }
  const opened = await openPane($)
  if (phase !== 'waiting') {
    closeIfUnused($)
    return
  }
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
  if (!hasGrid) return columns
  return Math.max(columns, Math.ceil((2 * CONSOLE_WIDTH) / cell.width) + 1)
}

// In window mode the pane opens beside the transcript as wide as the console
// needs, laid over it or out in the drawer, so the transcript moves aside.
// The keys go to the console, unless `isFocused` (a menu, a question). Where
// it doesn't fit, it stays shut.
async function openDrawerPane($: Dollar, isFocused = false): Promise<boolean> {
  const opened = await $.ui.open({
    id: PANE,
    title: game?.title ?? 'ClaudeBoy',
    closeOnEscape: true,
    ...(isFocused ? { focus: true as const } : {}),
    columns: drawerColumns(),
    // Inline: the status and hints, or the room a menu or the question needs
    rows: menu || isAskingTakeover ? INLINE_ROWS : STATUS_ROWS + 2,
  })
  if (!opened.isPlaced) await $.ui.close({ id: PANE })
  return opened.isPlaced
}

// Opens the pane again (or retitles it) as the picture needs it: in window
// mode beside the console, which has the keys unless `isFocused`; else with
// the picture in it, taking the keys
async function reopenPane($: Dollar, isFocused = false): Promise<boolean> {
  if (pictureMode() === 'window') return openDrawerPane($, isFocused)
  return (await openPane($)).isPlaced
}

// The kind of picture changed while in the game (the display setting, a
// terminal refusing pictures, no window to be had): drawn the new way from
// now on, the pane opened for it. False when it can't be placed.
async function changeView($: Dollar): Promise<boolean> {
  forgetPicture()
  hasWindowFocus = false
  wanted.keys = 0
  // A picture's or cells' size comes from the pane as it draws
  wanted.view = pictureMode() === 'window' ? 'window' : 'none'
  scheduleControl($)
  const isPlaced = await reopenPane($)
  redraw($)
  return isPlaced
}

async function acceptOffer($: Dollar) {
  if (phase !== 'offered') return
  const { isPlaced } = await openPane($)
  if (!isPlaced) return
  if (phase === 'offered') await startPlaying($)
  else closeIfUnused($)
}

// Claude finished, or asked something, while the pane was opening: closed
// again, unless something else took it up meanwhile (/claudeboy play, a menu)
function closeIfUnused($: Dollar) {
  if (phase === 'idle' && !menu && !isWelcomeOpen) void $.ui.close({ id: PANE })
}

async function startPlaying($: Dollar) {
  cancelTimer()
  phase = 'playing'
  wanted.countdown = 0
  toastWhenPaused = null
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
  if (engine && engine === quittingEngine) {
    // On its way out: a fresh one once it's gone, or given up on
    void engineStopped?.then(() => {
      if (isInGame() && !engine) void runEngine($)
    })
  } else if (engine) scheduleControl($)
  else void runEngine($)
}

// No drop-in after all: Claude is done or needs the person before it came, or
// before they took up the offer, or a menu is opening
function cancelDropIn($: Dollar) {
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
  isTurnOverUnderMenu = false
  menu = null
  menuError = null
  isPlayAfterOpen = false
  const wasPlaying = isInGame()
  // Under a menu the game was already paused: no pause will come to toast
  const wasRunning = wanted.isPlaying
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
  if (wasPlaying && wasRunning) {
    toastWhenPaused = why
    isSavedSincePause = false
  } else if (wasPlaying && why) {
    $.ui.toast(why)
  }
  scheduleControl($)
  armAwayStop($)
}

// The engine stops once nobody has played for a while
function armAwayStop($: Dollar) {
  awayTimer?.cancel()
  awayTimer = $.clock.after(AWAY_STOP_MS, () => void stopEngine($))
}

// Claude is about to ask the person something, so they must see the prompt
async function needsYou($: Dollar) {
  if (phase === 'waiting' || phase === 'offered') cancelDropIn($)
  else if (isInGame()) await handBack($, 'Claude needs you')
}

// ---------------------------------------------------------------------------
// The engine

async function runEngine($: Dollar) {
  if (engine || !game) return
  const playing = game
  const id = Math.random().toString(36).slice(2, 8)
  const folder = runtimeDir + '/claudeboy-' + id
  runDir = folder
  engineError = null
  announcedSeq = 0
  pulledSeq = 0
  let markExited = () => {}
  engineExited = new Promise((resolve) => {
    markExited = resolve
  })
  // Set before anything is awaited, so a caller finds it at once. The engine
  // stays paused until its control file says otherwise, and reads it every frame.
  const child = $.process.spawn({ argv: [enginePath(), 'run', playing.rom, savesDir(playing), folder] })
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
      // One given up on (stopEngine) isn't listened to: a new one may play
      if (engine === child) for (const line of lines) onEngineLine($, line)
    }
  } catch (error) {
    engineError ??= "ClaudeBoy couldn't start its emulator"
    $.ui.log('claudeboy: the engine failed: ' + String(error), { to: 'debug' })
  }
  // An engine given up on (stopEngine) may end after the next one started:
  // then only its own folder is its to clear
  const isCurrent = engine === child
  const wasStopped = quittingEngine === child
  if (wasStopped) quittingEngine = null
  if (stderr) $.ui.log('claudeboy: the engine said: ' + stderr.trim(), { to: 'debug' })
  if (isCurrent) forgetEngine()
  markExited()
  void $.process.run(['rm', '-rf', folder]).catch(() => {})
  // Ending on its own while someone plays means it failed or was refused;
  // while they're away, the next drop-in simply starts it again. It also ends
  // when this module unloads, and then there's nothing to close.
  if (isCurrent && isInGame() && !wasStopped) {
    try {
      $.ui.toast(engineError ?? "ClaudeBoy's emulator stopped", { timeoutMs: 8000 })
      if (isTurnRunning) isDismissed = true
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
    case '@frame': {
      const [width = 0, height = 0] = words.slice(1).map((word) => Math.floor(Number(word)) || 0)
      if (words[0] && width > 0 && height > 0) showImageFrame($, { shm: words[0], format: 'rgba', width, height })
      break
    }
    case '@term': {
      // Resized, or another font: the picture is laid out anew
      const [columns = 0, rows = 0, width = 0, height = 0] = words.map((word) => Number(word) || 0)
      // A terminal that doesn't say its cells' size keeps the engine's estimate
      if (columns > 0 && rows > 0 && width > 0 && height > 0) cell = { width: width / columns, height: height / rows }
      terminalColumns = columns
      redraw($)
      break
    }
    case '@cell': {
      // Where the terminal fills the desktop, the engine tells its cells'
      // size; 0 0 once it doesn't (the console goes to the drawer)
      const [width = 0, height = 0] = words.map(Number)
      hasGrid = width > 0 && height > 0
      if (hasGrid) cell = { width, height }
      if (wanted.view === 'window' && isInGame()) void openDrawerPane($).catch(() => false)
      redraw($)
      break
    }
    case '@drawer':
      drawer = { width: Number(words[0]) || 0, workWidth: Number(words[1]) || 0 }
      // Sized now the drawer is: the pane makes room to match
      if (wanted.view === 'window' && isInGame()) void openDrawerPane($).catch(() => false)
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
      // The other session paused before the question was answered
      if (isAskingTakeover) {
        isAskingTakeover = false
        void reopenPane($)
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
      if (words[0] && isInGame()) void onKey($, words[0])
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
  if (isInGame() && !(await changeView($))) await handBack($, '')
}

// Another session is playing this game right now: it has the saves until it
// pauses. The pane asks whether to play it here instead, with the keys, so 1
// and 0 or the arrows and Enter answer. Should that session pause first, the
// engine takes the game and the question goes.
async function onBusy($: Dollar) {
  if (!isInGame()) return
  isAskingTakeover = true
  // The gamepad goes with the picture; a new one waits for a click
  hasKeys = false
  await reopenPane($, true)
  redraw($)
}

// Yes: take it. The other session saves and lets go at once.
async function takeOver($: Dollar) {
  isAskingTakeover = false
  await reopenPane($)
  redraw($)
  await sendCommand($, 'takeover')
}

// No: stay out for the rest of the turn
async function declineTakeover($: Dollar) {
  if (isTurnRunning) isDismissed = true
  await handBack($, '')
}

// The game moved: another session took it, or it couldn't be taken
async function leaveGame($: Dollar, toast: string) {
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
    // Pictures and the console over the pane at the Game Boy's own rate.
    // Cells cost the terminal a redraw of the pane each frame, and the drawer
    // can be as large as half the screen: half the rate for those.
    'fps=' + (wanted.view.startsWith('image') || (wanted.view === 'window' && wanted.overlay !== 'none') ? 60 : 30),
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
    const timeout = $.clock.after(COMMAND_TIMEOUT_MS, () => {
      // Taken back too, so an engine that comes round doesn't run it later
      commands = commands.filter((command) => command.id !== id)
      scheduleControl($)
      resolve({ isOk: false, words: "the emulator didn't answer" })
    })
    commands.push({ id, name, arg, resolve, timer: timeout })
    scheduleControl($)
  })
}

// Starts the engine, paused, for a command given while nobody plays
async function ensureRunning($: Dollar): Promise<string | null> {
  const problem = await loadGame($)
  if (problem) return problem
  if (!engine) {
    // Started by itself (runEngine sets it at once), it stops like any other
    void runEngine($)
    if (!isInGame()) armAwayStop($)
  }
  return null
}

async function stopEngine($: Dollar) {
  let markStopped = () => {}
  const stopped = new Promise<void>((resolve) => {
    markStopped = resolve
  })
  awayTimer?.cancel()
  awayTimer = null
  const child = engine
  if (!child) return
  quittingEngine = child
  engineStopped = stopped
  try {
    // It saves and says goodbye on its own; ending the loop is the last resort
    void sendCommand($, 'quit')
    await Promise.race([engineExited, $.clock.sleep(1500)])
    if (engine === child) {
      await child.return({ code: null, signal: null }).catch(() => {})
      // Its stream ends after: wait for that too, so a new engine follows it
      await Promise.race([engineExited, $.clock.sleep(1000)])
    }
    // Given up on if it still hasn't ended: a new one may start meanwhile, and
    // mustn't be handed what was this one's (its quit above, most of all)
    if (engine === child) forgetEngine()
  } finally {
    markStopped()
  }
}

// No engine any more: its commands unanswered, its folder not written to
function forgetEngine() {
  engine = null
  for (const command of commands) {
    command.timer.cancel()
    command.resolve({ isOk: false, words: 'the emulator stopped' })
  }
  commands = []
  runDir = null
}

// ---------------------------------------------------------------------------
// The picture

function forgetPicture() {
  wanted.overlay = 'none'
  size = null
  drawnView = null
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

// What's going on, for the line under the picture, or in the drawer under the
// console (`isInDrawer`), which is what's to be clicked there
function statusLine(isInDrawer = false): { text: string; isStrong: boolean } {
  if (phase === 'countdown') return { text: "Claude's done · back in " + countdown + ' · Esc keeps playing', isStrong: true }
  if (picker) return { text: (picker === 'save' ? 'Save to' : 'Load') + ' which slot? 1–9, any other key cancels', isStrong: true }
  if (message) return { text: message, isStrong: false }
  if (wanted.view === 'window' ? !hasWindowFocus : !hasKeys) {
    if (isInDrawer) return { text: 'Click here to play', isStrong: true }
    return { text: wanted.view === 'window' ? 'Click the game to play' : 'Click the game to play · Esc goes back to Claude', isStrong: true }
  }
  const doing = isOpenedByCommand ? 'Playing' : isTurnRunning ? 'Claude is working' : "Claude's done · Esc goes back"
  return { text: doing, isStrong: false }
}

// ClaudeBoy's own keys, the ones the Game Boy doesn't have: dim, as Claude
// Code's hints are, at most three to a row. F with the speed now.
function hintRows(): string[] {
  if (picker) {
    const slots = SLOTS.map((slot) => slot + ': ' + (slotTimes[slot] ? ago(slotTimes[slot], slotsReadAt) : 'empty'))
    return [slots.slice(0, 3), slots.slice(3, 6), slots.slice(6, 9)].map((row) => row.join(' · '))
  }
  return [
    phase === 'countdown' ? 'S: save · L: load' : 'S: save · L: load · Esc: exit',
    'O: open · G: game library',
    'M: sound · F: ×' + wanted.speed,
  ]
}

// How a menu's items are picked: by number (the first nine), or the arrows
function pickKeys(count: number): string {
  return (count > 1 ? '1–' + Math.min(count, 9) : '1') + ' or ↑↓ and Enter'
}

// The engine's run folder's name, drawn in the pane under the game window:
// the engine finds the terminal tab whose screen shows it, to show the window
// only while that tab is in front
function marker(): string {
  return runDir ? runDir.slice(runDir.lastIndexOf('/') + 1) : ''
}

// The drawer's lines under the game: what's going on, then what keys do
function windowTitle(): string {
  const status = statusLine(true).text
  return [(game?.title ?? 'ClaudeBoy') + ' · ' + status, ...hintRows()].join('\n')
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
      pulledSeq = Math.max(pulledSeq, seq || 0)
      if (!size || columns !== size.columns || rows !== size.rows) continue
      cells = { size: 'cells:' + columns + 'x' + rows, text: text.slice(newline + 1) }
      if (drawnView === cells.size) {
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
  if (drawnView !== 'image') {
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
    while (frame && frame !== blittedFrame && drawnView === 'image') {
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
        $.ui.log('claudeboy: pictures refused (' + result.deny + '); drawing the game another way', { to: 'debug' })
        if (isInGame()) void changeView($)
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

function ago(at: number, now: number): string {
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
    slotsReadAt = await $.clock.now()
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
  } else if (key === 'o') {
    await openMenu($, 'open')
  } else if (key === 'g') {
    await openMenu($, 'games')
  }
}

// ---------------------------------------------------------------------------
// Opening games, and the library

// Shows a menu in the pane, the game paused under it, the pane taking the
// keys: typed paths, 1–9, the arrows and Enter reach the menu
async function openMenu($: Dollar, which: 'open' | 'games') {
  // The game from before the library in it (which needs the engine, once)
  if (!isLibraryKept) {
    await ensureEngine($)
    await keepFirstGame($)
  }
  // An empty library has nothing to pick: open a game instead
  if (which === 'games' && !library.length) which = 'open'
  if (which === 'open') foundRoms = await findRomsNearby($)
  // A drop-in or offer pending doesn't start the game behind the menu
  if (phase === 'waiting' || phase === 'offered') cancelDropIn($)
  menu = which
  menuError = null
  picker = null
  if (isInGame()) {
    cancelTimer()
    phase = 'playing'
    wanted.isPlaying = false
    wanted.countdown = 0
    scheduleControl($)
  }
  hasKeys = false
  await reopenPane($, true)
  redraw($)
}

// Back to the game, or to the welcome
async function closeMenu($: Dollar) {
  menu = null
  menuError = null
  if (isPlayAfterOpen && game) {
    isPlayAfterOpen = false
    await playNow($)
    return
  }
  isPlayAfterOpen = false
  if (phase === 'playing') {
    await startPlaying($)
    await reopenPane($)
    if (isTurnOverUnderMenu && !isTurnRunning) startCountdown($)
    isTurnOverUnderMenu = false
  } else if (isWelcomeOpen) {
    showWelcomeStatus($, game ? 'Ready to play ' + game.title + '.' : NO_GAME)
  } else {
    // Opened while nobody played (/claudeboy open): nothing to go back to.
    // Deferred, as this may run inside the ui.close hook for this very pane.
    // A turn still running drops in as it would have without the menu.
    $.clock.after(0, () => void $.ui.close({ id: PANE }))
    armDropIn($)
  }
  redraw($)
}

// A game picked from a menu, or a path typed into it
async function chooseGame($: Dollar, path: string) {
  if (!path.trim()) return
  menuError = null
  redraw($)
  const problem = await switchGame($, path)
  if (problem) {
    menuError = problem
    redraw($)
    return
  }
  await closeMenu($)
}

// Game Boy games in the usual places, not yet in the library: offered when
// opening a game, so most never need typing
async function findRomsNearby($: Dollar): Promise<string[]> {
  const folders = ['Downloads', 'Desktop', 'Documents', 'Games', 'roms', ''].map((name) => home + (name ? '/' + name : ''))
  folders.push(root, root + '/roms', dataDir + '/roms')
  return (await romsIn($, folders, true)).slice(0, 9)
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
    // No game yet: the first thing is to open one
    if (problem === NO_GAME && isWelcomeOpen) await openMenu($, 'open')
    else showWelcomeStatus($, problem ?? 'Ready to play ' + (game?.title ?? 'your game') + '.')
  })()
  return {}
}

async function turnOff($: Dollar) {
  isOn = false
  await $.store.set('isOn', false)
  if (phase === 'waiting' || phase === 'offered') cancelDropIn($)
  else if (phase !== 'idle' || isWelcomeOpen || menu) await handBack($, '')
  await stopEngine($)
  return { text: 'ClaudeBoy is off. Your game is saved.' }
}

async function playNow($: Dollar) {
  // A menu up over the game (or over nothing) goes first: the game resumes
  // under it, or opens below
  if (menu) {
    isPlayAfterOpen = false
    await closeMenu($)
  }
  const problem = await loadGame($)
  if (problem === NO_GAME) {
    isPlayAfterOpen = true
    await openMenu($, 'open')
    return {}
  }
  if (problem) return { text: problem }
  isOpenedByCommand = true
  if (phase === 'countdown') keepPlaying($)
  if (phase === 'playing') {
    redraw($)
    return {}
  }
  cancelTimer()
  phase = 'idle'
  isWelcomeOpen = false
  if (pictureMode() === 'window') {
    await openDrawerPane($)
    await startPlaying($)
    return {}
  }
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
  const now = await $.clock.now()
  const lines = SLOTS.map((slot) => '  ' + slot + '  ' + (times[slot] ? ago(times[slot], now) : 'empty'))
  const auto = times.auto ? 'autosaved ' + ago(times.auto, now) : 'not autosaved yet'
  return { text: game.title + ' (' + auto + '):\n' + lines.join('\n') + '\n\nIn the game, S saves to a slot and L loads one.' }
}

// /claudeboy open [path]: opens that game; without one, the pane's picker
async function openCommand($: Dollar, path: string) {
  if (!path) {
    await openMenu($, 'open')
    return {}
  }
  const problem = await switchGame($, path)
  if (problem) return { text: problem }
  if (menu) await closeMenu($)
  else if (isInGame()) await startPlaying($)
  return { text: 'ClaudeBoy plays ' + game?.title + ' now.' }
}

// /claudeboy games [n]: lists the library, or plays a game from it
async function gamesCommand($: Dollar, choice: string) {
  await ensureEngine($)
  await keepFirstGame($)
  if (!choice) {
    if (!library.length) return { text: NO_GAME }
    const lines = library.map((kept, i) => '  ' + (i + 1) + '  ' + kept.title + (kept.rom === game?.rom ? '  (playing)' : '') + '\n     ' + kept.rom)
    return { text: 'Your game library:\n' + lines.join('\n') + '\n\nPlay one with /claudeboy games <number>, or press G in ClaudeBoy.' }
  }
  const kept = library[Number(choice) - 1]
  if (!/^\d+$/.test(choice) || !kept) return { text: 'No game number ' + choice + '. /claudeboy games lists them.' }
  return openCommand($, kept.rom)
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
  // The picture needs the pane (the console plays in the drawer without)
  if (isInGame() && !(await changeView($)) && pictureMode() !== 'window') await handBack($, '')
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
    const keptIsOn = await $.store.get('isOn')
    isOn = keptIsOn === true
    const rom = await $.store.get('rom')
    romChoice = typeof rom === 'string' ? rom : null
    const kept = await $.store.get('library')
    isLibraryKept = Array.isArray(kept)
    library = isLibraryKept
      ? (kept as unknown[]).filter((entry): entry is LibraryGame => typeof (entry as LibraryGame)?.rom === 'string')
      : []
    wasUsed = romChoice !== null || keptIsOn !== undefined
    // Off until the person turns it on: the game opens unasked, often at work
    isSoundOn = (await $.store.get('sound')) === true
    const shown = await $.store.get('display')
    display = shown === 'cells' || shown === 'image' || shown === 'window' ? shown : 'auto'
    await $.command.register({
      name: 'claudeboy',
      description: 'Play a Game Boy game while Claude works',
      argumentHint: '[play|off|open [path]|games [n]|save <1-9>|load <1-9>|slots|sound on/off|display auto/cells/image/window|help]',
      immediate: true,
    })
    return next(e)
  })

  on('session.end', async ($, e, next) => {
    // Out of the game first, so the engine ending isn't taken for a failure.
    // After /clear or a resume the process goes on: the pane is closed too.
    // On exit there's no time for that: the engine must save and go first.
    const isProcessGoingOn = e.reason === 'clear' || e.reason === 'resume'
    if (isProcessGoingOn && (isInGame() || isWelcomeOpen || menu)) await handBack($, '')
    else goAway($, '')
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
      case 'open':
        return openCommand($, rest)
      case 'games':
        return gamesCommand($, rest)
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
    runningCalls = new Map()
    askingCalls = new Set()
    // On or off as /claudeboy last left it in any session
    isOn = (await $.store.get('isOn')) === true
    if (isWelcomeOpen && !menu) {
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
    runningCalls = new Map()
    askingCalls = new Set()
    if (phase === 'waiting' || phase === 'offered') {
      cancelDropIn($)
    } else if (phase === 'playing' && !isOpenedByCommand) {
      if (e.isAborted) await handBack($, '')
      // Under a menu, the count starts once it closes
      else if (menu) isTurnOverUnderMenu = true
      else startCountdown($)
    }
    return next(e)
  })

  // A permission dialog is about to ask the person. In auto mode most asks go
  // to the classifier instead, and this never fires for those: play goes on.
  on('classic.PermissionRequest', async ($, e, next) => {
    const asked = callAsked(e.tool_name, e.tool_input)
    if (asked) askingCalls.add(asked)
    await needsYou($)
    return next(e)
  })

  on('tool.call', async ($, e, next) => {
    const { tool, tool_use_id: callId, ...input } = e as { tool: string; tool_use_id?: string } & Record<string, unknown>
    const id = callId ?? 'call-' + nextCallId++
    runningCalls.set(id, { tool, input: stableJson(input) })
    // These put a question or a plan to the person whatever the mode
    if (tool === 'AskUserQuestion' || tool === 'ExitPlanMode') {
      askingCalls.add(id)
      await needsYou($)
    }
    try {
      return await next(e)
    } finally {
      // Answered (its call ended): once nothing else is asked, drop back in
      runningCalls.delete(id)
      askingCalls.delete(id)
      armDropIn($)
    }
  })

  // The pane is closing (this module's own closes don't reach this hook)
  on('ui.close', async ($, e, next) => {
    if (e.id !== PANE) return next(e)
    // Esc in a menu goes back to the game (or the welcome)
    if (e.origin?.kind === 'person' && menu) {
      isPlayAfterOpen = false
      await closeMenu($)
      return { value: undefined }
    }
    // Esc during the countdown keeps playing: the pane stays open
    if (e.origin?.kind === 'person' && phase === 'countdown') {
      keepPlaying($)
      // The gamepad gave the keys back before Esc closed the pane
      hasKeys = false
      // Opened again, so the next Esc closes it: a refused close spends it
      await reopenPane($)
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
      lastTaps = 0
    }
    if (!hasKeys && (post.held !== 0 || post.keys.length > 0 || post.taps.some((count) => count > 0))) {
      hasKeys = true
      redraw($)
    }
    // A Game Boy button is "any other key" for the slot picker too
    const taps = post.taps.reduce((sum, count) => sum + count, 0)
    if (picker && taps > lastTaps) {
      picker = null
      redraw($)
    }
    lastTaps = taps
    wanted.keys = isInGame() ? post.held : 0
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
    const { Box, Text, Button, Input, Raster, Image, Client } = $.ui.resolve(e)

    if (menu === 'open') {
      const isFirst = !library.length
      return (
        <Box flexDirection="column" gap={1}>
          <Text bold>{isFirst ? 'Open your first Game Boy game' : 'Open a Game Boy game'}</Text>
          {foundRoms.length ? (
            <Box flexDirection="column">
              <Text dimColor>Found on this computer:</Text>
              {foundRoms.map((rom, i) => (
                <Button
                  key={'found-' + i}
                  label={rom.slice(rom.lastIndexOf('/') + 1)}
                  hotkey={String(i + 1)}
                  plain
                  {...(i === 0 ? { autoFocus: true as const } : {})}
                  onPress={() => chooseGame($, rom)}
                />
              ))}
            </Box>
          ) : null}
          <Input
            key="rom-path"
            label="Or its path"
            placeholder="~/Downloads/game.gb"
            submitLabel="open"
            {...(foundRoms.length ? {} : { autoFocus: true as const })}
            onSubmit={(path) => chooseGame($, path)}
          />
          {menuError ? <Text color="red">{menuError}</Text> : null}
          <Text dimColor>
            {(foundRoms.length ? pickKeys(foundRoms.length) + ' picks one · ' : '') + 'Esc goes back'}
          </Text>
        </Box>
      )
    }
    if (menu === 'games') {
      return (
        <Box flexDirection="column" gap={1}>
          <Text bold>Your game library</Text>
          <Box flexDirection="column">
            {library.map((kept, i) => (
              <Button
                key={'game-' + i}
                label={kept.title + (kept.rom === game?.rom ? '  · playing' : '')}
                {...(i < 9 ? { hotkey: String(i + 1) } : {})}
                plain
                {...(i === 0 ? { autoFocus: true as const } : {})}
                onPress={() => chooseGame($, kept.rom)}
              />
            ))}
            <Button key="games-open" label="Open another game…" hotkey="o" plain onPress={() => openMenu($, 'open')} />
          </Box>
          {menuError ? <Text color="red">{menuError}</Text> : null}
          <Text dimColor>{pickKeys(library.length) + ' picks one · O opens another · Esc goes back'}</Text>
        </Box>
      )
    }

    if (isWelcomeOpen) {
      return (
        <Box flexDirection="column" gap={1}>
          <Text bold>ClaudeBoy is on</Text>
          <Text>
            When Claude has been working for two seconds, your Game Boy game opens: as a picture in this pane in kitty
            or Ghostty; on a Game Boy drawn over the pane, or at the screen's edge, in other terminals; drawn in text
            with no desktop. You're handed back when Claude is done or needs you, and the game is saved each time.
          </Text>
          <Text>{"The Game Boy's buttons: " + CONTROLS + '.'}</Text>
          <Text>{'In the game: ' + GAME_KEYS + '.'}</Text>
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
              {text}
            </Text>
            {hintRows().map((row, i) => (
              <Text key={'hint-' + i} dimColor wrap="truncate-end">
                {row}
              </Text>
            ))}
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
    drawnView = picture ? (mode === 'image' ? 'image' : view) : null
    if (!picture) return <Text dimColor>Starting {game?.title ?? 'the game'}…</Text>

    const { text: status, isStrong } = statusLine()

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
          {hintRows().map((row, i) => (
            <Text key={'hint-' + i} dimColor wrap="truncate-end">
              {row}
            </Text>
          ))}
        </Box>
      </Box>
    )
  })
}
