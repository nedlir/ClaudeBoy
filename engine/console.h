// The console drawn around the game: a Game Boy in Claude's orange, in pixel
// art, wide with the controls at the sides, its buttons lit and pressed in as
// they're held.

#ifndef CLAUDEBOY_CONSOLE_H
#define CLAUDEBOY_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#define CONSOLE_SCREEN_WIDTH 160
#define CONSOLE_SCREEN_HEIGHT 144
#define CONSOLE_MAX_WIDTH 272
#define CONSOLE_MAX_HEIGHT 198

// Hit by a point: the speed button, past the Game Boy's eight
#define CONSOLE_SPEED 8

struct ConsoleShape {
	bool isWide;
	int width;
	int height;
};

// The console's shape, and the largest whole multiple of its size that fits
// a `width` by `height` box (at least 1)
int consoleFit(int width, int height, struct ConsoleShape* shape);
// Where the screen is in the console, in its own pixels
void consoleScreen(struct ConsoleShape shape, int* x, int* y);

// Draws the console with `screen` (160 by 144, each 0x00RRGGBB) in it, as
// 0xAARRGGBB, see-through outside its body; `keys` lights the Game Boy's held
// buttons, `isSpeedHeld` the speed button, `speed` its label
void consoleDraw(struct ConsoleShape shape, const uint32_t* screen, uint32_t keys, bool isSpeedHeld, int speed,
                 bool isOn, uint32_t* out);

// The button at a point of the console, in its own pixels: a Game Boy bit
// (0 to 7), CONSOLE_SPEED, or -1 for none
int consoleButtonAt(struct ConsoleShape shape, double x, double y);

#endif
