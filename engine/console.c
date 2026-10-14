// The console drawn around the game, in pixel art at one pixel per pixel of
// the Game Boy's screen. The engine scales the whole of it to the room it has.

#include "console.h"

#include <math.h>
#include <string.h>

// Claude's orange, and what's cut from it
#define BODY 0xFFD97757u
#define BODY_LIGHT 0xFFEDA088u
#define BODY_SHADE 0xFFB45A40u
#define RECESS 0xFFC4664Au
#define SLOT 0xFFA4502Fu
#define BEZEL 0xFF2A2321u
#define BEZEL_DEEP 0xFF171211u
#define BEZEL_EDGE 0xFF3D3330u
#define CREAM 0xFFF6E7DCu
#define INK 0xFF4A2216u
#define PAD 0xFF2B2220u
#define PAD_LIGHT 0xFF4D403Bu
#define BUTTON 0xFF8E2F1Fu
#define BUTTON_LIGHT 0xFFB8543Cu
#define BUTTON_SHADE 0xFF5E1D13u
#define PILL 0xFF5A4038u
#define PILL_LIGHT 0xFF7A5A50u
// Held: lit, warm
#define LIT 0xFFFFC9AEu
#define LIT_GLOW 0xFFFFE9DEu
#define LED_ON 0xFFFF5A4Au
#define LED_GLOW 0xFFFFA08Fu

enum { KEY_A, KEY_B, KEY_SELECT, KEY_START, KEY_RIGHT, KEY_LEFT, KEY_UP, KEY_DOWN };

struct Layout {
	int bodyRadius[4]; // top left, top right, bottom right, bottom left
	int bezel[4];      // x, y, width, height
	int bezelRadius[4];
	int screenX;
	int screenY;
	int led[2];
	int stripe[4]; // y, left line's end, label's x, right line's start
	int stripeLeft;
	int stripeRight;
	int logo[2];
	int pad[2];
	int b[2];
	int a[2];
	int buttonRadius;
	int pills[3][4]; // select, start, speed: one end, the other
	int pillLabels[3][2];
	int slot[4][4]; // the speaker's: one end, the other
};

// Wide, the controls at either side of the screen, CONSOLE_WIDTH by
// CONSOLE_HEIGHT
static const struct Layout LAYOUT = {
	.bodyRadius = { 30, 30, 30, 30 },
	.bezel = { 48, 8, 176, 166 },
	.bezelRadius = { 6, 6, 6, 6 },
	.screenX = 56,
	.screenY = 22,
	.led = { 24, 30 },
	.stripe = { 14, 110, 114, 162 },
	.stripeLeft = 56,
	.stripeRight = 216,
	.logo = { 99, 186 },
	.pad = { 24, 84 },
	.b = { 237, 100 },
	.a = { 258, 82 },
	.buttonRadius = 9,
	.pills = { { 19, 130, 29, 124 }, { 19, 152, 29, 146 }, { 243, 152, 253, 146 } },
	.pillLabels = { { 12, 134 }, { 14, 156 }, { 245, 156 } },
	.slot = { { 222, 188, 229, 172 }, { 228, 188, 235, 172 }, { 234, 186, 241, 172 }, { 240, 182, 246, 172 } },
};

int consoleFit(int width, int height) {
	const int scale = width / CONSOLE_WIDTH < height / CONSOLE_HEIGHT ? width / CONSOLE_WIDTH : height / CONSOLE_HEIGHT;
	return scale < 1 ? 1 : scale;
}

void consoleScreen(int* x, int* y) {
	*x = LAYOUT.screenX;
	*y = LAYOUT.screenY;
}

// --- Drawing ---------------------------------------------------------------

struct Canvas {
	uint32_t* pixels;
	int width;
	int height;
};

static uint32_t get(const struct Canvas* canvas, int x, int y) {
	return x >= 0 && y >= 0 && x < canvas->width && y < canvas->height ? canvas->pixels[y * canvas->width + x] : 0;
}

static void put(struct Canvas* canvas, int x, int y, uint32_t color) {
	if (x >= 0 && y >= 0 && x < canvas->width && y < canvas->height) {
		canvas->pixels[y * canvas->width + x] = color;
	}
}

// Halfway between two colors, for glows
static uint32_t mix(uint32_t a, uint32_t b) {
	return 0xFF000000u | ((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) / 2) << 16 |
	       ((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) / 2) << 8 | (((a & 0xFF) + (b & 0xFF)) / 2);
}

static bool inRoundRect(int x, int y, const int rect[4], const int radius[4]) {
	if (x < rect[0] || y < rect[1] || x >= rect[0] + rect[2] || y >= rect[1] + rect[3]) {
		return false;
	}
	const int corners[4][2] = { { rect[0] + radius[0], rect[1] + radius[0] },
		                        { rect[0] + rect[2] - radius[1], rect[1] + radius[1] },
		                        { rect[0] + rect[2] - radius[2], rect[1] + rect[3] - radius[2] },
		                        { rect[0] + radius[3], rect[1] + rect[3] - radius[3] } };
	const int signs[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	for (int i = 0; i < 4; i++) {
		const double dx = x + 0.5 - corners[i][0];
		const double dy = y + 0.5 - corners[i][1];
		if (dx * signs[i][0] > 0 && dy * signs[i][1] > 0 && dx * dx + dy * dy > (double) radius[i] * radius[i]) {
			return false;
		}
	}
	return true;
}

static bool inCircle(int x, int y, double cx, double cy, double r) {
	const double dx = x + 0.5 - cx;
	const double dy = y + 0.5 - cy;
	return dx * dx + dy * dy <= r * r;
}

static bool inCapsule(int x, int y, double x0, double y0, double x1, double y1, double r) {
	const double px = x + 0.5 - x0;
	const double py = y + 0.5 - y0;
	const double dx = x1 - x0;
	const double dy = y1 - y0;
	double t = (px * dx + py * dy) / (dx * dx + dy * dy);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	const double ex = px - t * dx;
	const double ey = py - t * dy;
	return ex * ex + ey * ey <= r * r;
}

// A raised round button: lit from the top left; held, it's sunk a pixel and lit
static void drawButton(struct Canvas* canvas, double cx, double cy, double r, bool isHeld) {
	if (isHeld) {
		cx += 1;
		cy += 1;
	}
	for (int y = (int) (cy - r - 3); y <= (int) (cy + r + 3); y++) {
		for (int x = (int) (cx - r - 3); x <= (int) (cx + r + 3); x++) {
			if (inCircle(x, y, cx, cy, r)) {
				uint32_t color = isHeld ? LIT : BUTTON;
				if (!isHeld && !inCircle(x - 2, y - 2, cx, cy, r)) {
					color = BUTTON_LIGHT;
				} else if (!isHeld && !inCircle(x + 2, y + 2, cx, cy, r)) {
					color = BUTTON_SHADE;
				} else if (isHeld && inCircle(x + 3, y + 3, cx - 1, cy - 1, r * 0.45)) {
					color = LIT_GLOW;
				}
				put(canvas, x, y, color);
			} else if (isHeld && inCircle(x, y, cx, cy, r + 2.5)) {
				// A glow around it while held
				put(canvas, x, y, mix(get(canvas, x, y), LIT));
			}
		}
	}
}

static void drawPill(struct Canvas* canvas, const int pill[4], bool isHeld) {
	const int shift = isHeld ? 1 : 0;
	const double r = 4;
	for (int y = pill[3] - 6; y <= pill[1] + 7; y++) {
		for (int x = pill[0] - 6; x <= pill[2] + 7; x++) {
			if (inCapsule(x, y, pill[0] + shift, pill[1] + shift, pill[2] + shift, pill[3] + shift, r)) {
				uint32_t color = isHeld ? LIT : PILL;
				if (!isHeld && !inCapsule(x - 1, y - 1, pill[0], pill[1], pill[2], pill[3], r)) {
					color = PILL_LIGHT;
				}
				put(canvas, x, y, color);
			} else if (isHeld && inCapsule(x, y, pill[0] + 1, pill[1] + 1, pill[2] + 1, pill[3] + 1, r + 2)) {
				put(canvas, x, y, mix(get(canvas, x, y), LIT));
			}
		}
	}
}

// The cross: each arm lit as its direction is held
static void drawPad(struct Canvas* canvas, int cx, int cy, uint32_t keys) {
	const int reach = 17;
	const int half = 6;
	for (int y = cy - reach - 1; y <= cy + reach + 1; y++) {
		for (int x = cx - reach - 1; x <= cx + reach + 1; x++) {
			const int dx = x - cx;
			const int dy = y - cy;
			const bool isAcross = dy >= -half && dy < half && dx >= -reach && dx < reach;
			const bool isDown = dx >= -half && dx < half && dy >= -reach && dy < reach;
			if (!isAcross && !isDown) {
				continue;
			}
			uint32_t color = PAD;
			// Raised edges, top and left
			const bool isEdge = (isAcross && dy == -half && (dx < -half || dx >= half)) ||
			                    (isDown && dx == -half && (dy < -half || dy >= half)) || (isDown && dy == -reach) ||
			                    (isAcross && dx == -reach);
			if (isEdge) {
				color = PAD_LIGHT;
			}
			int arm = -1;
			if (dx >= half) arm = KEY_RIGHT;
			else if (dx < -half) arm = KEY_LEFT;
			else if (dy < -half) arm = KEY_UP;
			else if (dy >= half) arm = KEY_DOWN;
			if (arm >= 0 && (keys & (1u << arm))) {
				const int distance = arm == KEY_RIGHT ? dx : arm == KEY_LEFT ? -dx - 1 : arm == KEY_UP ? -dy - 1 : dy;
				color = distance > reach - 4 ? LIT_GLOW : LIT;
			}
			put(canvas, x, y, color);
		}
	}
	// The dimple in the middle
	for (int y = cy - 3; y < cy + 3; y++) {
		for (int x = cx - 3; x < cx + 3; x++) {
			if (inCircle(x, y, cx, cy, 3)) {
				put(canvas, x, y, PAD_LIGHT);
			}
		}
	}
}

// --- Letters ---------------------------------------------------------------

// Three by five, for labels
static const struct {
	char c;
	uint8_t rows[5];
} SMALL[] = {
	{ 'A', { 2, 5, 7, 5, 5 } }, { 'B', { 6, 5, 6, 5, 6 } }, { 'C', { 3, 4, 4, 4, 3 } }, { 'D', { 6, 5, 5, 5, 6 } },
	{ 'E', { 7, 4, 6, 4, 7 } }, { 'L', { 4, 4, 4, 4, 7 } }, { 'O', { 2, 5, 5, 5, 2 } }, { 'R', { 6, 5, 6, 5, 5 } },
	{ 'S', { 3, 4, 2, 1, 6 } }, { 'T', { 7, 2, 2, 2, 2 } }, { 'U', { 5, 5, 5, 5, 7 } }, { 'X', { 5, 5, 2, 5, 5 } },
	{ '1', { 2, 6, 2, 2, 7 } }, { '2', { 6, 1, 2, 4, 7 } }, { '3', { 7, 1, 3, 1, 7 } }, { '4', { 5, 5, 7, 1, 1 } },
};

static void drawSmall(struct Canvas* canvas, int x, int y, const char* text, uint32_t color) {
	for (; *text; text++, x += 4) {
		for (size_t i = 0; i < sizeof(SMALL) / sizeof(SMALL[0]); i++) {
			if (SMALL[i].c != *text) {
				continue;
			}
			for (int row = 0; row < 5; row++) {
				for (int column = 0; column < 3; column++) {
					if (SMALL[i].rows[row] & (4 >> column)) {
						put(canvas, x + column, y + row, color);
					}
				}
			}
		}
	}
}

// Five by seven, drawn bold and leaning, for the name
static const struct {
	char c;
	uint8_t rows[7];
} LARGE[] = {
	{ 'C', { 14, 17, 16, 16, 16, 17, 14 } }, { 'L', { 16, 16, 16, 16, 16, 16, 31 } },
	{ 'A', { 14, 17, 17, 31, 17, 17, 17 } }, { 'U', { 17, 17, 17, 17, 17, 17, 14 } },
	{ 'D', { 30, 17, 17, 17, 17, 17, 30 } }, { 'E', { 31, 16, 16, 30, 16, 16, 31 } },
	{ 'B', { 30, 17, 17, 30, 17, 17, 30 } }, { 'O', { 14, 17, 17, 17, 17, 17, 14 } },
	{ 'Y', { 17, 17, 10, 4, 4, 4, 4 } },
};

static void drawName(struct Canvas* canvas, int x, int y, const char* text, uint32_t color) {
	for (; *text; text++, x += 7) {
		for (size_t i = 0; i < sizeof(LARGE) / sizeof(LARGE[0]); i++) {
			if (LARGE[i].c != *text) {
				continue;
			}
			for (int row = 0; row < 7; row++) {
				const int lean = (6 - row) / 3;
				for (int column = 0; column < 5; column++) {
					if (LARGE[i].rows[row] & (16 >> column)) {
						put(canvas, x + column + lean, y + row, color);
						put(canvas, x + column + lean + 1, y + row, color);
					}
				}
			}
		}
	}
}

// Claude's spark: tapering rays of uneven length around a round heart
static void drawSpark(struct Canvas* canvas, int cx, int cy, uint32_t color) {
	static const double LENGTHS[12] = { 7.4, 5.6, 6.8, 5.2, 7.2, 6.0, 5.4, 7.4, 5.0, 6.6, 6.2, 5.6 };
	for (int y = cy - 8; y <= cy + 8; y++) {
		for (int x = cx - 8; x <= cx + 8; x++) {
			const double dx = x + 0.5 - cx;
			const double dy = y + 0.5 - cy;
			const double distance = sqrt(dx * dx + dy * dy);
			bool isIn = distance <= 1.9;
			for (int ray = 0; ray < 12 && !isIn; ray++) {
				const double angle = ray * M_PI / 6 + 0.2;
				const double along = dx * cos(angle) + dy * sin(angle);
				const double across = fabs(-dx * sin(angle) + dy * cos(angle));
				isIn = along > 0 && along <= LENGTHS[ray] && across <= 1.25 * (1 - along / LENGTHS[ray]) + 0.35;
			}
			if (isIn) {
				put(canvas, x, y, color);
			}
		}
	}
}

void consoleDraw(const uint32_t* screen, uint32_t keys, bool isSpeedHeld, int speed, uint32_t* out) {
	const struct Layout* layout = &LAYOUT;
	struct Canvas canvas = { out, CONSOLE_WIDTH, CONSOLE_HEIGHT };
	const int body[4] = { 0, 0, CONSOLE_WIDTH, CONSOLE_HEIGHT };

	// The body, bevelled: lit along its top and left, shaded along the rest
	for (int y = 0; y < CONSOLE_HEIGHT; y++) {
		for (int x = 0; x < CONSOLE_WIDTH; x++) {
			uint32_t color = 0;
			if (inRoundRect(x, y, body, layout->bodyRadius)) {
				color = BODY;
				if (!inRoundRect(x - 2, y - 2, body, layout->bodyRadius)) {
					color = BODY_LIGHT;
				} else if (!inRoundRect(x + 3, y + 3, body, layout->bodyRadius)) {
					color = BODY_SHADE;
				}
			}
			out[y * CONSOLE_WIDTH + x] = color;
		}
	}

	// The bezel, sunk: shaded along its top and left
	const int* bezel = layout->bezel;
	for (int y = bezel[1]; y < bezel[1] + bezel[3]; y++) {
		for (int x = bezel[0]; x < bezel[0] + bezel[2]; x++) {
			if (!inRoundRect(x, y, bezel, layout->bezelRadius)) {
				continue;
			}
			uint32_t color = BEZEL;
			if (!inRoundRect(x - 2, y - 2, bezel, layout->bezelRadius)) {
				color = BEZEL_DEEP;
			} else if (!inRoundRect(x + 1, y + 1, bezel, layout->bezelRadius)) {
				color = BEZEL_EDGE;
			}
			put(&canvas, x, y, color);
		}
	}
	// Its stripe and label, in Claude's colors
	for (int x = layout->stripeLeft; x < layout->stripe[1]; x++) {
		put(&canvas, x, layout->stripe[0], BODY);
		put(&canvas, x, layout->stripe[0] + 3, CREAM);
	}
	for (int x = layout->stripe[3]; x < layout->stripeRight; x++) {
		put(&canvas, x, layout->stripe[0], BODY);
		put(&canvas, x, layout->stripe[0] + 3, CREAM);
	}
	drawSmall(&canvas, layout->stripe[2], layout->stripe[0] - 1, "CLAUDE CODE", CREAM);

	// The screen, edged in shadow
	for (int y = 0; y < CONSOLE_SCREEN_HEIGHT; y++) {
		memcpy(&out[(layout->screenY + y) * CONSOLE_WIDTH + layout->screenX], &screen[y * CONSOLE_SCREEN_WIDTH],
		       CONSOLE_SCREEN_WIDTH * sizeof(uint32_t));
		for (int x = 0; x < CONSOLE_SCREEN_WIDTH; x++) {
			out[(layout->screenY + y) * CONSOLE_WIDTH + layout->screenX + x] |= 0xFF000000u;
		}
	}
	for (int x = -1; x <= CONSOLE_SCREEN_WIDTH; x++) {
		put(&canvas, layout->screenX + x, layout->screenY - 1, BEZEL_DEEP);
	}
	for (int y = 0; y < CONSOLE_SCREEN_HEIGHT; y++) {
		put(&canvas, layout->screenX - 1, layout->screenY + y, BEZEL_DEEP);
	}

	// The power light, glowing while the game runs
	for (int y = layout->led[1] - 5; y <= layout->led[1] + 5; y++) {
		for (int x = layout->led[0] - 5; x <= layout->led[0] + 5; x++) {
			if (inCircle(x, y, layout->led[0], layout->led[1], 3)) {
				put(&canvas, x, y, inCircle(x, y, layout->led[0] - 1, layout->led[1] - 1, 1.2) ? LED_GLOW : LED_ON);
			} else if (inCircle(x, y, layout->led[0], layout->led[1], 5)) {
				put(&canvas, x, y, mix(get(&canvas, x, y), LED_ON));
			}
		}
	}

	drawSpark(&canvas, layout->logo[0], layout->logo[1], CREAM);
	drawName(&canvas, layout->logo[0] + 11, layout->logo[1] - 4, "CLAUDEBOY", INK);

	drawPad(&canvas, layout->pad[0], layout->pad[1], keys);

	// A and B in their slanted recess
	for (int y = layout->a[1] - 18; y <= layout->b[1] + 18; y++) {
		for (int x = layout->b[0] - 18; x <= layout->a[0] + 18; x++) {
			if (inCapsule(x, y, layout->b[0], layout->b[1], layout->a[0], layout->a[1], layout->buttonRadius + 4)) {
				put(&canvas, x, y, RECESS);
			}
		}
	}
	const int r = layout->buttonRadius;
	drawButton(&canvas, layout->b[0], layout->b[1], r, keys & (1u << KEY_B));
	drawButton(&canvas, layout->a[0], layout->a[1], r, keys & (1u << KEY_A));
	drawSmall(&canvas, layout->b[0] - 1, layout->b[1] + r + 4, "B", INK);
	drawSmall(&canvas, layout->a[0] - 1, layout->a[1] + r + 4, "A", INK);

	drawPill(&canvas, layout->pills[0], keys & (1u << KEY_SELECT));
	drawPill(&canvas, layout->pills[1], keys & (1u << KEY_START));
	drawPill(&canvas, layout->pills[2], isSpeedHeld || speed > 1);
	char speedLabel[4] = { 'X', (char) ('0' + (speed > 9 ? 9 : speed < 1 ? 1 : speed)), 0, 0 };
	drawSmall(&canvas, layout->pillLabels[0][0], layout->pillLabels[0][1], "SELECT", INK);
	drawSmall(&canvas, layout->pillLabels[1][0], layout->pillLabels[1][1], "START", INK);
	drawSmall(&canvas, layout->pillLabels[2][0], layout->pillLabels[2][1], speedLabel, INK);

	// The speaker's slots, where they fall on the body
	for (int i = 0; i < 4; i++) {
		const int* slot = layout->slot[i];
		for (int y = slot[3] - 3; y <= slot[1] + 3; y++) {
			for (int x = slot[0] - 3; x <= slot[2] + 3; x++) {
				if (inCapsule(x, y, slot[0], slot[1], slot[2], slot[3], 1.6) &&
				    inRoundRect(x + 3, y + 3, body, layout->bodyRadius)) {
					put(&canvas, x, y, SLOT);
				}
			}
		}
	}
}

int consoleButtonAt(double x, double y) {
	const struct Layout* layout = &LAYOUT;
	const double dx = x - layout->pad[0];
	const double dy = y - layout->pad[1];
	// The cross, a little generous, by its strongest direction
	if (fabs(dx) <= 21 && fabs(dy) <= 21 && (fabs(dx) > 3 || fabs(dy) > 3)) {
		if (fabs(dx) >= fabs(dy)) {
			return dx > 0 ? KEY_RIGHT : KEY_LEFT;
		}
		return dy > 0 ? KEY_DOWN : KEY_UP;
	}
	const int px = (int) x;
	const int py = (int) y;
	if (inCircle(px, py, layout->a[0], layout->a[1], layout->buttonRadius + 3)) {
		return KEY_A;
	}
	if (inCircle(px, py, layout->b[0], layout->b[1], layout->buttonRadius + 3)) {
		return KEY_B;
	}
	const int buttons[3] = { KEY_SELECT, KEY_START, CONSOLE_SPEED };
	for (int i = 0; i < 3; i++) {
		const int* pill = layout->pills[i];
		if (inCapsule(px, py, pill[0], pill[1], pill[2], pill[3], 7)) {
			return buttons[i];
		}
	}
	return -1;
}
