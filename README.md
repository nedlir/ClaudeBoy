# ClaudeBoy

A Claude Code mod that drops you into a Game Boy game while Claude works, and
hands you back when it's done. Everything runs on your machine: the emulator is
[mGBA](https://mgba.io)'s Game Boy core in a small process of its own, and there's
no server or account.

When Claude has been working for two seconds, a pane opens beside the
transcript with your game, right where you left it. When Claude finishes
there's a three-second countdown, then the game pauses, **saves itself**, and
you're handed back. If Claude needs you, for a permission prompt or a question,
you're handed back at once and dropped in again after you answer. In auto mode
the classifier answers most prompts, so play goes on through those.

Game Boy and Game Boy Color games both work. It was built against Pokémon Yellow.

## Setup

1. Build the emulator, once. It fetches mGBA 0.10.5's source (about 14 MB,
   checked against its checksum) and needs `cmake`, a C compiler and `curl`:

   ```
   ./scripts/build.sh
   ```

   `/claudeboy` runs this for you if you skip it.

2. Put a ROM where ClaudeBoy looks: `~/.local/share/claudeboy/roms/`, or this
   folder (or its `roms/`). `/claudeboy rom <path>` picks any other file.

3. Load the mod. To try it for one session:

   ```
   claude --plugin-dir ~/Desktop/ClaudeBoy
   ```

   To keep it, install it from this folder, which stays the copy Claude Code
   runs (after an edit, `/reload-plugins` picks it up):

   ```
   claude plugin marketplace add ~/Desktop/ClaudeBoy
   claude plugin install claudeboy@claudeboy
   ```

4. Turn it on with `/claudeboy`. `/claudeboy off` turns it off again.

## Playing

Click the game to give it the keys: the picture in the pane, or the game's
drawer. In the pane, Esc hands the keys back to the prompt; in the drawer,
Esc hands you back to Claude.

| Game Boy | Key |
| :- | :- |
| D-pad | arrow keys |
| A | Z |
| B | X |
| Start | Enter |
| Select | Backspace |

| ClaudeBoy | Key |
| :- | :- |
| Save to a slot | S, then 1–9 |
| Load a slot | L, then 1–9 |
| Sound on or off | M (off at first) |
| Fast-forward ×2, ×4, ×8, then back | F |

The game's window sees keys as any game does. Terminals, though, report key
presses but not releases, so in the pane a lone press is a short tap, and a
held key counts as held for as long as its auto-repeat keeps coming. In
games like Pokémon a tap takes one step and holding walks, with a short stop
after the first step while the repeat starts. Shortening your keyboard's repeat
delay in your desktop's settings shortens that stop.

## Saves

- **Autosave.** The game is saved whenever it pauses: each time you're handed
  back, when you close the pane, and when the session ends. It's also saved
  every two minutes of play. The next drop-in picks up exactly there.
- **Slots.** Nine save states per game: S or L in the game, or
  `/claudeboy save <1-9>`, `/claudeboy load <1-9>` and `/claudeboy slots` from
  the prompt, which work while Claude is idle too. A slot holds the whole
  game, its in-game save included, so loading one brings everything back as
  it was.
- **In-game saves** go to a regular battery save that other emulators read.

Everything lives in `~/.local/share/claudeboy/saves/<game>/`: `battery.sav`,
`auto.state` and `slot1.state` to `slot9.state`. To bring a save from another
emulator, put its `.sav` there as `battery.sav` and delete `auto.state`, if
there is one: an autosave carries the in-game save it was made with, and
loading it writes that back. Set `CLAUDEBOY_DATA_DIR` to keep saves and ROMs
somewhere else.

One session at a time plays a given game. Another session that tries gets told
so, and stays out for a minute.

## Where the game shows

- **In kitty or Ghostty:** right in the pane beside the transcript, as a real
  picture rendered at exactly the size the terminal shows it, so every pixel
  is sharp. It fills the pane at the Game Boy screen's shape and follows the
  pane and the terminal as they resize or change font.
- **In any other terminal on a desktop** (GNOME Terminal, Konsole, Alacritty
  and the rest), ClaudeBoy draws the game itself on top of Claude's side pane.
  When the terminal is maximized, it works out where the pane's picture
  belongs on screen from the desktop's work area and the terminal's grid of
  cells. It lays a borderless game surface exactly there, at a whole-number
  scale, while Claude Code draws the pane's frame and status lines around
  it, in the terminal's own font, as in kitty. It fades in and out with the
  turn. When the terminal isn't maximized its place can't be known, because
  Wayland doesn't say where windows are. Then the game slides out as a
  drawer at the right edge of the screen, the pane opening at the same
  width so the transcript moves aside. Either way Esc hands back early, and
  you click the game to play if it doesn't take the keyboard by itself. It
  uses X11, or XWayland on Wayland desktops, loaded only when needed. It
  stays above other windows while it's out.
- **With no desktop**, as over SSH: in the pane, drawn with colored
  half-block characters, two pixels per cell. Small panes look blocky.

`/claudeboy display auto|image|window|cells` overrides the choice, and
`CLAUDE_CODE_FORCE_TERMINAL_IMAGES=1` tells Claude Code to send pictures to a
terminal it doesn't recognize. kitty is one install away on Ubuntu:
`sudo apt install kitty`. Don't run `claude` inside tmux, which passes no
pictures through.

## Commands

```
/claudeboy              turn it on
/claudeboy off          turn it off
/claudeboy play         play now, whether or not Claude is working
/claudeboy save <1-9>   save to a slot
/claudeboy load <1-9>   load a slot
/claudeboy slots        list the slots
/claudeboy rom [path]   list the ROMs found, or pick one by number or path
/claudeboy sound on|off
/claudeboy display auto|cells|image
```

## How it works

`hooks/register.tsx` is the mod. It follows the turn (start, completion,
permission prompts), opens the pane, and starts the engine. The engine,
`engine/claudeboy-engine.c`, runs the game at the Game Boy's 59.7 frames a
second. Every frame it reads a one-line control file the mod rewrites (play or
pause, held buttons, the picture's size, commands), and it reports on stdout.
Each frame goes to a file in `$XDG_RUNTIME_DIR` as the cells of a `Raster`, or
in kitty and Ghostty into shared memory for an `Image`, rendered at the box's
size in pixels. The mod paints it in place. The engine also reads the
terminal's size, in pixels too where the terminal reports it, so the mod can
refit the picture whenever the terminal changes. `hooks/gamepad.tsx` runs over
the picture and turns key presses into held buttons. Other terminals get the
engine's own window (`engine/window.c`). The engine loads `libX11` the first
time the window opens and reads the buttons straight from the window. Keys
that aren't the Game Boy's, like S, L, M and F, go up to the mod, which sends
back the window's title. Sound, when on, is played through `aplay`, `pacat`
or `ffplay`.

The engine can also be run by hand:

```
dist/claudeboy-engine info <rom>
dist/claudeboy-engine script <rom> <saves> <out> 600 tap:start 120 shot:title
```

## Development

```
claude plugin validate .
claude plugin test .
```

## Licenses

mGBA is MPL-2.0. Its source is fetched by `scripts/build.sh` and built into
`dist/claudeboy-engine`. ROMs aren't included. Use games you own.
