// The gamepad: catches keys over the game picture and posts which Game Boy
// buttons are held, which the hooks module hands to the engine. Other keys
// (S, L, M, F and digits for the slots) are posted as they come.
//
// The terminal reports presses, never releases. A lone press is a tap, held
// long enough for any game to see and short enough that taps a few frames
// apart stay apart. A held key repeats, after a pause, and counts as held for
// as long as its repeats keep coming.
//
// Time here is the frame clock's: each tick of `surface.every` moves it on.

import type { ClientModule } from 'claude-code'

const TICK_MS = 16
// A press closer than this to the last of the same key is its auto-repeat
const REPEAT_GAP_MS = 80
const TAP_MS = 100
const HOLD_WHILE_REPEATING_MS = 110
// The other keys, newest last, so a key a post replaced still arrives
const KEYS_KEPT = 8

// Bits in the Game Boy's own order, as the engine takes them
const BUTTONS: Record<string, number> = {
  z: 0, // A
  x: 1, // B
  backspace: 2, // Select
  return: 3, // Start
  right: 4,
  left: 5,
  up: 6,
  down: 7,
}

type Pad = {
  // Tells this gamepad's posts apart from another's, whose counts start over
  id: string
  now: number
  lastPressAt: Map<number, number>
  heldUntil: Map<number, number>
  taps: number[]
  keys: { seq: number; key: string }[]
  seq: number
  posted: string
}

const Gamepad: ClientModule<null, Pad> = (props, surface) => {
  if (surface.state === undefined) {
    const pad: Pad = {
      id: Math.random().toString(36).slice(2, 10),
      now: 0,
      lastPressAt: new Map(),
      heldUntil: new Map(),
      taps: [0, 0, 0, 0, 0, 0, 0, 0],
      keys: [],
      seq: 0,
      posted: '',
    }

    surface.onKey((e) => {
      const name = e.key.length === 1 ? e.key.toLowerCase() : e.key
      const button = BUTTONS[name]
      if (button === undefined) {
        if (!e.ctrl && !e.meta) pad.keys = [...pad.keys, { seq: ++pad.seq, key: name }].slice(-KEYS_KEPT)
        return
      }
      const last = pad.lastPressAt.get(button)
      pad.lastPressAt.set(button, pad.now)
      if (last !== undefined && pad.now - last <= REPEAT_GAP_MS) {
        pad.heldUntil.set(button, pad.now + HOLD_WHILE_REPEATING_MS)
      } else {
        // The engine holds a tap for a few frames even if its release comes first
        pad.taps[button] = (pad.taps[button] ?? 0) + 1
        pad.heldUntil.set(button, Math.max(pad.heldUntil.get(button) ?? 0, pad.now + TAP_MS))
      }
    })

    surface.every(TICK_MS, () => {
      pad.now += TICK_MS
      let held = 0
      for (const [button, until] of pad.heldUntil) {
        if (pad.now < until) held |= 1 << button
        else pad.heldUntil.delete(button)
      }
      const post = { pad: pad.id, held, taps: [...pad.taps], keys: pad.keys }
      const text = JSON.stringify(post)
      if (text === pad.posted) return
      pad.posted = text
      surface.post(post)
    })

    surface.setState(pad)
  }

  const { Box } = surface.elements
  return Box({ width: '100%', height: '100%' })
}

export default Gamepad
