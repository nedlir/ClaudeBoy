// Whether the terminal tab that holds this Claude Code session is in front:
// its window the active one and the tab the one shown. Read from the desktop's
// accessibility bus (AT-SPI), which terminals like GNOME Terminal and Konsole
// report to, so it works for a terminal on Wayland, whose windows X11 can't
// see. The tab is the one whose screen shows `marker`, which the mod draws in
// the pane, under the game window. libatspi is loaded at run time: without
// it, or without the bus, the answer is "can't tell".

#ifndef CLAUDEBOY_FRONTMOST_H
#define CLAUDEBOY_FRONTMOST_H

enum Frontmost {
	FRONTMOST_UNKNOWN = -1,
	FRONTMOST_BEHIND = 0,
	FRONTMOST_IN_FRONT = 1,
};

// Starts watching, on a thread of its own; once
void frontmostStart(const char* marker);
enum Frontmost frontmostState(void);

#endif
