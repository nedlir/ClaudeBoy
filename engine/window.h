// The game window, for terminals that can't show pictures: the game on a
// Game Boy in Claude's orange, laid over Claude's pane or in a drawer at the
// right edge of the desktop. X11, loaded when first opened, so the
// engine needs no X11 to build or to run without a window; on a Wayland
// desktop it's XWayland's.

#ifndef CLAUDEBOY_WINDOW_H
#define CLAUDEBOY_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WINDOW_SCREEN_WIDTH 160
#define WINDOW_SCREEN_HEIGHT 144
#define WINDOW_TYPED_MAX 16

struct GameWindow;

// What came in since the last pump
struct GameWindowEvents {
	// The Game Boy's buttons held now, by key or on the console with the
	// mouse, in its own bit order
	uint32_t keys;
	// Other keys pressed, each a lowercase letter or digit; a click on the
	// console's speed button is an f
	char typed[WINDOW_TYPED_MAX];
	int typedCount;
	// The person closed the window
	bool isClosed;
	// The picture must be drawn again: shown, resized or uncovered
	bool needsDrawing;
	// 1 when the window got the keyboard, 0 when it lost it, -1 neither
	int focus;
};

// Opens the drawer, unmapped, docked to the right edge of the desktop, the
// game `scale` times its size (0: as large as suits the desktop); NULL with
// the reason in `error` when there's no display or no X11
struct GameWindow* gameWindowOpen(int scale, char* error, size_t errorSize);
void gameWindowShow(struct GameWindow* window);
void gameWindowHide(struct GameWindow* window);
void gameWindowSetTitle(struct GameWindow* window, const char* utf8);
// The speed the console's speed button shows
void gameWindowSetSpeed(struct GameWindow* window, int speed);
void gameWindowPump(struct GameWindow* window, struct GameWindowEvents* events);
// Draws a frame of 160 by 144 pixels, each 0x00RRGGBB, on the console's
// screen: the console as large as fits at a whole multiple of its size, its
// held buttons lit, in the middle
void gameWindowDraw(struct GameWindow* window, const uint32_t* frame);
void gameWindowClose(struct GameWindow* window);
// The drawer's width, and the width of the desktop it docks in, in pixels
void gameWindowDrawer(struct GameWindow* window, int* width, int* workWidth);
// The desktop's work area: what's left of the screen beside its panels
void gameWindowWorkArea(struct GameWindow* window, int* x, int* y, int* width, int* height);
// Lays the window over a box of the screen, in pixels: the console alone, as
// large as fits at a whole multiple of its size, in its middle; a width of 0
// docks it as the drawer again
void gameWindowPlace(struct GameWindow* window, int x, int y, int width, int height);
// Called if the display goes away, before the process has to end
void gameWindowOnLost(void (*callback)(void));

#endif
