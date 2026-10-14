// Whether the terminal tab that holds this Claude Code session is in front:
// its window the active one and the tab the one shown. Read from the desktop's
// accessibility bus (AT-SPI), which terminals like GNOME Terminal and Konsole
// report to, so it works for a terminal on Wayland, whose windows X11 can't
// see. The tab is the one whose screen shows `marker`, which the mod draws in
// the pane, under the game window; until that's found, the tab that was in
// front when the game first showed. libatspi is loaded at run time: without
// it, or without the bus, the answer is "can't tell".

#ifndef CLAUDEBOY_FRONTMOST_H
#define CLAUDEBOY_FRONTMOST_H

#include <stdbool.h>

enum Frontmost {
	FRONTMOST_UNKNOWN = -1,
	FRONTMOST_BEHIND = 0,
	FRONTMOST_IN_FRONT = 1,
};

// Starts watching, on a thread of its own; once
void frontmostStart(const char* marker);
enum Frontmost frontmostState(void);
// Whether the game is out: the bus is looked at only while it is
void frontmostSetActive(bool isGameOut);
// The game is about to show for the first time since play began: the
// terminal in front now is this session's, if the marker hasn't said which.
// Answered within a round (30 ms); show the game once it is, so the terminal
// is looked at before the game takes the keyboard from it.
void frontmostNoteShown(void);
bool frontmostIsNoteAnswered(void);

#endif
