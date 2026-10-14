// The game window: the Game Boy's screen in a window of its own, for
// terminals that can't show pictures. X11, loaded when first opened, so the
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
	// The Game Boy's buttons held now, in its own bit order
	uint32_t keys;
	// Other keys pressed, each a lowercase letter or digit
	char typed[WINDOW_TYPED_MAX];
	int typedCount;
	// The person closed the window
	bool isClosed;
	// The picture must be drawn again: shown, resized or uncovered
	bool needsDrawing;
	// 1 when the window got the keyboard, 0 when it lost it, -1 neither
	int focus;
};

// Opens a window, unmapped, `scale` times the screen's size; NULL with the
// reason in `error` when there's no display or no X11
struct GameWindow* gameWindowOpen(int scale, char* error, size_t errorSize);
void gameWindowShow(struct GameWindow* window);
void gameWindowHide(struct GameWindow* window);
void gameWindowSetTitle(struct GameWindow* window, const char* utf8);
void gameWindowPump(struct GameWindow* window, struct GameWindowEvents* events);
// Draws a frame of 160 by 144 pixels, each 0x00RRGGBB, as large as fits at
// a whole multiple of its size, in the middle
void gameWindowDraw(struct GameWindow* window, const uint32_t* frame);
void gameWindowClose(struct GameWindow* window);
// Called if the display goes away, before the process has to end
void gameWindowOnLost(void (*callback)(void));

#endif
