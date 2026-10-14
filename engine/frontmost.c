// Watches whether this session's terminal tab is in front, over AT-SPI. A
// thread of its own: the bus answers in well under a millisecond, but it's
// another process, and the game mustn't wait on it.

#include "frontmost.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The few libatspi and GLib types used, declared here: no headers needed
typedef struct _AtspiAccessible AtspiAccessible;
typedef struct _AtspiStateSet AtspiStateSet;
typedef struct {
	unsigned domain;
	int code;
	char* message;
} GError;

enum {
	ROLE_FRAME = 23,
	ROLE_PAGE_TAB = 37,
	ROLE_TERMINAL = 60,
	STATE_ACTIVE = 1,
	STATE_SELECTED = 23,
};

static struct {
	int (*init)(void);
	AtspiAccessible* (*getDesktop)(int);
	int (*childCount)(AtspiAccessible*, GError**);
	AtspiAccessible* (*childAt)(AtspiAccessible*, int, GError**);
	AtspiAccessible* (*parent)(AtspiAccessible*, GError**);
	int (*role)(AtspiAccessible*, GError**);
	AtspiStateSet* (*stateSet)(AtspiAccessible*);
	int (*contains)(AtspiStateSet*, int);
	void (*clearCache)(AtspiAccessible*);
	int (*characterCount)(AtspiAccessible*, GError**);
	char* (*text)(AtspiAccessible*, int, int, GError**);
	void (*unref)(void*);
	void (*free)(void*);
	void (*freeError)(GError*);
	int (*iterate)(void*, int);
} A;

static atomic_int state = FRONTMOST_UNKNOWN;
static char marker[128];

static void sleepMs(int ms) {
	struct timespec pause = { ms / 1000, (long) (ms % 1000) * 1000000L };
	nanosleep(&pause, NULL);
}

static bool load(void) {
	void* atspi = dlopen("libatspi.so.0", RTLD_NOW | RTLD_LOCAL);
	void* gobject = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
	void* glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
	if (!atspi || !gobject || !glib) {
		return false;
	}
	struct {
		void* library;
		const char* name;
		void** slot;
	} symbols[] = {
		{ atspi, "atspi_init", (void**) &A.init },
		{ atspi, "atspi_get_desktop", (void**) &A.getDesktop },
		{ atspi, "atspi_accessible_get_child_count", (void**) &A.childCount },
		{ atspi, "atspi_accessible_get_child_at_index", (void**) &A.childAt },
		{ atspi, "atspi_accessible_get_parent", (void**) &A.parent },
		{ atspi, "atspi_accessible_get_role", (void**) &A.role },
		{ atspi, "atspi_accessible_get_state_set", (void**) &A.stateSet },
		{ atspi, "atspi_state_set_contains", (void**) &A.contains },
		{ atspi, "atspi_accessible_clear_cache", (void**) &A.clearCache },
		// An accessible is its own text interface
		{ atspi, "atspi_text_get_character_count", (void**) &A.characterCount },
		{ atspi, "atspi_text_get_text", (void**) &A.text },
		{ gobject, "g_object_unref", (void**) &A.unref },
		{ glib, "g_free", (void**) &A.free },
		{ glib, "g_error_free", (void**) &A.freeError },
		{ glib, "g_main_context_iteration", (void**) &A.iterate },
	};
	for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
		*symbols[i].slot = dlsym(symbols[i].library, symbols[i].name);
		if (!*symbols[i].slot) {
			return false;
		}
	}
	// 0 started, 1 already was; anything else, no bus
	const int started = A.init();
	return started == 0 || started == 1;
}

// A call's error, if any, let go of: true when there was one
static bool failed(GError* error) {
	if (!error) {
		return false;
	}
	A.freeError(error);
	return true;
}

static int roleOf(AtspiAccessible* node) {
	GError* error = NULL;
	const int role = A.role(node, &error);
	return failed(error) ? -1 : role;
}

static bool hasState(AtspiAccessible* node, int which) {
	A.clearCache(node);
	AtspiStateSet* states = A.stateSet(node);
	if (!states) {
		return false;
	}
	const bool has = A.contains(states, which) != 0;
	A.unref(states);
	return has;
}

// Whether a terminal's screen shows the marker
static bool showsMarker(AtspiAccessible* terminal) {
	GError* error = NULL;
	const int count = A.characterCount(terminal, &error);
	if (failed(error) || count <= 0) {
		return false;
	}
	char* text = A.text(terminal, 0, count, &error);
	if (failed(error) || !text) {
		return false;
	}
	const bool shows = strstr(text, marker) != NULL;
	A.free(text);
	return shows;
}

// The terminal showing the marker, under `node`, a reference the caller lets
// go of; NULL when none. Not deeper than a terminal's widgets go.
static AtspiAccessible* findTerminal(AtspiAccessible* node, int depth) {
	if (depth > 12) {
		return NULL;
	}
	if (roleOf(node) == ROLE_TERMINAL) {
		return showsMarker(node) ? node : NULL;
	}
	GError* error = NULL;
	const int count = A.childCount(node, &error);
	if (failed(error)) {
		return NULL;
	}
	for (int i = 0; i < count && i < 64; i++) {
		AtspiAccessible* child = A.childAt(node, i, &error);
		if (failed(error) || !child) {
			continue;
		}
		AtspiAccessible* found = findTerminal(child, depth + 1);
		if (found != child) {
			A.unref(child);
		}
		if (found) {
			return found;
		}
	}
	return NULL;
}

static AtspiAccessible* findOurs(void) {
	AtspiAccessible* desktop = A.getDesktop(0);
	if (!desktop) {
		return NULL;
	}
	GError* error = NULL;
	const int count = A.childCount(desktop, &error);
	AtspiAccessible* found = NULL;
	for (int i = 0; !failed(error) && i < count && !found; i++) {
		AtspiAccessible* app = A.childAt(desktop, i, &error);
		if (failed(error) || !app) {
			error = NULL;
			continue;
		}
		found = findTerminal(app, 0);
		if (found != app) {
			A.unref(app);
		}
	}
	A.unref(desktop);
	return found;
}

// In front: the window holding the terminal is the active one, and the tab
// holding it, if it's in one, the tab shown. -1 when the terminal is gone.
static int inFront(AtspiAccessible* terminal) {
	GError* error = NULL;
	bool isTabShown = true;
	AtspiAccessible* node = A.parent(terminal, &error);
	if (failed(error) || !node) {
		return -1;
	}
	for (int depth = 0; node && depth < 16; depth++) {
		const int role = roleOf(node);
		if (role < 0) {
			A.unref(node);
			return -1;
		}
		if (role == ROLE_PAGE_TAB && !hasState(node, STATE_SELECTED)) {
			isTabShown = false;
		}
		if (role == ROLE_FRAME) {
			const bool isActive = hasState(node, STATE_ACTIVE);
			A.unref(node);
			return isTabShown && isActive;
		}
		AtspiAccessible* up = A.parent(node, &error);
		A.unref(node);
		node = failed(error) ? NULL : up;
	}
	if (node) {
		A.unref(node);
	}
	return -1;
}

static void* watch(void* unused) {
	(void) unused;
	if (!load()) {
		return NULL;
	}
	AtspiAccessible* terminal = NULL;
	for (int round = 0;; round++) {
		// Messages from the bus the calls didn't take, handled so they don't pile up
		for (int i = 0; i < 64 && A.iterate(NULL, 0); i++) {
		}
		// Looked for again every second until found, and checked now and then:
		// the pane is redrawn, so the marker could go from a terminal that shows
		// something else now
		if (terminal && round % 20 == 0 && !showsMarker(terminal)) {
			A.unref(terminal);
			terminal = NULL;
		}
		if (!terminal && round % 7 == 0) {
			terminal = findOurs();
		}
		if (!terminal) {
			atomic_store(&state, FRONTMOST_UNKNOWN);
			sleepMs(150);
			continue;
		}
		const int front = inFront(terminal);
		if (front < 0) {
			A.unref(terminal);
			terminal = NULL;
			atomic_store(&state, FRONTMOST_UNKNOWN);
		} else {
			atomic_store(&state, front ? FRONTMOST_IN_FRONT : FRONTMOST_BEHIND);
		}
		sleepMs(150);
	}
	return NULL;
}

void frontmostStart(const char* text) {
	static bool isStarted = false;
	if (isStarted || !text || !text[0]) {
		return;
	}
	isStarted = true;
	strncpy(marker, text, sizeof(marker) - 1);
	pthread_t thread;
	pthread_attr_t attributes;
	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread, &attributes, watch, NULL);
	pthread_attr_destroy(&attributes);
}

enum Frontmost frontmostState(void) {
	return (enum Frontmost) atomic_load(&state);
}
