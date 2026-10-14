// Watches whether this session's terminal tab is in front, over AT-SPI. A
// thread of its own: the bus answers in well under a millisecond, but it's
// another process, and the game mustn't wait on it.

#include "frontmost.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
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
	ROLE_MENU = 33,
	ROLE_MENU_BAR = 34,
	ROLE_POPUP_MENU = 41,
	ROLE_TOOL_BAR = 63,
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
	unsigned (*processId)(AtspiAccessible*, GError**);
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
// This process's ancestors: the terminal running the session is one of them
// (GNOME Terminal's server, Konsole), so its windows are the ones to watch
static int ancestors[64];
static int ancestorCount;
static atomic_bool isShowNoted = false;
static atomic_bool isNoteAnswered = true;
// libatspi couldn't be loaded, or the bus reached: nothing will be answered
static atomic_bool hasFailed = false;
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
		{ atspi, "atspi_accessible_get_process_id", (void**) &A.processId },
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

static void readAncestors(void) {
	int pid = getppid();
	while (pid > 1 && ancestorCount < 64) {
		ancestors[ancestorCount++] = pid;
		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE* stat = fopen(path, "r");
		if (!stat) {
			break;
		}
		// "pid (name) state ppid ...": the name may hold spaces, so after its ')'
		char line[512];
		const char* after = fgets(line, sizeof(line), stat) ? strrchr(line, ')') : NULL;
		fclose(stat);
		int parent = 0;
		if (!after || sscanf(after + 1, " %*c %d", &parent) != 1) {
			break;
		}
		pid = parent;
	}
}

// An app on the bus that's one of this process's ancestors
static bool isOurApp(AtspiAccessible* app) {
	GError* error = NULL;
	const int pid = (int) A.processId(app, &error);
	if (failed(error)) {
		return false;
	}
	for (int i = 0; i < ancestorCount; i++) {
		if (ancestors[i] == pid) {
			return true;
		}
	}
	return false;
}

// Menus and tool bars hold no terminal, and many nodes: not looked through
static bool isMenu(int role) {
	return role == ROLE_MENU || role == ROLE_MENU_BAR || role == ROLE_POPUP_MENU || role == ROLE_TOOL_BAR;
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
	const int role = roleOf(node);
	if (role == ROLE_TERMINAL) {
		return showsMarker(node) ? node : NULL;
	}
	if (role < 0 || isMenu(role)) {
		return NULL;
	}
	GError* error = NULL;
	const int count = A.childCount(node, &error);
	if (failed(error)) {
		return NULL;
	}
	for (int i = 0; i < count && i < 64; i++) {
		AtspiAccessible* child = A.childAt(node, i, &error);
		if (failed(error) || !child) {
			error = NULL;
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

// The terminal showing the marker, among the apps that are this process's
// ancestors, or with `isAnyApp` all of them (a session run through
// something between, as a daemon)
static AtspiAccessible* findOurs(bool isAnyApp) {
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
		if (isAnyApp || isOurApp(app)) {
			found = findTerminal(app, 0);
		}
		if (found != app) {
			A.unref(app);
		}
	}
	A.unref(desktop);
	return found;
}

// The terminal in the shown tab of the active window, if a terminal's window
// is the active one: a reference the caller lets go of
static AtspiAccessible* findShownTerminal(AtspiAccessible* node, int depth) {
	if (depth > 12) {
		return NULL;
	}
	const int role = roleOf(node);
	if (role == ROLE_TERMINAL) {
		return node;
	}
	if (role < 0 || isMenu(role) || (role == ROLE_PAGE_TAB && !hasState(node, STATE_SELECTED))) {
		return NULL;
	}
	GError* error = NULL;
	const int count = A.childCount(node, &error);
	if (failed(error)) {
		return NULL;
	}
	for (int i = 0; i < count && i < 64; i++) {
		AtspiAccessible* child = A.childAt(node, i, &error);
		if (failed(error) || !child) {
			error = NULL;
			continue;
		}
		AtspiAccessible* found = findShownTerminal(child, depth + 1);
		if (found != child) {
			A.unref(child);
		}
		if (found) {
			return found;
		}
	}
	return NULL;
}

static AtspiAccessible* findActiveTerminal(void) {
	AtspiAccessible* desktop = A.getDesktop(0);
	if (!desktop) {
		return NULL;
	}
	GError* error = NULL;
	const int apps = A.childCount(desktop, &error);
	AtspiAccessible* found = NULL;
	for (int i = 0; !failed(error) && i < apps && !found; i++) {
		AtspiAccessible* app = A.childAt(desktop, i, &error);
		if (failed(error) || !app) {
			error = NULL;
			continue;
		}
		if (!isOurApp(app)) {
			A.unref(app);
			continue;
		}
		const int windows = A.childCount(app, &error);
		for (int j = 0; !failed(error) && j < windows && j < 32 && !found; j++) {
			AtspiAccessible* window = A.childAt(app, j, &error);
			if (failed(error) || !window) {
				error = NULL;
				continue;
			}
			if (roleOf(window) == ROLE_FRAME && hasState(window, STATE_ACTIVE)) {
				found = findShownTerminal(window, 0);
			}
			if (found != window) {
				A.unref(window);
			}
		}
		error = NULL;
		A.unref(app);
	}
	A.unref(desktop);
	return found;
}

// The window holding a terminal, and the tab holding it if it's in one:
// references the caller lets go of. False when the terminal is gone.
static bool locate(AtspiAccessible* terminal, AtspiAccessible** frame, AtspiAccessible** tab) {
	*frame = NULL;
	*tab = NULL;
	GError* error = NULL;
	AtspiAccessible* node = A.parent(terminal, &error);
	for (int depth = 0; !failed(error) && node && depth < 16; depth++) {
		const int role = roleOf(node);
		if (role < 0) {
			break;
		}
		if (role == ROLE_FRAME) {
			*frame = node;
			return true;
		}
		AtspiAccessible* up = A.parent(node, &error);
		if (role == ROLE_PAGE_TAB && !*tab) {
			*tab = node;
		} else {
			A.unref(node);
		}
		node = up;
	}
	if (node) {
		A.unref(node);
	}
	if (*tab) {
		A.unref(*tab);
		*tab = NULL;
	}
	return false;
}

static void letGo(AtspiAccessible** node) {
	if (*node) {
		A.unref(*node);
		*node = NULL;
	}
}

static void* watch(void* unused) {
	(void) unused;
	if (!load()) {
		atomic_store(&hasFailed, true);
		atomic_store(&isNoteAnswered, true);
		return NULL;
	}
	readAncestors();
	// This session's terminal, its window and its tab, and whether the
	// marker said so; until it's found, the terminal in front when the game
	// first shows: the person just started Claude working there
	AtspiAccessible* terminal = NULL;
	AtspiAccessible* frame = NULL;
	AtspiAccessible* tab = NULL;
	bool isMarked = false;
	for (int round = 0;; round++) {
		// Messages from the bus the calls didn't take, handled so they don't pile up
		for (int i = 0; i < 64 && A.iterate(NULL, 0); i++) {
		}
		// The game is about to show: the terminal in front now, before the game
		// takes the keyboard, is this session's, if nothing said otherwise
		if (atomic_exchange(&isShowNoted, false)) {
			if (!terminal) {
				terminal = findActiveTerminal();
			}
			atomic_store(&isNoteAnswered, true);
		}
		// The marker settles it: looked for while it hasn't (in every app now
		// and then), and checked now and then, in case the pane moved to
		// another terminal. A terminal once found is kept while the marker
		// shows nowhere else.
		if (round % 60 == 0 && !(isMarked && showsMarker(terminal))) {
			AtspiAccessible* marked = findOurs(round % 300 == 0);
			if (marked) {
				letGo(&terminal);
				letGo(&frame);
				letGo(&tab);
				terminal = marked;
				isMarked = true;
			}
		}
		// Its window and tab, found again now and then: a tab can be dragged
		// to another window
		if (terminal && (!frame || round % 30 == 0)) {
			letGo(&frame);
			letGo(&tab);
			if (!locate(terminal, &frame, &tab)) {
				// Gone: its tab closed
				letGo(&terminal);
				isMarked = false;
			}
		}
		if (!terminal) {
			atomic_store(&state, FRONTMOST_UNKNOWN);
		} else {
			const bool isFront = hasState(frame, STATE_ACTIVE) && (!tab || hasState(tab, STATE_SELECTED));
			atomic_store(&state, isFront ? FRONTMOST_IN_FRONT : FRONTMOST_BEHIND);
		}
		sleepMs(30);
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

void frontmostNoteShown(void) {
	if (atomic_load(&hasFailed)) {
		return;
	}
	atomic_store(&isNoteAnswered, false);
	atomic_store(&isShowNoted, true);
	// The watcher failed meanwhile: nobody will answer
	if (atomic_load(&hasFailed)) {
		atomic_store(&isNoteAnswered, true);
	}
}

bool frontmostIsNoteAnswered(void) {
	return atomic_load(&isNoteAnswered);
}

enum Frontmost frontmostState(void) {
	return (enum Frontmost) atomic_load(&state);
}
