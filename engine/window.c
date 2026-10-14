// The game window, on X11 through libX11 and libXext loaded when it's first
// opened. Laid exactly over a box of the terminal (Claude's side pane) where
// the engine can tell where that is, the game alone, so Claude draws the rest;
// elsewhere a drawer: docked to the right edge of the desktop and as tall as
// it, the game in the middle and what's going on under it. Either way it has
// no title bar, and fades in and out. The few Xlib types it uses are declared here as Xlib.h has them, so
// building needs no X11 headers; without X11, or a display, it doesn't open
// and the engine says so.

#define _GNU_SOURCE

#include "window.h"

#include "console.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

// --- Xlib, as Xlib.h and XShm.h declare it ---------------------------------

typedef unsigned long XID;
typedef XID Window;
typedef XID Pixmap;
typedef unsigned long Atom;
typedef unsigned long Time;
typedef unsigned long KeySym;
typedef int Bool;
typedef int Status;
typedef struct _XDisplay Display;
typedef struct _XGC* GC;

typedef struct {
	void* extData;
	unsigned long visualId;
	int visualClass;
	unsigned long redMask;
	unsigned long greenMask;
	unsigned long blueMask;
	int bitsPerRgb;
	int mapEntries;
} Visual;

typedef struct XImage {
	int width;
	int height;
	int xoffset;
	int format;
	char* data;
	int byteOrder;
	int bitmapUnit;
	int bitmapBitOrder;
	int bitmapPad;
	int depth;
	int bytesPerLine;
	int bitsPerPixel;
	unsigned long redMask;
	unsigned long greenMask;
	unsigned long blueMask;
	char* obdata;
	struct {
		void* createImage;
		int (*destroyImage)(struct XImage*);
		void* getPixel;
		void* putPixel;
		void* subImage;
		void* addPixel;
	} f;
} XImage;

typedef struct {
	int type;
	unsigned long serial;
	Bool sendEvent;
	Display* display;
	Window window;
	Window root;
	Window subwindow;
	Time time;
	int x;
	int y;
	int xRoot;
	int yRoot;
	unsigned int state;
	unsigned int keycode;
	Bool sameScreen;
} XKeyEvent;

typedef struct {
	int type;
	unsigned long serial;
	Bool sendEvent;
	Display* display;
	Window event;
	Window window;
	int x;
	int y;
	int width;
	int height;
	int borderWidth;
	Window above;
	Bool overrideRedirect;
} XConfigureEvent;

typedef struct {
	int type;
	unsigned long serial;
	Bool sendEvent;
	Display* display;
	Window window;
	Atom messageType;
	int format;
	union {
		char b[20];
		short s[10];
		long l[5];
	} data;
} XClientMessageEvent;

typedef struct {
	int type;
	unsigned long serial;
	Bool sendEvent;
	Display* display;
	Window window;
	Window root;
	Window subwindow;
	Time time;
	int x;
	int y;
	int xRoot;
	int yRoot;
	unsigned int state;
	unsigned int button;
	Bool sameScreen;
} XButtonEvent;

typedef union {
	int type;
	XKeyEvent xkey;
	XButtonEvent xbutton;
	XConfigureEvent xconfigure;
	XClientMessageEvent xclient;
	long pad[24];
} XEvent;

typedef struct {
	long flags;
	int x;
	int y;
	int width;
	int height;
	int minWidth;
	int minHeight;
	int maxWidth;
	int maxHeight;
	int widthInc;
	int heightInc;
	struct {
		int x;
		int y;
	} minAspect, maxAspect;
	int baseWidth;
	int baseHeight;
	int winGravity;
} XSizeHints;

typedef struct {
	char* resName;
	char* resClass;
} XClassHint;

typedef struct {
	unsigned long shmseg;
	int shmid;
	char* shmaddr;
	Bool readOnly;
} XShmSegmentInfo;

enum {
	KeyPress = 2,
	KeyRelease = 3,
	ButtonPress = 4,
	ButtonRelease = 5,
	FocusIn = 9,
	FocusOut = 10,
	Expose = 12,
	MapNotify = 19,
	ConfigureNotify = 22,
	ClientMessage = 33,
};

#define KeyPressMask (1L << 0)
#define KeyReleaseMask (1L << 1)
#define ButtonPressMask (1L << 2)
#define ButtonReleaseMask (1L << 3)
#define ExposureMask (1L << 15)
#define StructureNotifyMask (1L << 17)
#define SubstructureNotifyMask (1L << 19)
#define SubstructureRedirectMask (1L << 20)
#define FocusChangeMask (1L << 21)
#define ZPixmap 2
#define PropModeReplace 0
#define XA_ATOM 4
#define XA_CARDINAL 6
#define USPosition (1L << 0)
#define USSize (1L << 1)
#define PPosition (1L << 2)
#define PSize (1L << 3)
#define PMinSize (1L << 4)
#define PMaxSize (1L << 5)
#define PResizeInc (1L << 6)
#define PAspect (1L << 7)
#define PBaseSize (1L << 8)
#define SHAPE_BOUNDING 0
#define SHAPE_SET 0

static struct {
	bool isLoaded;
	Display* (*OpenDisplay)(const char*);
	int (*CloseDisplay)(Display*);
	int (*DefaultScreen)(Display*);
	Window (*RootWindow)(Display*, int);
	Visual* (*DefaultVisual)(Display*, int);
	int (*DefaultDepth)(Display*, int);
	int (*DisplayHeight)(Display*, int);
	int (*DisplayWidth)(Display*, int);
	int (*GetWindowProperty)(Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long*, unsigned long*,
	                         unsigned char**);
	int (*Free)(void*);
	GC (*CreateGC)(Display*, Window, unsigned long, void*);
	XID (*LoadFont)(Display*, const char*);
	int (*SetFont)(Display*, GC, XID);
	int (*SetForeground)(Display*, GC, unsigned long);
	int (*DrawString)(Display*, Window, GC, int, int, const char*, int);
	GC (*DefaultGC)(Display*, int);
	Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
	int (*DestroyWindow)(Display*, Window);
	int (*SelectInput)(Display*, Window, long);
	int (*MapRaised)(Display*, Window);
	int (*UnmapWindow)(Display*, Window);
	int (*MoveWindow)(Display*, Window, int, int);
	int (*MoveResizeWindow)(Display*, Window, int, int, unsigned, unsigned);
	int (*StoreName)(Display*, Window, const char*);
	Atom (*InternAtom)(Display*, const char*, Bool);
	int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int);
	Status (*SetWMProtocols)(Display*, Window, Atom*, int);
	void (*SetWMNormalHints)(Display*, Window, XSizeHints*);
	int (*SetClassHint)(Display*, Window, XClassHint*);
	int (*Pending)(Display*);
	int (*NextEvent)(Display*, XEvent*);
	KeySym (*LookupKeysym)(XKeyEvent*, int);
	int (*Flush)(Display*);
	int (*Sync)(Display*, Bool);
	XImage* (*CreateImage)(Display*, Visual*, unsigned, int, int, char*, unsigned, unsigned, int, int);
	int (*PutImage)(Display*, Window, GC, XImage*, int, int, int, int, unsigned, unsigned);
	Status (*SendEvent)(Display*, Window, Bool, long, XEvent*);
	Bool (*SetDetectableAutoRepeat)(Display*, Bool, Bool*);
	void* (*SetErrorHandler)(int (*)(Display*, void*));
	void* (*SetIOErrorHandler)(int (*)(Display*));
	Pixmap (*CreateBitmapFromData)(Display*, Window, const char*, unsigned, unsigned);
	int (*FreePixmap)(Display*, Pixmap);
	void (*ShapeCombineMask)(Display*, Window, int, int, int, Pixmap, int);
	Bool (*ShmQueryExtension)(Display*);
	XImage* (*ShmCreateImage)(Display*, Visual*, unsigned, int, char*, XShmSegmentInfo*, unsigned, unsigned);
	Bool (*ShmAttach)(Display*, XShmSegmentInfo*);
	Bool (*ShmDetach)(Display*, XShmSegmentInfo*);
	Bool (*ShmPutImage)(Display*, Window, GC, XImage*, int, int, int, int, unsigned, unsigned, Bool);
} X;

static bool loadX11(char* error, size_t errorSize) {
	if (X.isLoaded) {
		return true;
	}
	void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
	if (!x11) {
		snprintf(error, errorSize, "no X11 (libX11.so.6) to draw a window with");
		return false;
	}
	// Optional: without it frames go over the X connection
	void* xext = dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL);
	struct {
		void* library;
		const char* name;
		void** slot;
		bool isRequired;
	} symbols[] = {
#define FROM(library, field, isRequired) { library, "X" #field, (void**) &X.field, isRequired }
		FROM(x11, OpenDisplay, true),
		FROM(x11, CloseDisplay, true),
		FROM(x11, DefaultScreen, true),
		FROM(x11, RootWindow, true),
		FROM(x11, DefaultVisual, true),
		FROM(x11, DefaultDepth, true),
		FROM(x11, DisplayHeight, true),
		FROM(x11, DisplayWidth, true),
		FROM(x11, GetWindowProperty, true),
		FROM(x11, Free, true),
		FROM(x11, CreateGC, true),
		FROM(x11, LoadFont, true),
		FROM(x11, SetFont, true),
		FROM(x11, SetForeground, true),
		FROM(x11, DrawString, true),
		FROM(x11, DefaultGC, true),
		FROM(x11, CreateSimpleWindow, true),
		FROM(x11, DestroyWindow, true),
		FROM(x11, SelectInput, true),
		FROM(x11, MapRaised, true),
		FROM(x11, UnmapWindow, true),
		FROM(x11, MoveWindow, true),
		FROM(x11, MoveResizeWindow, true),
		FROM(x11, StoreName, true),
		FROM(x11, InternAtom, true),
		FROM(x11, ChangeProperty, true),
		FROM(x11, SetWMProtocols, true),
		FROM(x11, SetWMNormalHints, true),
		FROM(x11, SetClassHint, true),
		FROM(x11, Pending, true),
		FROM(x11, NextEvent, true),
		FROM(x11, LookupKeysym, true),
		FROM(x11, Flush, true),
		FROM(x11, Sync, true),
		FROM(x11, CreateImage, true),
		FROM(x11, PutImage, true),
		FROM(x11, SendEvent, true),
		FROM(x11, SetErrorHandler, true),
		FROM(x11, SetIOErrorHandler, true),
		{ x11, "XkbSetDetectableAutoRepeat", (void**) &X.SetDetectableAutoRepeat, false },
		FROM(x11, CreateBitmapFromData, true),
		FROM(x11, FreePixmap, true),
		FROM(xext, ShapeCombineMask, false),
		FROM(xext, ShmQueryExtension, false),
		FROM(xext, ShmCreateImage, false),
		FROM(xext, ShmAttach, false),
		FROM(xext, ShmDetach, false),
		FROM(xext, ShmPutImage, false),
#undef FROM
	};
	for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
		*symbols[i].slot = symbols[i].library ? dlsym(symbols[i].library, symbols[i].name) : NULL;
		if (!*symbols[i].slot && symbols[i].isRequired) {
			snprintf(error, errorSize, "libX11 lacks %s", symbols[i].name);
			return false;
		}
	}
	X.isLoaded = true;
	return true;
}

// --- The window --------------------------------------------------------------

struct GameWindow {
	Display* display;
	int screen;
	Window window;
	Window root;
	Visual* visual;
	int depth;
	GC gc;
	Atom deleteWindow;
	Atom activeWindow;
	Atom wmState;
	Atom wmStateAbove;
	Atom wmName;
	Atom utf8String;
	Atom wmIcon;
	Atom wmPid;

	int width;
	int height;
	int x;
	int y;
	bool hasPosition;
	bool isShown;
	bool needsDrawing;

	XImage* image;
	XShmSegmentInfo shm;
	bool isShm;
	bool canShm;

	uint32_t keys;
	bool pressed[256];
	int redShift;
	int greenShift;
	int blueShift;

	// Where the drawer docks, the width of the desktop it docks in, and the
	// two lines of status it shows under the game, in Latin-1 for X's fonts
	int dockX;
	int dockY;
	int dockWidth;
	int dockHeight;
	int workWidth;
	Atom wmStates[3];
	GC textGc;
	bool hasFont;
	int lineHeight;
	int ascent;
	int charWidth;
	char lines[2][192];

	// The desktop's work area, and the box of it the window is laid over
	// instead of docking, when it is
	int workX;
	int workY;
	int workHeight;
	bool isOverlay;
	int overlayX;
	int overlayY;
	int overlayWidth;
	int overlayHeight;

	// Asks for the keyboard once mapped
	bool isActivating;

	// Fading in (1) or out (-1) since `fadeStartMs`, or still (0)
	int fade;
	long long fadeStartMs;
	Atom opacity;

	// The console around the game, drawn at its own size, then `scale` times
	// that at `consoleX`, `consoleY`; `drawn` is what it was drawn with, to
	// draw again only when a button changes
	struct ConsoleShape shape;
	int scale;
	int consoleX;
	int consoleY;
	uint32_t art[CONSOLE_MAX_WIDTH * CONSOLE_MAX_HEIGHT];
	uint32_t drawn;
	// The window cut to the console's outline, at this size; 0, not cut
	int cutWidth;
	int cutScale;
	// Buttons held with the mouse, the speed button among them, and the speed
	uint32_t mouseKeys;
	bool isSpeedHeld;
	int speed;
};

#define FADE_MS 160
// How far the drawer's game slides in from as it fades in
#define SLIDE_PX 48

static long long nowMs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// The window's place: laid over its box, or docked
static void targetRect(const struct GameWindow* window, int* x, int* y, int* width, int* height) {
	if (window->isOverlay) {
		*x = window->overlayX;
		*y = window->overlayY;
		*width = window->overlayWidth;
		*height = window->overlayHeight;
	} else {
		*x = window->dockX;
		*y = window->dockY;
		*width = window->dockWidth;
		*height = window->dockHeight;
	}
}

static void setOpacity(struct GameWindow* window, double alpha) {
	alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
	const unsigned long value = (unsigned long) (alpha * 0xFFFFFFFFu);
	X.ChangeProperty(window->display, window->window, window->opacity, XA_CARDINAL, 32, PropModeReplace,
	                 (const unsigned char*) &value, 1);
}

#define DRAWER_MARGIN 16
#define DRAWER_COLOR 0x1C1C22u
#define TEXT_COLOR 0xD8D8E0u
#define DIM_TEXT_COLOR 0x8C8C98u

static int textArea(const struct GameWindow* window) {
	return window->hasFont && !window->isOverlay ? 2 * window->lineHeight + 14 : 0;
}

static unsigned long pixelOf(const struct GameWindow* window, uint32_t rgb) {
	return ((rgb >> 16) & 0xFF) << window->redShift | ((rgb >> 8) & 0xFF) << window->greenShift |
	       (rgb & 0xFF) << window->blueShift;
}

// X errors arrive later than the call that made them: these note the last
// one, so a failed shared-memory attach can fall back
static volatile bool hasXError = false;

static int onXError(Display* display, void* event) {
	(void) display;
	(void) event;
	hasXError = true;
	return 0;
}

// The display went away (the desktop ended): nothing more can be drawn, and
// Xlib ends the process when this returns, so the engine saves first
static void (*onLost)(void) = NULL;

static int onXIOError(Display* display) {
	(void) display;
	if (onLost) {
		onLost();
	}
	_exit(0);
}

void gameWindowOnLost(void (*callback)(void)) {
	onLost = callback;
}

static int shiftOf(unsigned long mask) {
	int shift = 0;
	while (mask && !(mask & 1)) {
		mask >>= 1;
		shift++;
	}
	return shift;
}

// A Game Boy, for the window's icon
static void drawIcon(unsigned long* icon, int size) {
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			uint32_t pixel = 0;
			const bool isBody = x >= 6 && x <= 25 && y >= 1 && y <= 30 && !((x == 6 || x == 25) && (y == 1 || y == 30));
			if (isBody) {
				pixel = 0xFFC8C8D0;
			}
			if (x >= 8 && x <= 23 && y >= 3 && y <= 15) {
				pixel = 0xFF505064;
			}
			if (x >= 10 && x <= 21 && y >= 5 && y <= 13) {
				pixel = 0xFF9BBC0F;
			}
			const bool isPad = (x >= 9 && x <= 13 && y == 21) || (x == 11 && y >= 19 && y <= 23);
			if (isPad) {
				pixel = 0xFF303030;
			}
			const bool isA = (x - 22) * (x - 22) + (y - 20) * (y - 20) <= 2;
			const bool isB = (x - 18) * (x - 18) + (y - 22) * (y - 22) <= 2;
			if (isA || isB) {
				pixel = 0xFFA02050;
			}
			icon[y * size + x] = pixel;
		}
	}
}

// The status, two lines apart, as Latin-1 for X's fonts and WM_NAME: what
// Latin-1 lacks becomes a dash
static void setTitle(struct GameWindow* window, const char* utf8) {
	memset(window->lines, 0, sizeof(window->lines));
	int line = 0;
	size_t length = 0;
	for (const unsigned char* c = (const unsigned char*) utf8; *c;) {
		unsigned codepoint = *c++;
		if (codepoint >= 0xC0) {
			const int extra = codepoint >= 0xF0 ? 3 : codepoint >= 0xE0 ? 2 : 1;
			codepoint &= 0x3F >> extra;
			for (int i = 0; i < extra && (*c & 0xC0) == 0x80; i++) {
				codepoint = (codepoint << 6) | (*c++ & 0x3F);
			}
		}
		if (codepoint == '\n') {
			if (line == 0) {
				line = 1;
				length = 0;
			}
			continue;
		}
		if (length < sizeof(window->lines[0]) - 1) {
			window->lines[line][length++] = (char) (codepoint <= 0xFF ? codepoint : '-');
		}
	}
	char name[400];
	snprintf(name, sizeof(name), "%s%s%s", window->lines[0], window->lines[1][0] ? " - " : "", window->lines[1]);
	X.StoreName(window->display, window->window, name);
	char joined[512];
	size_t out = 0;
	for (const char* c = utf8; *c && out < sizeof(joined) - 4; c++) {
		if (*c == '\n') {
			memcpy(joined + out, " \xC2\xB7 ", 4);
			out += 4;
		} else {
			joined[out++] = *c;
		}
	}
	joined[out] = '\0';
	X.ChangeProperty(window->display, window->window, window->wmName, window->utf8String, 8, PropModeReplace,
	                 (const unsigned char*) joined, (int) out);
	window->needsDrawing = true;
}

// The drawer's size, as tall as the work area and wide enough for the game
// at a whole multiple of its size, and where it docks: the right edge
static void dock(struct GameWindow* window, int scale) {
	long area[4] = { 0, 0, X.DisplayWidth(window->display, window->screen), X.DisplayHeight(window->display, window->screen) };
	Atom type = 0;
	int format = 0;
	unsigned long count = 0;
	unsigned long after = 0;
	unsigned char* data = NULL;
	const Atom workArea = X.InternAtom(window->display, "_NET_WORKAREA", 0);
	if (X.GetWindowProperty(window->display, window->root, workArea, 0, 4, 0, 0, &type, &format, &count, &after, &data) == 0 &&
	    data) {
		if (format == 32 && count >= 4) {
			memcpy(area, data, sizeof(area));
		}
		X.Free(data);
	}
	const int areaWidth = (int) area[2];
	const int areaHeight = (int) area[3];
	// The console as large as fits the desktop's height, at a whole multiple
	// of its size, and never more than half its width
	struct ConsoleShape shape;
	const int fits =
		consoleFit(areaWidth / 2 - 2 * DRAWER_MARGIN, areaHeight - 2 * DRAWER_MARGIN - textArea(window), &shape);
	scale = scale < 1 || scale > fits ? fits : scale;
	scale = scale > 8 ? 8 : scale;
	window->width = shape.width * scale + 2 * DRAWER_MARGIN;
	window->height = areaHeight > 0 ? areaHeight : shape.height * scale + 2 * DRAWER_MARGIN + textArea(window);
	window->dockX = (int) area[0] + areaWidth - window->width;
	window->dockY = (int) area[1];
	window->dockWidth = window->width;
	window->dockHeight = window->height;
	window->workX = (int) area[0];
	window->workY = (int) area[1];
	window->workWidth = areaWidth;
	window->workHeight = areaHeight;
}

static void setDockHints(struct GameWindow* window) {
	XSizeHints hints = { 0 };
	hints.flags = USPosition | PPosition | USSize | PSize | PMinSize | PMaxSize;
	targetRect(window, &hints.x, &hints.y, &hints.width, &hints.height);
	hints.minWidth = hints.maxWidth = hints.width;
	hints.minHeight = hints.maxHeight = hints.height;
	X.SetWMNormalHints(window->display, window->window, &hints);
}

void gameWindowWorkArea(struct GameWindow* window, int* x, int* y, int* width, int* height) {
	*x = window->workX;
	*y = window->workY;
	*width = window->workWidth;
	*height = window->workHeight;
}

void gameWindowPlace(struct GameWindow* window, int boxX, int boxY, int boxWidth, int boxHeight) {
	const bool isOverlay = boxWidth > 0 && boxHeight > 0;
	// The console as large as fits the box at a whole multiple, in its middle
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	if (isOverlay) {
		struct ConsoleShape shape;
		const int scale = consoleFit(boxWidth, boxHeight, &shape);
		width = shape.width * scale;
		height = shape.height * scale;
		x = boxX + (boxWidth - width) / 2;
		y = boxY + (boxHeight - height) / 2;
	}
	if (isOverlay == window->isOverlay && (!isOverlay || (x == window->overlayX && y == window->overlayY &&
	                                                     width == window->overlayWidth && height == window->overlayHeight))) {
		return;
	}
	window->isOverlay = isOverlay;
	window->overlayX = x;
	window->overlayY = y;
	window->overlayWidth = width;
	window->overlayHeight = height;
	setDockHints(window);
	int targetX;
	int targetY;
	int targetWidth;
	int targetHeight;
	targetRect(window, &targetX, &targetY, &targetWidth, &targetHeight);
	X.MoveResizeWindow(window->display, window->window, targetX, targetY, (unsigned) targetWidth, (unsigned) targetHeight);
	window->width = targetWidth;
	window->height = targetHeight;
	window->needsDrawing = true;
	X.Flush(window->display);
}

// X's own fonts: the larger where the server has it
static void loadFont(struct GameWindow* window) {
	static const struct {
		const char* name;
		int lineHeight;
		int ascent;
		int charWidth;
	} FONTS[] = { { "9x15", 18, 12, 9 }, { "fixed", 15, 10, 6 } };
	for (size_t i = 0; i < sizeof(FONTS) / sizeof(FONTS[0]); i++) {
		hasXError = false;
		const XID font = X.LoadFont(window->display, FONTS[i].name);
		X.Sync(window->display, 0);
		if (font && !hasXError) {
			window->textGc = X.CreateGC(window->display, window->window, 0, NULL);
			X.SetFont(window->display, window->textGc, font);
			window->hasFont = true;
			window->lineHeight = FONTS[i].lineHeight;
			window->ascent = FONTS[i].ascent;
			window->charWidth = FONTS[i].charWidth;
			return;
		}
	}
}

void gameWindowDrawer(struct GameWindow* window, int* width, int* workWidth) {
	*width = window->width;
	*workWidth = window->workWidth;
}

struct GameWindow* gameWindowOpen(int scale, char* error, size_t errorSize) {
	if (!getenv("DISPLAY")) {
		snprintf(error, errorSize, "no desktop to open a window on (DISPLAY is unset)");
		return NULL;
	}
	if (!loadX11(error, errorSize)) {
		return NULL;
	}
	X.SetErrorHandler(onXError);
	X.SetIOErrorHandler(onXIOError);
	Display* display = X.OpenDisplay(NULL);
	if (!display) {
		snprintf(error, errorSize, "can't reach the display %s", getenv("DISPLAY"));
		return NULL;
	}

	struct GameWindow* window = calloc(1, sizeof(*window));
	window->display = display;
	window->screen = X.DefaultScreen(display);
	window->root = X.RootWindow(display, window->screen);
	window->visual = X.DefaultVisual(display, window->screen);
	window->depth = X.DefaultDepth(display, window->screen);
	window->gc = X.DefaultGC(display, window->screen);
	window->redShift = shiftOf(window->visual->redMask);
	window->greenShift = shiftOf(window->visual->greenMask);
	window->blueShift = shiftOf(window->visual->blueMask);
	if (window->depth < 24 || window->visual->redMask != (0xFFUL << window->redShift) ||
	    window->visual->greenMask != (0xFFUL << window->greenShift) ||
	    window->visual->blueMask != (0xFFUL << window->blueShift)) {
		snprintf(error, errorSize, "the display's colors aren't 24-bit");
		X.CloseDisplay(display);
		free(window);
		return NULL;
	}

	// The font's height sets aside room for the status: sized with a stand-in
	// window, measured again once the real one exists
	window->hasFont = true;
	window->lineHeight = 18;
	dock(window, scale);
	window->hasFont = false;
	window->window = X.CreateSimpleWindow(display, window->root, window->dockX, window->dockY, (unsigned) window->width,
	                                      (unsigned) window->height, 0, 0, pixelOf(window, DRAWER_COLOR));
	loadFont(window);
	X.SelectInput(display, window->window,
	              KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | ExposureMask | StructureNotifyMask |
	                  FocusChangeMask);

	window->deleteWindow = X.InternAtom(display, "WM_DELETE_WINDOW", 0);
	X.SetWMProtocols(display, window->window, &window->deleteWindow, 1);
	window->activeWindow = X.InternAtom(display, "_NET_ACTIVE_WINDOW", 0);
	window->wmState = X.InternAtom(display, "_NET_WM_STATE", 0);
	window->wmStateAbove = X.InternAtom(display, "_NET_WM_STATE_ABOVE", 0);
	window->wmName = X.InternAtom(display, "_NET_WM_NAME", 0);
	window->utf8String = X.InternAtom(display, "UTF8_STRING", 0);
	window->wmIcon = X.InternAtom(display, "_NET_WM_ICON", 0);
	window->wmPid = X.InternAtom(display, "_NET_WM_PID", 0);
	window->opacity = X.InternAtom(display, "_NET_WM_WINDOW_OPACITY", 0);

	// A drawer: docked where it's told, its own size, with no title bar,
	// above the other windows and out of the taskbar
	setDockHints(window);
	const Atom motif = X.InternAtom(display, "_MOTIF_WM_HINTS", 0);
	const unsigned long noDecorations[5] = { 2, 0, 0, 0, 0 };
	X.ChangeProperty(display, window->window, motif, motif, 32, PropModeReplace, (const unsigned char*) noDecorations, 5);
	window->wmStates[0] = window->wmStateAbove;
	window->wmStates[1] = X.InternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", 0);
	window->wmStates[2] = X.InternAtom(display, "_NET_WM_STATE_SKIP_PAGER", 0);
	XClassHint classHint = { "claudeboy", "ClaudeBoy" };
	X.SetClassHint(display, window->window, &classHint);
	unsigned long pid = (unsigned long) getpid();
	X.ChangeProperty(display, window->window, window->wmPid, XA_CARDINAL, 32, PropModeReplace, (const unsigned char*) &pid, 1);
	static unsigned long icon[2 + 32 * 32];
	icon[0] = icon[1] = 32;
	drawIcon(icon + 2, 32);
	X.ChangeProperty(display, window->window, window->wmIcon, XA_CARDINAL, 32, PropModeReplace, (const unsigned char*) icon,
	                 2 + 32 * 32);
	setTitle(window, "ClaudeBoy");
	if (X.SetDetectableAutoRepeat) {
		// A held key presses once and lets go once, however long it's held
		X.SetDetectableAutoRepeat(display, 1, NULL);
	}
	window->canShm = X.ShmQueryExtension && X.ShmCreateImage && X.ShmAttach && X.ShmPutImage &&
	                 X.ShmQueryExtension(display);
	X.Flush(display);
	return window;
}

void gameWindowShow(struct GameWindow* window, bool isActivating) {
	if (window->isShown) {
		// Fading out: fade back in from as far as it got
		if (window->fade < 0) {
			const long long left = FADE_MS - (nowMs() - window->fadeStartMs);
			window->fade = 1;
			window->fadeStartMs = nowMs() - (left > 0 ? left : 0);
		}
		return;
	}
	window->isActivating = isActivating;
	// Each time in its place, above the other windows, fading in
	X.ChangeProperty(window->display, window->window, window->wmState, XA_ATOM, 32, PropModeReplace,
	                 (const unsigned char*) window->wmStates, 3);
	setDockHints(window);
	int x;
	int y;
	int width;
	int height;
	targetRect(window, &x, &y, &width, &height);
	X.MoveResizeWindow(window->display, window->window, x, y, (unsigned) width, (unsigned) height);
	window->width = width;
	window->height = height;
	setOpacity(window, 0);
	X.MapRaised(window->display, window->window);
	X.Flush(window->display);
	window->isShown = true;
	window->needsDrawing = true;
	window->fade = 1;
	window->fadeStartMs = nowMs();
}

// Fades out, then unmaps; the keys let go at once
void gameWindowHide(struct GameWindow* window, bool isInstant) {
	window->keys = 0;
	window->mouseKeys = 0;
	window->isSpeedHeld = false;
	memset(window->pressed, 0, sizeof(window->pressed));
	if (!window->isShown) {
		return;
	}
	if (isInstant) {
		X.UnmapWindow(window->display, window->window);
		X.Flush(window->display);
		window->isShown = false;
		window->fade = 0;
		return;
	}
	if (window->fade < 0) {
		return;
	}
	window->fade = -1;
	window->fadeStartMs = nowMs();
}

// Steps a fade along: called with every pump
static void animate(struct GameWindow* window) {
	if (!window->fade) {
		return;
	}
	const double t = (double) (nowMs() - window->fadeStartMs) / FADE_MS;
	if (t >= 1) {
		if (window->fade < 0) {
			X.UnmapWindow(window->display, window->window);
			window->isShown = false;
		}
		setOpacity(window, 1);
		window->fade = 0;
	} else {
		setOpacity(window, window->fade > 0 ? t : 1 - t);
	}
	window->needsDrawing = true;
	X.Flush(window->display);
}

void gameWindowSetSpeed(struct GameWindow* window, int speed) {
	if (speed != window->speed) {
		window->speed = speed;
		window->needsDrawing = true;
	}
}

void gameWindowSetTitle(struct GameWindow* window, const char* utf8) {
	setTitle(window, utf8);
	X.Flush(window->display);
}

// Asks the window manager to give the window the keys, once it's mapped
static void activate(struct GameWindow* window) {
	XEvent event = { 0 };
	event.xclient.type = ClientMessage;
	event.xclient.sendEvent = 1;
	event.xclient.display = window->display;
	event.xclient.window = window->window;
	event.xclient.messageType = window->activeWindow;
	event.xclient.format = 32;
	// From a pager, which window managers trust to move the focus
	event.xclient.data.l[0] = 2;
	event.xclient.data.l[1] = 0;
	X.SendEvent(window->display, window->root, 0, SubstructureRedirectMask | SubstructureNotifyMask, &event);
	X.Flush(window->display);
}

static int buttonOf(KeySym symbol) {
	switch (symbol) {
	case 0x7A: // z
	case 0x5A: // Z
		return 0; // A
	case 0x78: // x
	case 0x58: // X
		return 1; // B
	case 0xFF08: // BackSpace
		return 2; // Select
	case 0xFF0D: // Return
	case 0xFF8D: // KP_Enter
		return 3; // Start
	case 0xFF53:
		return 4; // Right
	case 0xFF51:
		return 5; // Left
	case 0xFF52:
		return 6; // Up
	case 0xFF54:
		return 7; // Down
	default:
		return -1;
	}
}

void gameWindowPump(struct GameWindow* window, struct GameWindowEvents* events) {
	animate(window);
	memset(events, 0, sizeof(*events));
	events->focus = -1;
	while (X.Pending(window->display)) {
		XEvent event;
		X.NextEvent(window->display, &event);
		switch (event.type) {
		case KeyPress:
		case KeyRelease: {
			const unsigned keycode = event.xkey.keycode & 0xFF;
			const KeySym symbol = X.LookupKeysym(&event.xkey, 0);
			const int button = buttonOf(symbol);
			if (event.type == KeyRelease) {
				window->pressed[keycode] = false;
				if (button >= 0) {
					window->keys &= ~(1u << button);
				}
				break;
			}
			// A held key's repeats press nothing more
			if (window->pressed[keycode]) {
				break;
			}
			window->pressed[keycode] = true;
			if (symbol == 0xFF1B) {
				// Escape: back to Claude, as closing the drawer
				events->isClosed = true;
			} else if (button >= 0) {
				window->keys |= 1u << button;
			} else if (((symbol >= 'a' && symbol <= 'z') || (symbol >= '0' && symbol <= '9')) &&
			           events->typedCount < WINDOW_TYPED_MAX) {
				events->typed[events->typedCount++] = (char) symbol;
			}
			break;
		}
		case ButtonPress:
		case ButtonRelease: {
			if (event.xbutton.button != 1 || window->scale < 1) {
				break;
			}
			// The console's buttons press as they're clicked, held while held
			if (event.type == ButtonRelease) {
				window->mouseKeys = 0;
				window->isSpeedHeld = false;
				window->needsDrawing = true;
				break;
			}
			const int button = consoleButtonAt(window->shape, (double) (event.xbutton.x - window->consoleX) / window->scale,
			                                   (double) (event.xbutton.y - window->consoleY) / window->scale);
			if (button == CONSOLE_SPEED) {
				window->isSpeedHeld = true;
				if (events->typedCount < WINDOW_TYPED_MAX) {
					events->typed[events->typedCount++] = 'f';
				}
			} else if (button >= 0) {
				window->mouseKeys |= 1u << button;
			}
			window->needsDrawing = true;
			break;
		}
		case FocusIn:
			events->focus = 1;
			break;
		case FocusOut:
			// Keys let go elsewhere would otherwise stay held
			window->keys = 0;
			window->mouseKeys = 0;
			window->isSpeedHeld = false;
			memset(window->pressed, 0, sizeof(window->pressed));
			events->focus = 0;
			break;
		case ConfigureNotify:
			if (event.xconfigure.width != window->width || event.xconfigure.height != window->height) {
				window->width = event.xconfigure.width;
				window->height = event.xconfigure.height;
				window->needsDrawing = true;
			}
			// The window manager's own says where the window is on the screen
			if (event.xconfigure.sendEvent) {
				window->x = event.xconfigure.x;
				window->y = event.xconfigure.y;
				window->hasPosition = true;
			}
			break;
		case Expose:
			window->needsDrawing = true;
			break;
		case MapNotify:
			if (window->isActivating) {
				activate(window);
			}
			window->needsDrawing = true;
			break;
		case ClientMessage:
			if ((Atom) event.xclient.data.l[0] == window->deleteWindow) {
				events->isClosed = true;
			}
			break;
		}
	}
	events->keys = window->keys | window->mouseKeys;
	events->needsDrawing = window->needsDrawing && window->isShown;
	window->needsDrawing = false;
}

static void freeImage(struct GameWindow* window) {
	if (!window->image) {
		return;
	}
	if (window->isShm) {
		X.ShmDetach(window->display, &window->shm);
		X.Sync(window->display, 0);
		shmdt(window->shm.shmaddr);
	} else {
		free(window->image->data);
	}
	window->image->data = NULL;
	window->image->f.destroyImage(window->image);
	window->image = NULL;
}

// An image the window's size, in shared memory where the display allows
static bool fitImage(struct GameWindow* window) {
	if (window->image && window->image->width == window->width && window->image->height == window->height) {
		return true;
	}
	freeImage(window);
	if (window->canShm) {
		memset(&window->shm, 0, sizeof(window->shm));
		XImage* image = X.ShmCreateImage(window->display, window->visual, (unsigned) window->depth, ZPixmap, NULL, &window->shm,
		                                 (unsigned) window->width, (unsigned) window->height);
		if (image && image->bitsPerPixel == 32) {
			window->shm.shmid = shmget(IPC_PRIVATE, (size_t) image->bytesPerLine * image->height, IPC_CREAT | 0600);
			if (window->shm.shmid >= 0) {
				window->shm.shmaddr = image->data = shmat(window->shm.shmid, NULL, 0);
				window->shm.readOnly = 0;
				hasXError = false;
				const bool isAttached = window->shm.shmaddr != (char*) -1 && X.ShmAttach(window->display, &window->shm);
				X.Sync(window->display, 0);
				// Marked for removal now: gone once both sides let go
				shmctl(window->shm.shmid, IPC_RMID, NULL);
				if (isAttached && !hasXError) {
					window->image = image;
					window->isShm = true;
					return true;
				}
				if (window->shm.shmaddr != (char*) -1) {
					shmdt(window->shm.shmaddr);
				}
			}
			image->data = NULL;
			image->f.destroyImage(image);
		} else if (image) {
			image->f.destroyImage(image);
		}
		// Not here: a display across the network, or a server without it
		window->canShm = false;
	}
	char* data = malloc((size_t) window->width * window->height * 4);
	if (!data) {
		return false;
	}
	window->image = X.CreateImage(window->display, window->visual, (unsigned) window->depth, ZPixmap, 0, data,
	                              (unsigned) window->width, (unsigned) window->height, 32, 0);
	if (!window->image || window->image->bitsPerPixel != 32) {
		free(data);
		if (window->image) {
			window->image->data = NULL;
			window->image->f.destroyImage(window->image);
			window->image = NULL;
		}
		return false;
	}
	window->isShm = false;
	return true;
}

// Cuts the window to the console's outline where it's laid over the
// terminal, so the terminal shows around its rounded corners; uncut as the
// drawer
static void cut(struct GameWindow* window) {
	const int width = window->isOverlay ? window->shape.width : 0;
	if (!X.ShapeCombineMask || (width == window->cutWidth && window->scale == window->cutScale)) {
		return;
	}
	window->cutWidth = width;
	window->cutScale = window->scale;
	if (!width) {
		X.ShapeCombineMask(window->display, window->window, SHAPE_BOUNDING, 0, 0, 0, SHAPE_SET);
		return;
	}
	const int scale = window->scale;
	const int maskWidth = window->shape.width * scale;
	const int maskHeight = window->shape.height * scale;
	const int pitch = (maskWidth + 7) / 8;
	char* bits = calloc((size_t) pitch * maskHeight, 1);
	if (!bits) {
		return;
	}
	for (int y = 0; y < maskHeight; y++) {
		const uint32_t* source = &window->art[(y / scale) * window->shape.width];
		for (int x = 0; x < maskWidth; x++) {
			if (source[x / scale] >> 24) {
				bits[y * pitch + x / 8] |= (char) (1 << (x % 8));
			}
		}
	}
	const Pixmap mask = X.CreateBitmapFromData(window->display, window->window, bits, (unsigned) maskWidth, (unsigned) maskHeight);
	free(bits);
	X.ShapeCombineMask(window->display, window->window, SHAPE_BOUNDING, 0, 0, mask, SHAPE_SET);
	X.FreePixmap(window->display, mask);
}

void gameWindowDraw(struct GameWindow* window, const uint32_t* frame) {
	if (!window->isShown || window->width <= 0 || window->height <= 0 || !fitImage(window)) {
		return;
	}
	const int width = window->width;
	const int height = window->height;
	// The console and the status under it, together in the middle of the
	// drawer; laid over its box, the window is the console's own size
	const int text = textArea(window);
	const int margin = window->isOverlay ? 0 : DRAWER_MARGIN;
	struct ConsoleShape shape;
	const int scale = consoleFit(width - 2 * margin, height - 2 * margin - text, &shape);
	const int consoleWidth = shape.width * scale;
	const int consoleHeight = shape.height * scale;
	// The drawer's console slides in a little as it fades in
	int slide = 0;
	if (window->fade > 0 && !window->isOverlay) {
		double t = (double) (nowMs() - window->fadeStartMs) / FADE_MS;
		t = t > 1 ? 1 : t;
		slide = (int) ((1 - t) * (1 - t) * SLIDE_PX);
	}
	const int left = (width - consoleWidth) / 2 + slide;
	const int top = (height - consoleHeight - text) / 2;

	// The console drawn again when its shape or a button changes; else only
	// the screen in it
	const uint32_t keys = window->keys | window->mouseKeys;
	const uint32_t drawn = keys | (uint32_t) shape.isWide << 8 | (uint32_t) window->isSpeedHeld << 9 |
	                       (uint32_t) window->speed << 10 | 1u << 31;
	if (drawn != window->drawn || shape.isWide != window->shape.isWide) {
		consoleDraw(shape, frame, keys, window->isSpeedHeld, window->speed, true, window->art);
		window->drawn = drawn;
	} else {
		int screenX;
		int screenY;
		consoleScreen(shape, &screenX, &screenY);
		for (int y = 0; y < WINDOW_SCREEN_HEIGHT; y++) {
			uint32_t* row = &window->art[(screenY + y) * shape.width + screenX];
			const uint32_t* source = &frame[y * WINDOW_SCREEN_WIDTH];
			for (int x = 0; x < WINDOW_SCREEN_WIDTH; x++) {
				row[x] = source[x] | 0xFF000000u;
			}
		}
	}
	window->shape = shape;
	window->scale = scale;
	window->consoleX = left;
	window->consoleY = top;
	cut(window);

	const uint32_t border = (uint32_t) pixelOf(window, DRAWER_COLOR);
	char* data = window->image->data;
	const int pitch = window->image->bytesPerLine;
	for (int y = 0; y < height; y++) {
		uint32_t* row = (uint32_t*) (data + (size_t) y * pitch);
		const int consoleY = y - top;
		if (consoleY < 0 || consoleY >= consoleHeight) {
			for (int x = 0; x < width; x++) {
				row[x] = border;
			}
			continue;
		}
		// Each of the console's rows is `scale` rows here: the rest copy the first
		if (consoleY % scale != 0 && y > 0) {
			memcpy(row, data + (size_t) (y - 1) * pitch, (size_t) width * 4);
			continue;
		}
		const uint32_t* source = &window->art[(consoleY / scale) * shape.width];
		for (int x = 0; x < width; x++) {
			const int consoleX = x - left;
			const uint32_t argb = consoleX < 0 || consoleX >= consoleWidth ? 0 : source[consoleX / scale];
			row[x] = !(argb >> 24) ? border
			                       : ((argb >> 16) & 0xFF) << window->redShift | ((argb >> 8) & 0xFF) << window->greenShift |
			                             (argb & 0xFF) << window->blueShift;
		}
	}

	if (window->isShm) {
		X.ShmPutImage(window->display, window->window, window->gc, window->image, 0, 0, 0, 0, (unsigned) width,
		              (unsigned) height, 0);
		// The server reads the memory as it handles the request: done before
		// the next frame writes over it
		X.Sync(window->display, 0);
	} else {
		X.PutImage(window->display, window->window, window->gc, window->image, 0, 0, 0, 0, (unsigned) width, (unsigned) height);
	}
	if (window->hasFont && !window->isOverlay) {
		const int most = (width - 2 * DRAWER_MARGIN) / window->charWidth;
		for (int line = 0; line < 2; line++) {
			const int length = (int) strlen(window->lines[line]);
			X.SetForeground(window->display, window->textGc, pixelOf(window, line == 0 ? TEXT_COLOR : DIM_TEXT_COLOR));
			X.DrawString(window->display, window->window, window->textGc, left,
			             top + consoleHeight + 10 + window->ascent + line * window->lineHeight, window->lines[line],
			             length < most ? length : most);
		}
	}
	X.Flush(window->display);
}

void gameWindowClose(struct GameWindow* window) {
	if (!window) {
		return;
	}
	freeImage(window);
	X.DestroyWindow(window->display, window->window);
	X.CloseDisplay(window->display);
	free(window);
}
