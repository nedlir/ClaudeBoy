// ClaudeBoy's engine: one Game Boy game on mGBA's core, run without a window
// for the ClaudeBoy Claude Code mod, which draws what this writes and hands it
// keys and commands. How the two talk is in USAGE below.

#define _GNU_SOURCE

#include <mgba/core/blip_buf.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/overrides.h>
#include <mgba-util/vfs.h>

#include "window.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const char USAGE[] =
	"usage:\n"
	"  claudeboy-engine info <rom>\n"
	"      One JSON line: the game's title, whether it is made for the Game Boy\n"
	"      Color, and the key its saves are kept under.\n"
	"\n"
	"  claudeboy-engine run <rom> <saves-dir> <run-dir>\n"
	"      Plays, paused until told otherwise. <saves-dir> keeps the battery\n"
	"      save (battery.sav), the autosave (auto.state) and the slots\n"
	"      (slot1.state to slot9.state). Every frame it reads <run-dir>/control,\n"
	"      one line the mod rewrites in place, such as\n"
	"        play=1 keys=17 taps=3,0,0,0,0,0,0,0 view=cells:160x72 fps=30\n"
	"        sound=1 speed=1 cmds=7:save:2,8:load:auto\n"
	"      (view=image:<w>x<h> asks for pictures that size in pixels instead,\n"
	"      view=window for a window of its own, titled by title=<base64>, laid\n"
	"      over the terminal's cells overlay=<col>,<row>,<cols>,<rows> where the\n"
	"      terminal fills the desktop, so it looks part of it)\n"
	"      and writes one line on stdout per event:\n"
	"        @ready <save-key> <color 0|1> <title>\n"
	"        @term <cols> <rows> <x-pixels> <y-pixels>  the terminal's size\n"
	"        @cells <seq> <cols> <rows>      <run-dir>/cells holds a new frame\n"
	"        @frame <shm-name> <w> <h>       a new RGBA frame in shared memory\n"
	"        @done <id> ok|fail <words>      a command has run\n"
	"        @saved <slot> <unix-ms>         a save state was written\n"
	"        @playing, @paused\n"
	"        @busy                           another session is playing it\n"
	"        @key <letter or digit>          typed in the game's window\n"
	"        @closed                         the person closed the window (Esc)\n"
	"        @drawer <width> <desktop-width> the window's width, in pixels\n"
	"        @cell <width> <height>          a terminal cell, in pixels, when told\n"
	"        @focus 1|0                      the window got or lost the keys\n"
	"        @nowindow <words>               no window can open here\n"
	"        @log <words>, @error <words>\n"
	"        @bye                            saved, and gone\n"
	"\n"
	"  claudeboy-engine script <rom> <saves-dir> <out-dir> <step>...\n"
	"      Runs steps as fast as it can, for testing: <n> runs n frames;\n"
	"      +a and -a hold and let go of a button (a b select start right left\n"
	"      up down); tap:a taps one; shot:<name> writes <out-dir>/<name>.ppm;\n"
	"      cells:<cols>x<rows>:<name> writes <out-dir>/<name>.cells as the mod\n"
	"      would get them; image:<w>x<h>:<name> writes the picture frame as\n"
	"      <out-dir>/<name>.ppm, its see-through edge magenta; save:<slot> and\n"
	"      load:<slot>; bench:<n> times n frames.\n";

#define WIDTH 160
#define HEIGHT 144
// Pixels per row of the core's video buffer, with room for a Super Game Boy border
#define STRIDE 256
#define BUTTONS 8

#define SAMPLE_RATE 48000
#define AUDIO_SAMPLES 4096
// Sound written ahead of the player, in bytes: enough to ride out a late frame
#define AUDIO_QUEUE_TARGET (SAMPLE_RATE * 4 / 25)

// A tap shows the game a press at least this many frames long, however soon
// its release comes after it
#define TAP_FRAMES 4
// While someone plays, the game is also saved this often, in case the engine
// is ended without the chance to
#define AUTOSAVE_SECONDS 120
// A frame the terminal never read is never unlinked by it, so the oldest
// leftovers go once this many newer frames exist: half a second's, room for
// a terminal that falls behind for a moment and for frames the mod skipped
#define FRAMES_IN_FLIGHT 32

#define MAX_CELL_COLUMNS 512
#define MAX_CELL_ROWS 256
// A picture frame is drawn at the size the terminal shows it, so the terminal
// never scales it; past this many pixels it's drawn smaller, and the terminal
// scales the rest
#define MAX_IMAGE_PIXELS 800000
#define MAX_IMAGE_SIDE 4096
// Frames up to this many pixels come up to 60 a second, larger ones 30
#define SMOOTH_IMAGE_PIXELS 450000
// How often the terminal's size is looked at, for the mod to fit the picture
#define TERMINAL_CHECK_NS 500000000LL
#define MAX_COMMANDS 16
#define CONTROL_MAX 4096

// One Game Boy frame: 70224 cycles at 4194304 Hz
static const int64_t FRAME_NS = 16742706;

static const char* const BUTTON_NAMES[BUTTONS] = { "a", "b", "select", "start", "right", "left", "up", "down" };

// Players tried in order for the sound, raw 16-bit stereo on their stdin
static const char* const AUDIO_PLAYERS[][16] = {
	{ "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-c", "2", "-r", "48000", "-B", "100000", "-", NULL },
	{ "pacat", "--raw", "--format=s16le", "--rate=48000", "--channels=2", "--latency-msec=100", NULL },
	{ "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "-", NULL },
};

enum ViewKind {
	VIEW_NONE,
	// Half-block cells for the mod's Raster: two pixels a cell, any terminal
	VIEW_CELLS,
	// Raw RGB in shared memory for the mod's Image: terminals with pictures
	VIEW_IMAGE,
	// A window of its own, for terminals without pictures
	VIEW_WINDOW,
};

struct Control {
	bool isPlaying;
	uint32_t keys;
	uint32_t taps[BUTTONS];
	enum ViewKind view;
	int columns;
	int rows;
	int boxWidth;
	int boxHeight;
	int fps;
	bool isSoundOn;
	int speed;
	char title[256];
	// A box of the terminal's cells to lay the window over: column, row,
	// columns, rows; no columns, none
	int overlay[4];
};

struct Command {
	long id;
	char name[16];
	char arg[32];
};

struct RomInfo {
	char title[17];
	char key[64];
	bool isColor;
	uint32_t crc32;
};

struct Engine {
	struct mCore* core;
	color_t* video;
	struct RomInfo rom;
	char saves[PATH_MAX];
	char run[PATH_MAX];
	char control[PATH_MAX];

	// What the control file said last, and the state that follows from it
	char line[CONTROL_MAX];
	struct Control wanted;
	bool hasTaps;
	uint32_t taps[BUTTONS];
	uint64_t tapUntil[BUTTONS];
	long lastCommand;

	uint64_t frames;
	bool isPlaying;
	bool hasPlayedSinceSave;
	int64_t lastSaveNs;

	// The lock on the game's saves: held while this engine plays or changes
	// them, so sessions take turns, never write over each other, and each
	// picks up where the last left off. Also the autosave as this engine last
	// wrote or read it, to tell when another session has played since.
	int lock;
	bool hasLock;
	bool isBusyReported;
	struct stat autosave;
	bool hasAutosave;

	// The frame last handed to the mod, and how
	color_t shown[WIDTH * HEIGHT];
	bool hasShown;
	bool mustShow;
	enum ViewKind shownView;
	int shownColumns;
	int shownRows;
	int shownBoxWidth;
	int shownBoxHeight;
	int64_t lastShownNs;
	unsigned seq;
	unsigned shmFrames;

	// The game's window, its title, and the buttons held in it
	struct GameWindow* window;
	bool isWindowBroken;
	bool isWindowDismissed;
	uint32_t windowKeys;
	char windowTitle[256];
	// A terminal cell's size as last told to the mod
	int cellWidth;
	int cellHeight;

	// The terminal Claude Code draws in, read for its size alone
	int tty;
	struct winsize terminal;
	int64_t lastTerminalCheckNs;

	int audioFd;
	bool isAudioBroken;
	int16_t audio[AUDIO_SAMPLES * 2];
};

static volatile sig_atomic_t isStopRequested = 0;

static void requestStop(int signal) {
	(void) signal;
	isStopRequested = 1;
}

static void emit(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	putchar('\n');
	fflush(stdout);
}

static void quietLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger;
	(void) category;
	if (!(level & (mLOG_FATAL | mLOG_ERROR))) {
		return;
	}
	fputs("mgba: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

static struct mLogger logger = { .log = quietLog };

static int64_t monotonicNs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t) now.tv_sec * 1000000000LL + now.tv_nsec;
}

static long long unixMs(void) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return (long long) now.tv_sec * 1000LL + now.tv_nsec / 1000000;
}

static void sleepUntil(int64_t deadlineNs) {
	for (int64_t left = deadlineNs - monotonicNs(); left > 0 && !isStopRequested; left = deadlineNs - monotonicNs()) {
		struct timespec wait = { .tv_sec = left / 1000000000LL, .tv_nsec = left % 1000000000LL };
		nanosleep(&wait, NULL);
	}
}

static void sleepMs(int ms) {
	sleepUntil(monotonicNs() + (int64_t) ms * 1000000LL);
}

static int clampInt(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

// mkdir -p
static bool makeDirectories(const char* path) {
	char partial[PATH_MAX];
	if (snprintf(partial, sizeof(partial), "%s", path) >= (int) sizeof(partial)) {
		return false;
	}
	for (char* cursor = partial + 1; *cursor; cursor++) {
		if (*cursor != '/') {
			continue;
		}
		*cursor = '\0';
		if (mkdir(partial, 0700) != 0 && errno != EEXIST) {
			return false;
		}
		*cursor = '/';
	}
	return mkdir(partial, 0700) == 0 || errno == EEXIST;
}

static uint32_t crc32Of(const uint8_t* data, size_t size) {
	static uint32_t table[256];
	static bool hasTable = false;
	if (!hasTable) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t value = i;
			for (int bit = 0; bit < 8; bit++) {
				value = value & 1 ? (value >> 1) ^ 0xEDB88320u : value >> 1;
			}
			table[i] = value;
		}
		hasTable = true;
	}
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; i++) {
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

// The title from the cartridge header, and a save key made of it and the
// ROM's CRC-32, so two dumps or versions of one game keep separate saves
static bool readRomInfo(const char* path, struct RomInfo* info, char* error, size_t errorSize) {
	memset(info, 0, sizeof(*info));
	FILE* file = fopen(path, "rb");
	if (!file) {
		snprintf(error, errorSize, "can't open the ROM: %s", strerror(errno));
		return false;
	}
	// The largest Game Boy cartridges hold 8 MiB
	const size_t maxSize = 8 * 1024 * 1024;
	uint8_t* data = malloc(maxSize + 1);
	size_t size = data ? fread(data, 1, maxSize + 1, file) : 0;
	fclose(file);
	if (!data || size < 0x150 || size > maxSize) {
		free(data);
		snprintf(error, errorSize, "that isn't a Game Boy ROM");
		return false;
	}

	info->isColor = (data[0x143] & 0x80) != 0;
	// Color games keep the last bytes of the title for their flag
	const int titleLength = info->isColor ? 15 : 16;
	int length = 0;
	for (int i = 0; i < titleLength; i++) {
		const uint8_t c = data[0x134 + i];
		if (c == 0) {
			break;
		}
		info->title[length++] = c >= 0x20 && c < 0x7F ? (char) c : ' ';
	}
	while (length > 0 && info->title[length - 1] == ' ') {
		length--;
	}
	info->title[length] = '\0';

	info->crc32 = crc32Of(data, size);
	free(data);

	char slug[32];
	int slugLength = 0;
	bool isGap = false;
	for (int i = 0; info->title[i] && slugLength < (int) sizeof(slug) - 2; i++) {
		const unsigned char c = (unsigned char) info->title[i];
		if (isalnum(c)) {
			if (isGap && slugLength > 0) {
				slug[slugLength++] = '-';
			}
			slug[slugLength++] = (char) tolower(c);
			isGap = false;
		} else {
			isGap = true;
		}
	}
	slug[slugLength] = '\0';
	snprintf(info->key, sizeof(info->key), "%s-%08x", slugLength ? slug : "game", info->crc32);
	return true;
}

static void printJsonString(const char* text) {
	putchar('"');
	for (const char* c = text; *c; c++) {
		if (*c == '"' || *c == '\\') {
			putchar('\\');
		}
		putchar(*c);
	}
	putchar('"');
}

static int info(const char* path) {
	struct RomInfo rom;
	char error[256];
	if (!readRomInfo(path, &rom, error, sizeof(error))) {
		printf("{\"error\":");
		printJsonString(error);
		printf("}\n");
		return 1;
	}
	printf("{\"title\":");
	printJsonString(rom.title);
	printf(",\"key\":");
	printJsonString(rom.key);
	printf(",\"color\":%s}\n", rom.isColor ? "true" : "false");
	return 0;
}

// --- The game --------------------------------------------------------------

static bool openGame(struct Engine* engine, const char* romPath, char* error, size_t errorSize) {
	if (!readRomInfo(romPath, &engine->rom, error, errorSize)) {
		return false;
	}
	struct VFile* rom = VFileOpen(romPath, O_RDONLY);
	if (!rom) {
		snprintf(error, errorSize, "can't open the ROM");
		return false;
	}
	struct mCore* core = mCoreFindVF(rom);
	if (!core || core->platform(core) != mPLATFORM_GB) {
		rom->close(rom);
		snprintf(error, errorSize, "that isn't a Game Boy ROM");
		return false;
	}
	mCoreInitConfig(core, NULL);
	core->init(core);

	// Never the person's own mGBA settings: these alone
	struct mCoreOptions options = {
		.useBios = false,
		.skipBios = true,
		.volume = 0x100,
		.sampleRate = SAMPLE_RATE,
		.audioBuffers = AUDIO_SAMPLES,
	};
	mCoreConfigLoadDefaults(&core->config, &options);
	// Black-and-white games get the colors a Super Game Boy or Game Boy Color
	// would have given them
	mCoreConfigSetDefaultIntValue(&core->config, "gb.colors", GB_COLORS_SGB_CGB_FALLBACK);
	mCoreLoadForeignConfig(core, &core->config);

	engine->video = calloc(STRIDE * 224, sizeof(color_t));
	core->setVideoBuffer(core, engine->video, STRIDE);
	core->setAudioBufferSize(core, AUDIO_SAMPLES);
	blip_set_rates(core->getAudioChannel(core, 0), core->frequency(core), SAMPLE_RATE);
	blip_set_rates(core->getAudioChannel(core, 1), core->frequency(core), SAMPLE_RATE);

	if (!core->loadROM(core, rom)) {
		snprintf(error, errorSize, "the ROM didn't load");
		return false;
	}

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/battery.sav", engine->saves);
	struct VFile* battery = VFileOpen(path, O_CREAT | O_RDWR);
	if (!battery) {
		snprintf(error, errorSize, "can't open the battery save %s: %s", path, strerror(errno));
		return false;
	}
	core->loadSave(core, battery);
	core->reset(core);
	engine->core = core;
	return true;
}

static void closeGame(struct Engine* engine) {
	if (!engine->core) {
		return;
	}
	// Unloading writes the battery save, the clock of games that keep one too
	engine->core->unloadROM(engine->core);
	engine->core->deinit(engine->core);
	engine->core = NULL;
}

static bool isSlotName(const char* slot) {
	return !strcmp(slot, "auto") || (slot[0] >= '1' && slot[0] <= '9' && slot[1] == '\0');
}

static void statePath(const struct Engine* engine, const char* slot, char* path, size_t size) {
	if (!strcmp(slot, "auto")) {
		snprintf(path, size, "%s/auto.state", engine->saves);
	} else {
		snprintf(path, size, "%s/slot%s.state", engine->saves, slot);
	}
}

static bool saveState(struct Engine* engine, const char* slot) {
	char path[PATH_MAX];
	char temporary[PATH_MAX + 8];
	statePath(engine, slot, path, sizeof(path));
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);

	struct VFile* vf = VFileOpen(temporary, O_CREAT | O_TRUNC | O_RDWR);
	if (!vf) {
		return false;
	}
	// With the battery save inside, a slot brings back the game exactly
	const bool isWritten = mCoreSaveStateNamed(engine->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC | SAVESTATE_METADATA);
	vf->close(vf);
	// Written whole, then moved over the old one, so a crash never leaves half
	if (!isWritten || rename(temporary, path) != 0) {
		unlink(temporary);
		return false;
	}
	if (!strcmp(slot, "auto")) {
		engine->hasPlayedSinceSave = false;
		engine->lastSaveNs = monotonicNs();
		engine->hasAutosave = stat(path, &engine->autosave) == 0;
	}
	emit("@saved %s %lld", slot, unixMs());
	return true;
}

static bool loadState(struct Engine* engine, const char* slot) {
	char path[PATH_MAX];
	statePath(engine, slot, path, sizeof(path));
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return false;
	}
	const bool isLoaded = mCoreLoadStateNamed(engine->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
	vf->close(vf);
	if (isLoaded) {
		engine->mustShow = true;
		engine->hasPlayedSinceSave = true;
		if (!strcmp(slot, "auto")) {
			engine->hasAutosave = stat(path, &engine->autosave) == 0;
		}
	}
	return isLoaded;
}

static int64_t modifiedNs(const struct stat* status) {
#ifdef __APPLE__
	return (int64_t) status->st_mtimespec.tv_sec * 1000000000LL + status->st_mtimespec.tv_nsec;
#else
	return (int64_t) status->st_mtim.tv_sec * 1000000000LL + status->st_mtim.tv_nsec;
#endif
}

// Whether the autosave on disk is another than this engine last wrote or
// read: another session played since
static bool isAutosaveNewer(const struct Engine* engine) {
	char path[PATH_MAX];
	statePath(engine, "auto", path, sizeof(path));
	struct stat now;
	if (stat(path, &now) != 0) {
		return false;
	}
	return !engine->hasAutosave || now.st_ino != engine->autosave.st_ino || now.st_size != engine->autosave.st_size ||
	       modifiedNs(&now) != modifiedNs(&engine->autosave);
}

// Takes the game's saves for this engine, unless another session is playing
// it now: before playing, and before changing them. A newer autosave than
// this engine's is the game now, so it's loaded first.
static bool claimGame(struct Engine* engine) {
	if (engine->hasLock) {
		return true;
	}
	if (engine->lock < 0 || flock(engine->lock, LOCK_EX | LOCK_NB) != 0) {
		return false;
	}
	engine->hasLock = true;
	if (isAutosaveNewer(engine) && loadState(engine, "auto")) {
		engine->hasPlayedSinceSave = false;
		emit("@log picked up the autosave another session left");
	}
	return true;
}

// Lets another session play: once this engine has paused and saved
static void releaseGame(struct Engine* engine) {
	if (engine->hasLock) {
		flock(engine->lock, LOCK_UN);
		engine->hasLock = false;
	}
}

static void applyKeys(struct Engine* engine, uint32_t held) {
	uint32_t keys = held & 0xFF;
	for (int i = 0; i < BUTTONS; i++) {
		if (engine->frames < engine->tapUntil[i]) {
			keys |= 1u << i;
		}
	}
	engine->core->setKeys(engine->core, keys);
}

static void runFrame(struct Engine* engine, uint32_t held) {
	applyKeys(engine, held);
	engine->core->runFrame(engine->core);
	engine->frames++;
}

// --- Frames for the mod ----------------------------------------------------

static inline uint32_t rgbOf(color_t pixel) {
	// The core's 32-bit color is 0x00BBGGRR
	return ((pixel & 0xFF) << 16) | (pixel & 0xFF00) | ((pixel >> 16) & 0xFF);
}

static void copyFrame(const struct Engine* engine, color_t* out) {
	for (int y = 0; y < HEIGHT; y++) {
		memcpy(&out[y * WIDTH], &engine->video[y * STRIDE], WIDTH * sizeof(color_t));
	}
}

static size_t base64Encode(const uint8_t* data, size_t size, char* out) {
	static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t length = 0;
	size_t i = 0;
	for (; i + 2 < size; i += 3) {
		const uint32_t triple = (uint32_t) data[i] << 16 | (uint32_t) data[i + 1] << 8 | data[i + 2];
		out[length++] = ALPHABET[(triple >> 18) & 63];
		out[length++] = ALPHABET[(triple >> 12) & 63];
		out[length++] = ALPHABET[(triple >> 6) & 63];
		out[length++] = ALPHABET[triple & 63];
	}
	if (i < size) {
		const uint32_t triple = (uint32_t) data[i] << 16 | (i + 1 < size ? (uint32_t) data[i + 1] << 8 : 0);
		out[length++] = ALPHABET[(triple >> 18) & 63];
		out[length++] = ALPHABET[(triple >> 12) & 63];
		out[length++] = i + 1 < size ? ALPHABET[(triple >> 6) & 63] : '=';
		out[length++] = '=';
	}
	out[length] = '\0';
	return length;
}

static void putWord(uint8_t* out, uint32_t word) {
	out[0] = word & 0xFF;
	out[1] = (word >> 8) & 0xFF;
	out[2] = (word >> 16) & 0xFF;
	out[3] = word >> 24;
}

// The frame as the cells of a Raster `columns` by `rows`: each an upper half
// block, its top pixel the foreground and its bottom one the background, so a
// cell is two square pixels. Smaller than the screen, each pixel is the
// average of the screen's pixels it covers. Returns the base64 text's length.
static size_t encodeCells(const color_t* frame, int columns, int rows, char* out) {
	static uint8_t words[MAX_CELL_COLUMNS * MAX_CELL_ROWS * 12];
	static int left[MAX_CELL_COLUMNS + 1];
	static int top[MAX_CELL_ROWS * 2 + 1];
	const int width = columns;
	const int height = rows * 2;
	for (int x = 0; x <= width; x++) {
		left[x] = x * WIDTH / width;
	}
	for (int y = 0; y <= height; y++) {
		top[y] = y * HEIGHT / height;
	}

	uint8_t* word = words;
	for (int row = 0; row < rows; row++) {
		for (int column = 0; column < columns; column++) {
			uint32_t halves[2];
			for (int half = 0; half < 2; half++) {
				const int y = row * 2 + half;
				const int x0 = left[column];
				const int x1 = left[column + 1] > x0 ? left[column + 1] : x0 + 1;
				const int y0 = top[y];
				const int y1 = top[y + 1] > y0 ? top[y + 1] : y0 + 1;
				uint32_t r = 0, g = 0, b = 0, n = 0;
				for (int sy = y0; sy < y1 && sy < HEIGHT; sy++) {
					for (int sx = x0; sx < x1 && sx < WIDTH; sx++) {
						const uint32_t rgb = rgbOf(frame[sy * WIDTH + sx]);
						r += rgb >> 16;
						g += (rgb >> 8) & 0xFF;
						b += rgb & 0xFF;
						n++;
					}
				}
				halves[half] = n ? ((r / n) << 16) | ((g / n) << 8) | (b / n) : 0;
			}
			putWord(word, 0x2580);
			putWord(word + 4, halves[0]);
			putWord(word + 8, halves[1]);
			word += 12;
		}
	}
	return base64Encode(words, (size_t) (word - words), out);
}

static bool writeCellsFile(const char* path, const color_t* frame, int columns, int rows, unsigned seq) {
	static char text[(MAX_CELL_COLUMNS * MAX_CELL_ROWS * 12 / 3 + 1) * 4 + 64];
	char temporary[PATH_MAX + 8];
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	const int header = snprintf(text, 64, "%d %d %u\n", columns, rows, seq);
	const size_t length = header + encodeCells(frame, columns, rows, text + header);

	FILE* file = fopen(temporary, "wb");
	if (!file) {
		return false;
	}
	const bool isWritten = fwrite(text, 1, length, file) == length;
	if (fclose(file) != 0 || !isWritten || rename(temporary, path) != 0) {
		unlink(temporary);
		return false;
	}
	return true;
}

static void shmName(char* out, size_t size, unsigned frame) {
	snprintf(out, size, "/claudeboy-%d-%u", (int) getpid(), frame);
}

// How one axis of the screen spreads over `size` pixels of a picture: for each
// of them, the screen pixels it covers and by how much. Covering pixels by
// area keeps edges crisp at any scale, where sampling would make some pixels
// wider than others and smoothing would blur them.
struct Spread {
	int size;
	int taps;
	int* first;
	int* count;
	float* weights;
};

static void spreadAxis(struct Spread* spread, int source, int size) {
	if (spread->size == size && spread->first) {
		return;
	}
	free(spread->first);
	free(spread->count);
	free(spread->weights);
	spread->size = size;
	spread->taps = source / size + 2;
	spread->first = calloc(size, sizeof(int));
	spread->count = calloc(size, sizeof(int));
	spread->weights = calloc((size_t) size * spread->taps, sizeof(float));
	const double step = (double) source / size;
	for (int i = 0; i < size; i++) {
		const double start = i * step;
		const double end = (i + 1) * step;
		int count = 0;
		for (int pixel = (int) start; pixel < end && pixel < source && count < spread->taps; pixel++) {
			const double overlap = fmin(end, pixel + 1) - fmax(start, pixel);
			if (overlap <= 0) {
				continue;
			}
			if (count == 0) {
				spread->first[i] = pixel;
			}
			spread->weights[(size_t) i * spread->taps + count++] = (float) (overlap / step);
		}
		spread->count[i] = count;
	}
}

// The screen as large as fits in a `width` by `height` RGBA picture at its own
// shape, in the middle, the strip left over see-through
static void renderPicture(const color_t* frame, int width, int height, uint8_t* out) {
	int pictureWidth = width;
	int pictureHeight = (int) lround((double) width * HEIGHT / WIDTH);
	if (pictureHeight > height) {
		pictureHeight = height;
		pictureWidth = (int) lround((double) height * WIDTH / HEIGHT);
	}
	pictureWidth = clampInt(pictureWidth, 1, width);
	pictureHeight = clampInt(pictureHeight, 1, height);
	const int left = (width - pictureWidth) / 2;
	const int top = (height - pictureHeight) / 2;

	static struct Spread across;
	static struct Spread down;
	spreadAxis(&across, WIDTH, pictureWidth);
	spreadAxis(&down, HEIGHT, pictureHeight);

	// Across first, into rows as wide as the picture
	static float* rows = NULL;
	static size_t rowsLength = 0;
	const size_t needed = (size_t) pictureWidth * HEIGHT * 3;
	if (rowsLength < needed) {
		free(rows);
		rows = malloc(needed * sizeof(float));
		rowsLength = needed;
	}
	for (int y = 0; y < HEIGHT; y++) {
		const color_t* line = &frame[y * WIDTH];
		float* row = &rows[(size_t) y * pictureWidth * 3];
		for (int x = 0; x < pictureWidth; x++) {
			const float* weights = &across.weights[(size_t) x * across.taps];
			float r = 0, g = 0, b = 0;
			for (int k = 0; k < across.count[x]; k++) {
				const uint32_t rgb = rgbOf(line[across.first[x] + k]);
				r += weights[k] * (float) (rgb >> 16);
				g += weights[k] * (float) ((rgb >> 8) & 0xFF);
				b += weights[k] * (float) (rgb & 0xFF);
			}
			row[x * 3] = r;
			row[x * 3 + 1] = g;
			row[x * 3 + 2] = b;
		}
	}

	// Then down, into the picture
	memset(out, 0, (size_t) width * height * 4);
	for (int y = 0; y < pictureHeight; y++) {
		const float* weights = &down.weights[(size_t) y * down.taps];
		uint8_t* pixel = out + ((size_t) (top + y) * width + left) * 4;
		for (int x = 0; x < pictureWidth; x++, pixel += 4) {
			float r = 0, g = 0, b = 0;
			for (int k = 0; k < down.count[y]; k++) {
				const float* source = &rows[((size_t) (down.first[y] + k) * pictureWidth + x) * 3];
				r += weights[k] * source[0];
				g += weights[k] * source[1];
				b += weights[k] * source[2];
			}
			pixel[0] = (uint8_t) fminf(255.0f, r + 0.5f);
			pixel[1] = (uint8_t) fminf(255.0f, g + 0.5f);
			pixel[2] = (uint8_t) fminf(255.0f, b + 0.5f);
			pixel[3] = 255;
		}
	}
}

// The size a picture for a `boxWidth` by `boxHeight` box is drawn at: the box
// itself, or past the pixel budget the largest of its shape within it
static void pictureSize(int boxWidth, int boxHeight, int* width, int* height) {
	*width = boxWidth;
	*height = boxHeight;
	const double pixels = (double) boxWidth * boxHeight;
	if (pixels > MAX_IMAGE_PIXELS) {
		const double shrink = sqrt(MAX_IMAGE_PIXELS / pixels);
		*width = clampInt((int) (boxWidth * shrink), 1, boxWidth);
		*height = clampInt((int) (boxHeight * shrink), 1, boxHeight);
	}
}

// The frame as a picture in a fresh shared-memory object the terminal reads
// and unlinks, drawn at the size the terminal shows it
static bool writeShmFrame(struct Engine* engine, const color_t* frame, int boxWidth, int boxHeight) {
	int width;
	int height;
	pictureSize(boxWidth, boxHeight, &width, &height);
	const size_t size = (size_t) width * height * 4;
	char name[64];
	if (engine->shmFrames >= FRAMES_IN_FLIGHT) {
		shmName(name, sizeof(name), engine->shmFrames - FRAMES_IN_FLIGHT);
		shm_unlink(name);
	}
	shmName(name, sizeof(name), engine->shmFrames++);

	const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0) {
		return false;
	}
	if (ftruncate(fd, (off_t) size) != 0) {
		close(fd);
		shm_unlink(name);
		return false;
	}
	uint8_t* out = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (out == MAP_FAILED) {
		shm_unlink(name);
		return false;
	}
	renderPicture(frame, width, height, out);
	munmap(out, size);
	emit("@frame %s %d %d", name, width, height);
	return true;
}

static void removeShmFrames(struct Engine* engine) {
	const unsigned first = engine->shmFrames > FRAMES_IN_FLIGHT ? engine->shmFrames - FRAMES_IN_FLIGHT : 0;
	for (unsigned frame = first; frame < engine->shmFrames; frame++) {
		char name[64];
		shmName(name, sizeof(name), frame);
		shm_unlink(name);
	}
}

// Hands the mod the newest frame as it asked for frames, at most at its frame
// rate and only when the picture changed, unless `isForced`
static void showFrame(struct Engine* engine, bool isForced) {
	const struct Control* wanted = &engine->wanted;
	if (wanted->view == VIEW_NONE || !engine->core) {
		return;
	}
	const int64_t now = monotonicNs();
	const bool isNewView = wanted->view != engine->shownView || wanted->columns != engine->shownColumns ||
	                       wanted->rows != engine->shownRows || wanted->boxWidth != engine->shownBoxWidth ||
	                       wanted->boxHeight != engine->shownBoxHeight;
	static color_t frame[WIDTH * HEIGHT];
	copyFrame(engine, frame);
	if (!isForced && !isNewView && engine->hasShown) {
		int fps = clampInt(wanted->fps, 1, 60);
		if (wanted->view == VIEW_IMAGE) {
			int width;
			int height;
			pictureSize(wanted->boxWidth, wanted->boxHeight, &width, &height);
			if ((long) width * height > SMOOTH_IMAGE_PIXELS && fps > 30) {
				fps = 30;
			}
		}
		const int64_t interval = 1000000000LL / fps;
		// A little early is fine: frames arrive on the Game Boy's clock
		if (now - engine->lastShownNs < interval - 4000000LL) {
			return;
		}
		if (!memcmp(frame, engine->shown, sizeof(frame))) {
			return;
		}
	}

	bool isShown = false;
	if (wanted->view == VIEW_CELLS) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/cells", engine->run);
		const unsigned seq = engine->seq + 1;
		isShown = writeCellsFile(path, frame, wanted->columns, wanted->rows, seq);
		if (isShown) {
			engine->seq = seq;
			emit("@cells %u %d %d", seq, wanted->columns, wanted->rows);
		}
	} else if (wanted->view == VIEW_WINDOW) {
		isShown = engine->window != NULL;
		if (isShown) {
			static uint32_t rgb[WIDTH * HEIGHT];
			for (int i = 0; i < WIDTH * HEIGHT; i++) {
				rgb[i] = rgbOf(frame[i]);
			}
			gameWindowDraw(engine->window, rgb);
		}
	} else {
		isShown = writeShmFrame(engine, frame, wanted->boxWidth, wanted->boxHeight);
	}
	if (!isShown) {
		return;
	}
	memcpy(engine->shown, frame, sizeof(frame));
	engine->hasShown = true;
	engine->lastShownNs = now;
	engine->shownView = wanted->view;
	engine->shownColumns = wanted->columns;
	engine->shownRows = wanted->rows;
	engine->shownBoxWidth = wanted->boxWidth;
	engine->shownBoxHeight = wanted->boxHeight;
}

// --- Sound -----------------------------------------------------------------

static void stopAudio(struct Engine* engine) {
	if (engine->audioFd >= 0) {
		// The player plays what it holds and exits on its own
		close(engine->audioFd);
		engine->audioFd = -1;
	}
}

static void startAudio(struct Engine* engine) {
	if (engine->audioFd >= 0 || engine->isAudioBroken) {
		return;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		engine->isAudioBroken = true;
		return;
	}
	const pid_t pid = fork();
	if (pid == 0) {
		dup2(fds[0], STDIN_FILENO);
		close(fds[0]);
		close(fds[1]);
		const int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		for (size_t i = 0; i < sizeof(AUDIO_PLAYERS) / sizeof(AUDIO_PLAYERS[0]); i++) {
			execvp(AUDIO_PLAYERS[i][0], (char* const*) AUDIO_PLAYERS[i]);
		}
		_exit(127);
	}
	close(fds[0]);
	if (pid < 0) {
		close(fds[1]);
		engine->isAudioBroken = true;
		return;
	}
	fcntl(fds[1], F_SETFL, O_NONBLOCK);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#ifdef F_SETPIPE_SZ
	fcntl(fds[1], F_SETPIPE_SZ, 4 * AUDIO_QUEUE_TARGET);
#endif
	engine->audioFd = fds[1];
	// What the core made while nobody listened would play late
	blip_clear(engine->core->getAudioChannel(engine->core, 0));
	blip_clear(engine->core->getAudioChannel(engine->core, 1));
}

// Takes the frame's sound from the core, and plays it when sound is on
static void pumpAudio(struct Engine* engine, bool isHeard) {
	struct mCore* core = engine->core;
	blip_t* left = core->getAudioChannel(core, 0);
	blip_t* right = core->getAudioChannel(core, 1);
	int available = blip_samples_avail(left);
	if (available > AUDIO_SAMPLES) {
		available = AUDIO_SAMPLES;
	}
	const int count = blip_read_samples(left, engine->audio, available, 1);
	blip_read_samples(right, engine->audio + 1, available, 1);
	if (!isHeard || engine->audioFd < 0 || count <= 0) {
		return;
	}

	// The player's clock and the Game Boy's drift apart: make a little more
	// or less sound per frame to keep the queue to the player near its target
	int queued = 0;
	if (ioctl(engine->audioFd, FIONREAD, &queued) == 0) {
		double error = (double) (AUDIO_QUEUE_TARGET - queued) / AUDIO_QUEUE_TARGET;
		error = error < -1 ? -1 : error > 1 ? 1 : error;
		const double rate = SAMPLE_RATE * (1.0 + 0.005 * error);
		blip_set_rates(left, core->frequency(core), rate);
		blip_set_rates(right, core->frequency(core), rate);
	}

	const ssize_t written = write(engine->audioFd, engine->audio, (size_t) count * 4);
	if (written < 0 && errno == EPIPE) {
		stopAudio(engine);
		engine->isAudioBroken = true;
		emit("@log no sound: no player started (aplay, pacat or ffplay)");
	}
	// A full pipe drops the rest of this frame's sound
}

// --- The control file ------------------------------------------------------

// Decodes base64 text into `out`, as a string cut short to fit
static void base64Decode(const char* text, char* out, size_t size) {
	static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint32_t buffer = 0;
	int bits = 0;
	size_t length = 0;
	for (const char* c = text; *c && *c != '='; c++) {
		const char* at = strchr(ALPHABET, *c);
		if (!at) {
			break;
		}
		buffer = (buffer << 6) | (uint32_t) (at - ALPHABET);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (length + 1 < size) {
				out[length++] = (char) ((buffer >> bits) & 0xFF);
			}
		}
	}
	out[length] = '\0';
}

static bool parseView(const char* value, struct Control* control) {
	int a = 0, b = 0;
	if (sscanf(value, "cells:%dx%d", &a, &b) == 2) {
		control->view = VIEW_CELLS;
		control->columns = clampInt(a, 1, MAX_CELL_COLUMNS);
		control->rows = clampInt(b, 1, MAX_CELL_ROWS);
		return true;
	}
	if (sscanf(value, "image:%dx%d", &a, &b) == 2) {
		control->view = VIEW_IMAGE;
		control->boxWidth = clampInt(a, 16, MAX_IMAGE_SIDE);
		control->boxHeight = clampInt(b, 16, MAX_IMAGE_SIDE);
		return true;
	}
	if (!strcmp(value, "window")) {
		control->view = VIEW_WINDOW;
		return true;
	}
	control->view = VIEW_NONE;
	return !strcmp(value, "none");
}

static int parseCommands(char* value, struct Command* commands) {
	int count = 0;
	for (char* entry = strtok(value, ","); entry && count < MAX_COMMANDS; entry = strtok(NULL, ",")) {
		struct Command* command = &commands[count];
		memset(command, 0, sizeof(*command));
		char* end = NULL;
		command->id = strtol(entry, &end, 10);
		if (end == entry || *end != ':') {
			continue;
		}
		char* name = end + 1;
		char* arg = strchr(name, ':');
		if (arg) {
			*arg++ = '\0';
			snprintf(command->arg, sizeof(command->arg), "%s", arg);
		}
		snprintf(command->name, sizeof(command->name), "%s", name);
		count++;
	}
	return count;
}

static void parseControl(char* line, struct Control* control, struct Command* commands, int* commandCount) {
	memset(control, 0, sizeof(*control));
	control->fps = 30;
	control->speed = 1;
	*commandCount = 0;
	char* save = NULL;
	for (char* token = strtok_r(line, " \n", &save); token; token = strtok_r(NULL, " \n", &save)) {
		char* value = strchr(token, '=');
		if (!value) {
			continue;
		}
		*value++ = '\0';
		if (!strcmp(token, "play")) {
			control->isPlaying = atoi(value) != 0;
		} else if (!strcmp(token, "keys")) {
			control->keys = (uint32_t) strtoul(value, NULL, 10);
		} else if (!strcmp(token, "taps")) {
			char* cursor = value;
			for (int i = 0; i < BUTTONS && *cursor; i++) {
				control->taps[i] = (uint32_t) strtoul(cursor, &cursor, 10);
				if (*cursor == ',') {
					cursor++;
				}
			}
		} else if (!strcmp(token, "view")) {
			parseView(value, control);
		} else if (!strcmp(token, "fps")) {
			control->fps = clampInt(atoi(value), 1, 60);
		} else if (!strcmp(token, "sound")) {
			control->isSoundOn = atoi(value) != 0;
		} else if (!strcmp(token, "speed")) {
			control->speed = clampInt(atoi(value), 1, 8);
		} else if (!strcmp(token, "overlay")) {
			sscanf(value, "%d,%d,%d,%d", &control->overlay[0], &control->overlay[1], &control->overlay[2], &control->overlay[3]);
		} else if (!strcmp(token, "title")) {
			base64Decode(value, control->title, sizeof(control->title));
		} else if (!strcmp(token, "cmds")) {
			*commandCount = parseCommands(value, commands);
		}
	}
}

static void runCommand(struct Engine* engine, const struct Command* command) {
	if (command->id <= engine->lastCommand) {
		return;
	}
	engine->lastCommand = command->id;
	const char* name = command->name;
	const char* slot = command->arg;
	const bool isChange = !strcmp(name, "save") || !strcmp(name, "load") || !strcmp(name, "reset");
	if (isChange && !claimGame(engine)) {
		emit("@done %ld fail busy: another Claude Code session is playing it", command->id);
		return;
	}
	if (!strcmp(name, "save") || !strcmp(name, "load")) {
		if (!isSlotName(slot)) {
			emit("@done %ld fail no slot %s", command->id, slot);
		} else if (!strcmp(name, "save")) {
			if (saveState(engine, slot)) {
				emit("@done %ld ok saved %s", command->id, slot);
			} else {
				emit("@done %ld fail couldn't write slot %s", command->id, slot);
			}
		} else if (loadState(engine, slot)) {
			emit("@done %ld ok loaded %s", command->id, slot);
		} else {
			char path[PATH_MAX];
			statePath(engine, slot, path, sizeof(path));
			emit("@done %ld fail %s", command->id, access(path, F_OK) == 0 ? "couldn't read that slot" : "that slot is empty");
		}
	} else if (!strcmp(name, "reset")) {
		engine->core->reset(engine->core);
		engine->mustShow = true;
		engine->hasPlayedSinceSave = true;
		emit("@done %ld ok reset", command->id);
	} else if (!strcmp(name, "refresh")) {
		engine->mustShow = true;
		emit("@done %ld ok refreshed", command->id);
	} else if (!strcmp(name, "quit")) {
		isStopRequested = 1;
		emit("@done %ld ok quitting", command->id);
	} else {
		emit("@done %ld fail no command %s", command->id, name);
	}
	// Changed while nobody plays here: the autosave says so for every session
	if (isChange && !engine->isPlaying) {
		if (engine->hasPlayedSinceSave) {
			saveState(engine, "auto");
		}
		releaseGame(engine);
	}
}

// Reads the control file. The mod rewrites it in place, so a line without its
// newline is a write in progress: the last whole one stands until next frame.
static void readControl(struct Engine* engine) {
	char line[CONTROL_MAX];
	const int fd = open(engine->control, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return;
	}
	const ssize_t size = read(fd, line, sizeof(line) - 1);
	close(fd);
	if (size <= 0) {
		return;
	}
	line[size] = '\0';
	char* end = strchr(line, '\n');
	if (!end) {
		return;
	}
	end[1] = '\0';
	if (!strcmp(line, engine->line)) {
		return;
	}
	memcpy(engine->line, line, (size_t) (end - line) + 2);

	struct Command commands[MAX_COMMANDS];
	int commandCount = 0;
	parseControl(line, &engine->wanted, commands, &commandCount);
	for (int i = 0; i < BUTTONS; i++) {
		// A count that went down is a new gamepad counting from zero
		if (engine->hasTaps && engine->wanted.taps[i] > engine->taps[i]) {
			engine->tapUntil[i] = engine->frames + TAP_FRAMES;
		}
		engine->taps[i] = engine->wanted.taps[i];
	}
	engine->hasTaps = true;
	for (int i = 0; i < commandCount && engine->core; i++) {
		runCommand(engine, &commands[i]);
	}
}

// --- The terminal ----------------------------------------------------------

static pid_t parentOf(pid_t pid) {
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
	FILE* file = fopen(path, "r");
	if (!file) {
		return -1;
	}
	char stat[1024];
	const size_t length = fread(stat, 1, sizeof(stat) - 1, file);
	fclose(file);
	stat[length] = '\0';
	// The command's name, in parentheses, may hold anything
	const char* end = strrchr(stat, ')');
	char state = 0;
	int parent = -1;
	if (!end || sscanf(end + 1, " %c %d", &state, &parent) != 2) {
		return -1;
	}
	return parent;
}

// The terminal Claude Code draws in, found on a standard stream of the
// engine's nearest forebear that has one. It's opened to ask its size alone:
// the mod sizes pictures to its cells, which only the terminal knows in
// pixels. Without /proc, or a terminal, there's none.
static int openTerminal(void) {
	pid_t pid = getppid();
	for (int depth = 0; depth < 8 && pid > 1; depth++) {
		for (int stream = 0; stream <= 2; stream++) {
			char link[64];
			char target[PATH_MAX];
			snprintf(link, sizeof(link), "/proc/%d/fd/%d", (int) pid, stream);
			const ssize_t length = readlink(link, target, sizeof(target) - 1);
			if (length <= 0) {
				continue;
			}
			target[length] = '\0';
			if (strncmp(target, "/dev/pts/", 9) != 0 && strncmp(target, "/dev/tty", 8) != 0) {
				continue;
			}
			const int fd = open(target, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0) {
				continue;
			}
			struct winsize size;
			if (ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
				return fd;
			}
			close(fd);
		}
		pid = parentOf(pid);
	}
	return -1;
}

// Tells the mod the terminal's size whenever it changes: a resize, or another
// font, changes how big a picture fits
static void watchTerminal(struct Engine* engine, bool isForced) {
	const int64_t now = monotonicNs();
	if (!isForced && now - engine->lastTerminalCheckNs < TERMINAL_CHECK_NS) {
		return;
	}
	engine->lastTerminalCheckNs = now;
	struct winsize size;
	if (engine->tty < 0 || ioctl(engine->tty, TIOCGWINSZ, &size) != 0) {
		return;
	}
	if (!isForced && !memcmp(&size, &engine->terminal, sizeof(size))) {
		return;
	}
	engine->terminal = size;
	emit("@term %u %u %u %u", size.ws_col, size.ws_row, size.ws_xpixel, size.ws_ypixel);
}

// --- The window ------------------------------------------------------------

static void layWindow(struct Engine* engine);

// Opens the window the first time the mod asks for one, shows it while the
// game plays in it, and passes on what happened in it
static void tendWindow(struct Engine* engine) {
	const bool isWanted = engine->wanted.view == VIEW_WINDOW;
	if (isWanted && !engine->window && !engine->isWindowBroken) {
		char error[256];
		engine->window = gameWindowOpen(0, error, sizeof(error));
		if (!engine->window) {
			engine->isWindowBroken = true;
			emit("@nowindow %s", error);
			return;
		}
		engine->windowTitle[0] = '\0';
		int width;
		int workWidth;
		gameWindowDrawer(engine->window, &width, &workWidth);
		emit("@drawer %d %d", width, workWidth);
	}
	if (!engine->window) {
		return;
	}
	if (!engine->wanted.isPlaying) {
		engine->isWindowDismissed = false;
	}
	layWindow(engine);
	if (isWanted && engine->isPlaying && !engine->isWindowDismissed) {
		gameWindowShow(engine->window);
	} else {
		gameWindowHide(engine->window);
	}
	if (engine->wanted.title[0] && strcmp(engine->wanted.title, engine->windowTitle) != 0) {
		snprintf(engine->windowTitle, sizeof(engine->windowTitle), "%s", engine->wanted.title);
		gameWindowSetTitle(engine->window, engine->windowTitle);
	}

	struct GameWindowEvents events;
	gameWindowPump(engine->window, &events);
	engine->windowKeys = events.keys;
	for (int i = 0; i < events.typedCount; i++) {
		emit("@key %c", events.typed[i]);
	}
	if (events.focus >= 0) {
		emit("@focus %d", events.focus);
	}
	if (events.isClosed) {
		// Gone at once; the mod pauses the game, which saves it
		engine->isWindowDismissed = true;
		engine->windowKeys = 0;
		gameWindowHide(engine->window);
		emit("@closed");
	}
	if (events.needsDrawing) {
		engine->mustShow = true;
	}
}

// Where the terminal's grid of cells is on the screen, when that can be told:
// a terminal that fills the desktop's work area, as a maximized one does.
// Its cells' size is the terminal's own where it says (kitty, Ghostty), or
// what fits the work area, its scrollbar and header bar at most what those
// usually take; its grid begins below a header bar of a GNOME-sized height.
static bool terminalGrid(struct Engine* engine, int* cellWidth, int* cellHeight, int* left, int* top) {
	const int columns = engine->terminal.ws_col;
	const int rows = engine->terminal.ws_row;
	if (!engine->window || columns <= 0 || rows <= 0) {
		return false;
	}
	int x;
	int y;
	int width;
	int height;
	gameWindowWorkArea(engine->window, &x, &y, &width, &height);
	if (engine->terminal.ws_xpixel && engine->terminal.ws_ypixel) {
		*cellWidth = engine->terminal.ws_xpixel / columns;
		*cellHeight = engine->terminal.ws_ypixel / rows;
	} else {
		*cellWidth = (width - 8) / columns;
		*cellHeight = (height - 40) / rows;
	}
	const int spareWidth = width - columns * *cellWidth;
	const int spareHeight = height - rows * *cellHeight;
	if (*cellWidth < 5 || *cellHeight < 10 || spareWidth < 0 || spareWidth > 24 || spareHeight < 30 || spareHeight > 90) {
		return false;
	}
	*left = x + 1;
	*top = y + (spareHeight < 47 ? spareHeight : 47);
	return true;
}

// Lays the window over the box of cells the mod names, when the terminal's
// grid can be found on the screen; docks it as the drawer when not
static void layWindow(struct Engine* engine) {
	int cellWidth = 0;
	int cellHeight = 0;
	int left = 0;
	int top = 0;
	const bool hasGrid = terminalGrid(engine, &cellWidth, &cellHeight, &left, &top);
	if (hasGrid && (cellWidth != engine->cellWidth || cellHeight != engine->cellHeight)) {
		engine->cellWidth = cellWidth;
		engine->cellHeight = cellHeight;
		emit("@cell %d %d", cellWidth, cellHeight);
	}
	const int* box = engine->wanted.overlay;
	if (!hasGrid || box[2] <= 0 || box[3] <= 0) {
		gameWindowPlace(engine->window, 0, 0, 0, 0);
		return;
	}
	// The game as large as fits the box at a whole multiple, in its middle
	const int boxWidth = box[2] * cellWidth;
	const int boxHeight = box[3] * cellHeight;
	int scale = boxWidth / WINDOW_SCREEN_WIDTH < boxHeight / WINDOW_SCREEN_HEIGHT ? boxWidth / WINDOW_SCREEN_WIDTH
	                                                                          : boxHeight / WINDOW_SCREEN_HEIGHT;
	scale = scale < 1 ? 1 : scale;
	const int width = WINDOW_SCREEN_WIDTH * scale;
	const int height = WINDOW_SCREEN_HEIGHT * scale;
	gameWindowPlace(engine->window, left + box[0] * cellWidth + (boxWidth - width) / 2,
	                top + box[1] * cellHeight + (boxHeight - height) / 2, width, height);
}

// The desktop ended under the window: save what was played, then go
static struct Engine* windowEngine = NULL;

static void onWindowLost(void) {
	if (windowEngine && windowEngine->hasPlayedSinceSave && windowEngine->hasLock) {
		saveState(windowEngine, "auto");
	}
	emit("@bye");
}

// --- Playing ---------------------------------------------------------------

static int play(const char* romPath, const char* saves, const char* run) {
	static struct Engine engine;
	engine.audioFd = -1;
	engine.tty = openTerminal();
	snprintf(engine.saves, sizeof(engine.saves), "%s", saves);
	snprintf(engine.run, sizeof(engine.run), "%s", run);
	snprintf(engine.control, sizeof(engine.control), "%s/control", run);
	if (!makeDirectories(saves) || !makeDirectories(run)) {
		emit("@error can't make the folders for saves: %s", strerror(errno));
		return 1;
	}

	char lockPath[PATH_MAX];
	snprintf(lockPath, sizeof(lockPath), "%s/.lock", saves);
	engine.lock = open(lockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	windowEngine = &engine;
	gameWindowOnLost(onWindowLost);

	char error[512];
	if (!openGame(&engine, romPath, error, sizeof(error))) {
		emit("@error %s", error);
		return 1;
	}
	if (loadState(&engine, "auto")) {
		engine.hasPlayedSinceSave = false;
	}
	engine.lastSaveNs = monotonicNs();
	emit("@ready %s %d %s", engine.rom.key, engine.rom.isColor ? 1 : 0, engine.rom.title);
	watchTerminal(&engine, true);

	const pid_t parent = getppid();
	int64_t next = monotonicNs();
	while (!isStopRequested) {
		readControl(&engine);
		watchTerminal(&engine, false);
		tendWindow(&engine);
		// Claude Code went away without telling: save and go too
		if (isStopRequested || getppid() != parent) {
			break;
		}

		if (!engine.wanted.isPlaying) {
			engine.isBusyReported = false;
			if (engine.isPlaying) {
				engine.isPlaying = false;
				stopAudio(&engine);
				// Handed back to Claude: this is the autosave, and then another
				// session may play
				if (engine.hasPlayedSinceSave) {
					saveState(&engine, "auto");
				}
				releaseGame(&engine);
				emit("@paused");
			}
			if (engine.mustShow) {
				engine.mustShow = false;
				showFrame(&engine, true);
			}
			sleepMs(15);
			continue;
		}

		if (!engine.isPlaying) {
			if (!claimGame(&engine)) {
				if (!engine.isBusyReported) {
					engine.isBusyReported = true;
					emit("@busy");
				}
				sleepMs(100);
				continue;
			}
			engine.isPlaying = true;
			engine.mustShow = true;
			next = monotonicNs();
			emit("@playing");
		}
		const bool isFast = engine.wanted.speed > 1;
		if (engine.wanted.isSoundOn && !isFast) {
			startAudio(&engine);
		} else {
			stopAudio(&engine);
		}
		for (int i = 0; i < engine.wanted.speed; i++) {
			runFrame(&engine, engine.wanted.keys | engine.windowKeys);
			pumpAudio(&engine, !isFast);
		}
		engine.hasPlayedSinceSave = true;
		showFrame(&engine, engine.mustShow);
		engine.mustShow = false;

		const int64_t now = monotonicNs();
		if (now - engine.lastSaveNs >= AUTOSAVE_SECONDS * 1000000000LL) {
			saveState(&engine, "auto");
		}
		next += FRAME_NS;
		// Far behind, after a stall: start the clock over rather than race
		if (now - next > 100000000LL) {
			next = now;
		} else {
			sleepUntil(next);
		}
	}

	stopAudio(&engine);
	// Unsaved play is only ever this engine's own, while it holds the game
	if (engine.hasPlayedSinceSave && engine.hasLock) {
		saveState(&engine, "auto");
	}
	releaseGame(&engine);
	removeShmFrames(&engine);
	if (engine.window) {
		gameWindowClose(engine.window);
	}
	closeGame(&engine);
	if (engine.tty >= 0) {
		close(engine.tty);
	}
	if (engine.lock >= 0) {
		close(engine.lock);
	}
	emit("@bye");
	return 0;
}

// --- Scripts, for testing --------------------------------------------------

static int buttonIndex(const char* name) {
	for (int i = 0; i < BUTTONS; i++) {
		if (!strcmp(name, BUTTON_NAMES[i])) {
			return i;
		}
	}
	return -1;
}

static bool writePpm(const char* path, const color_t* frame) {
	FILE* file = fopen(path, "wb");
	if (!file) {
		return false;
	}
	fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		const uint32_t rgb = rgbOf(frame[i]);
		const uint8_t pixel[3] = { rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF };
		fwrite(pixel, 1, 3, file);
	}
	return fclose(file) == 0;
}

static int script(const char* romPath, const char* saves, const char* out, int stepCount, char** steps) {
	static struct Engine engine;
	engine.audioFd = -1;
	engine.tty = -1;
	engine.lock = -1;
	snprintf(engine.saves, sizeof(engine.saves), "%s", saves);
	if (!makeDirectories(saves) || !makeDirectories(out)) {
		fprintf(stderr, "can't make %s or %s\n", saves, out);
		return 1;
	}
	char error[512];
	if (!openGame(&engine, romPath, error, sizeof(error))) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	printf("%s %s %s\n", engine.rom.key, engine.rom.isColor ? "color" : "mono", engine.rom.title);

	uint32_t held = 0;
	static color_t frame[WIDTH * HEIGHT];
	for (int i = 0; i < stepCount; i++) {
		const char* step = steps[i];
		char path[PATH_MAX];
		int columns = 0, rows = 0, count = 0;
		char name[128];
		if (isdigit((unsigned char) step[0])) {
			for (long n = strtol(step, NULL, 10); n > 0; n--) {
				runFrame(&engine, held);
				pumpAudio(&engine, false);
			}
		} else if ((step[0] == '+' || step[0] == '-') && buttonIndex(step + 1) >= 0) {
			const uint32_t bit = 1u << buttonIndex(step + 1);
			held = step[0] == '+' ? held | bit : held & ~bit;
		} else if (!strncmp(step, "tap:", 4) && buttonIndex(step + 4) >= 0) {
			const uint32_t bit = 1u << buttonIndex(step + 4);
			for (int n = 0; n < 10; n++) {
				runFrame(&engine, n < 5 ? held | bit : held);
				pumpAudio(&engine, false);
			}
		} else if (!strncmp(step, "shot:", 5)) {
			copyFrame(&engine, frame);
			snprintf(path, sizeof(path), "%s/%s.ppm", out, step + 5);
			if (!writePpm(path, frame)) {
				fprintf(stderr, "can't write %s\n", path);
				return 1;
			}
		} else if (sscanf(step, "cells:%dx%d:%127s", &columns, &rows, name) == 3) {
			copyFrame(&engine, frame);
			snprintf(path, sizeof(path), "%s/%s.cells", out, name);
			if (!writeCellsFile(path, frame, clampInt(columns, 1, MAX_CELL_COLUMNS), clampInt(rows, 1, MAX_CELL_ROWS), 1)) {
				fprintf(stderr, "can't write %s\n", path);
				return 1;
			}
		} else if (sscanf(step, "image:%dx%d:%127s", &columns, &rows, name) == 3) {
			// Here columns and rows are the picture's width and height in pixels
			copyFrame(&engine, frame);
			int width;
			int height;
			pictureSize(clampInt(columns, 16, MAX_IMAGE_SIDE), clampInt(rows, 16, MAX_IMAGE_SIDE), &width, &height);
			uint8_t* picture = malloc((size_t) width * height * 4);
			renderPicture(frame, width, height, picture);
			snprintf(path, sizeof(path), "%s/%s.ppm", out, name);
			FILE* file = fopen(path, "wb");
			if (!file) {
				fprintf(stderr, "can't write %s\n", path);
				return 1;
			}
			fprintf(file, "P6\n%d %d\n255\n", width, height);
			for (size_t i = 0; i < (size_t) width * height; i++) {
				// The see-through strip shows magenta
				const uint8_t* pixel = &picture[i * 4];
				const uint8_t rgb[3] = { pixel[3] ? pixel[0] : 255, pixel[3] ? pixel[1] : 0, pixel[3] ? pixel[2] : 255 };
				fwrite(rgb, 1, 3, file);
			}
			fclose(file);
			free(picture);
		} else if (!strncmp(step, "save:", 5) && isSlotName(step + 5)) {
			if (!saveState(&engine, step + 5)) {
				fprintf(stderr, "can't save %s\n", step + 5);
				return 1;
			}
		} else if (!strncmp(step, "load:", 5) && isSlotName(step + 5)) {
			if (!loadState(&engine, step + 5)) {
				fprintf(stderr, "can't load %s\n", step + 5);
				return 1;
			}
		} else if (sscanf(step, "bench:%d", &count) == 1 && count > 0) {
			const int64_t start = monotonicNs();
			for (int n = 0; n < count; n++) {
				runFrame(&engine, held);
				pumpAudio(&engine, false);
			}
			const double seconds = (monotonicNs() - start) / 1e9;
			printf("bench %d frames in %.3f s: %.0f fps\n", count, seconds, count / seconds);
		} else {
			fprintf(stderr, "unknown step: %s\n", step);
			return 2;
		}
	}
	closeGame(&engine);
	return 0;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	mLogSetDefaultLogger(&logger);

	struct sigaction stop = { .sa_handler = requestStop };
	sigemptyset(&stop.sa_mask);
	sigaction(SIGTERM, &stop, NULL);
	sigaction(SIGINT, &stop, NULL);
	sigaction(SIGHUP, &stop, NULL);
	// A player that died must not take the engine with it
	signal(SIGPIPE, SIG_IGN);
	// Finished players are reaped without waiting on them
	signal(SIGCHLD, SIG_IGN);

	if (argc == 3 && !strcmp(argv[1], "info")) {
		return info(argv[2]);
	}
	if (argc == 5 && !strcmp(argv[1], "run")) {
		return play(argv[2], argv[3], argv[4]);
	}
	if (argc >= 5 && !strcmp(argv[1], "script")) {
		return script(argv[2], argv[3], argv[4], argc - 5, argv + 5);
	}
	fputs(USAGE, stderr);
	return 2;
}
