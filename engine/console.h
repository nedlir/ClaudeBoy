// The console drawn around the game: a Game Boy in Claude's orange, in pixel
// art, wide with the controls at the sides, its buttons lit and pressed in as
// they're held.

#ifndef CLAUDEBOY_CONSOLE_H
#define CLAUDEBOY_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

// The Game Boy's screen, and the console around it, in its own pixels.
// hooks/register.tsx keeps CONSOLE_WIDTH too, to size the pane for it.
#define CONSOLE_SCREEN_WIDTH 160
#define CONSOLE_SCREEN_HEIGHT 144
#define CONSOLE_WIDTH 272
#define CONSOLE_HEIGHT 198

// Hit by a point: the speed button, past the Game Boy's eight
#define CONSOLE_SPEED 8

// The largest whole multiple of the console's size that fits a `width` by
// `height` box (at least 1)
int consoleFit(int width, int height);
// Where the screen is in the console
void consoleScreen(int* x, int* y);

// Draws the console with `screen` (160 by 144, each 0x00RRGGBB) in it, as
// 0xAARRGGBB, see-through outside its body; `keys` lights the Game Boy's held
// buttons, `isSpeedHeld` the speed button, `speed` its label
void consoleDraw(const uint32_t* screen, uint32_t keys, bool isSpeedHeld, int speed, uint32_t* out);

// The button at a point of the console: a Game Boy bit (0 to 7),
// CONSOLE_SPEED, or -1 for none
int consoleButtonAt(double x, double y);

#endif
