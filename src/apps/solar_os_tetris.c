#include "solar_os_tetris.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define TETRIS_COLS 10
#define TETRIS_ROWS 20
#define TETRIS_HEADER 16
#define TETRIS_FOOTER 14
#define TETRIS_TICK_MS 40U

typedef enum {
    TETRIS_PLAYING,
    TETRIS_DEAD,
} tetris_mode_t;

typedef struct {
    int8_t x;
    int8_t y;
} tetris_mino_t;

typedef struct {
    uint8_t board[TETRIS_ROWS][TETRIS_COLS];
    uint8_t bag[7];
    int bag_i;
    int kind;
    int rot;
    int x;
    int y;
    int next_kind;
    int cell;
    int origin_x;
    int origin_y;
    int panel_x;
    bool paused;
    bool timers_started;
    tetris_mode_t mode;
    uint32_t score;
    uint32_t best;
    uint32_t lines;
    uint32_t rng;
    uint32_t last_step_ms;
} tetris_state_t;

static void *tetris_state;
#define tetris (*(tetris_state_t *)tetris_state)
static uint32_t tetris_boot_rng = 0x9e3779b9u;

/* Four rotations of I O T S Z J L, each as four cells in a 4x4 box. */
static const tetris_mino_t TETRIS_SHAPES[7][4][4] = {
    {
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
        {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
        {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
        {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
    },
    {
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
    },
    {
        {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    {
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
        {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    {
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
        {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {0, 2}},
    },
    {
        {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 0}, {1, 1}, {0, 2}, {1, 2}},
    },
    {
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
        {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
    },
};

static uint32_t tetris_random(void)
{
    tetris.rng = (tetris.rng * 1664525U) + 1013904223U;
    return tetris.rng;
}

static int tetris_level(void)
{
    return (int)(tetris.lines / 10U);
}

static uint32_t tetris_step_ms(void)
{
    const uint32_t faster = (uint32_t)tetris_level() * 40U;
    uint32_t step = 500U;
    if (faster < step) {
        step -= faster;
    }
    if (step < 80U) {
        step = 80U;
    }
    return step;
}

static void tetris_fit_view(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const int top = TETRIS_HEADER;
    const int avail_h = height > (top + TETRIS_FOOTER + 4)
        ? height - top - TETRIS_FOOTER - 4
        : TETRIS_ROWS;
    const int panel = width > 180 ? 78 : 0;
    int cell_h = avail_h / TETRIS_ROWS;
    int cell_w = (width - panel - 12) / TETRIS_COLS;
    int cell = cell_h < cell_w ? cell_h : cell_w;
    if (cell < 3) {
        cell = 3;
    }
    if (cell > 16) {
        cell = 16;
    }
    const int well_w = cell * TETRIS_COLS;
    const int well_h = cell * TETRIS_ROWS;
    int origin_x = 6;
    if (panel > 0 && width - well_w - panel > 8) {
        origin_x = (width - panel - well_w) / 2;
    } else if (panel == 0) {
        origin_x = (width - well_w) / 2;
    }
    if (origin_x < 2) {
        origin_x = 2;
    }
    int origin_y = top + ((height - top - TETRIS_FOOTER) - well_h) / 2;
    if (origin_y < top) {
        origin_y = top;
    }
    tetris.cell = cell;
    tetris.origin_x = origin_x;
    tetris.origin_y = origin_y;
    tetris.panel_x = origin_x + well_w + 8;
}

static bool tetris_fits(int kind, int rot, int x, int y)
{
    for (int i = 0; i < 4; i++) {
        const int cx = x + TETRIS_SHAPES[kind][rot][i].x;
        const int cy = y + TETRIS_SHAPES[kind][rot][i].y;
        if (cx < 0 || cx >= TETRIS_COLS || cy >= TETRIS_ROWS) {
            return false;
        }
        if (cy >= 0 && tetris.board[cy][cx] != 0) {
            return false;
        }
    }
    return true;
}

static int tetris_take_kind(void)
{
    if (tetris.bag_i >= 7) {
        for (int i = 0; i < 7; i++) {
            tetris.bag[i] = (uint8_t)i;
        }
        for (int i = 6; i > 0; i--) {
            const int j = (int)(tetris_random() % (uint32_t)(i + 1));
            const uint8_t swap = tetris.bag[i];
            tetris.bag[i] = tetris.bag[j];
            tetris.bag[j] = swap;
        }
        tetris.bag_i = 0;
    }
    return (int)tetris.bag[tetris.bag_i++];
}

static void tetris_remember_best(void)
{
    if (tetris.score > tetris.best) {
        tetris.best = tetris.score;
    }
}

static int tetris_clear_lines(void)
{
    int cleared = 0;
    int dst = TETRIS_ROWS - 1;
    for (int src = TETRIS_ROWS - 1; src >= 0; src--) {
        bool full = true;
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (tetris.board[src][x] == 0) {
                full = false;
                break;
            }
        }
        if (full) {
            cleared++;
            continue;
        }
        if (dst != src) {
            memcpy(tetris.board[dst], tetris.board[src], TETRIS_COLS);
        }
        dst--;
    }
    while (dst >= 0) {
        memset(tetris.board[dst], 0, TETRIS_COLS);
        dst--;
    }
    return cleared;
}

static void tetris_spawn(void);

static void tetris_lock(void)
{
    for (int i = 0; i < 4; i++) {
        const int cx = tetris.x + TETRIS_SHAPES[tetris.kind][tetris.rot][i].x;
        const int cy = tetris.y + TETRIS_SHAPES[tetris.kind][tetris.rot][i].y;
        if (cy < 0 || cy >= TETRIS_ROWS || cx < 0 || cx >= TETRIS_COLS) {
            tetris.mode = TETRIS_DEAD;
            tetris.paused = false;
            tetris_remember_best();
            return;
        }
        tetris.board[cy][cx] = 1;
    }
    const int cleared = tetris_clear_lines();
    if (cleared > 0) {
        static const uint32_t points[] = {0U, 100U, 300U, 500U, 800U};
        const uint32_t multiplier = (uint32_t)tetris_level() + 1U;
        tetris.lines += (uint32_t)cleared;
        tetris.score += points[cleared] * multiplier;
    }
    tetris_remember_best();
    tetris_spawn();
}

static void tetris_spawn(void)
{
    tetris.kind = tetris.next_kind;
    tetris.rot = 0;
    tetris.x = 3;
    tetris.y = 0;
    tetris.next_kind = tetris_take_kind();
    if (!tetris_fits(tetris.kind, tetris.rot, tetris.x, tetris.y)) {
        tetris.mode = TETRIS_DEAD;
        tetris.paused = false;
        tetris_remember_best();
    }
}

static bool tetris_rotate(void)
{
    const int next = (tetris.rot + 1) & 3;
    static const int kicks[] = {0, -1, 1, -2, 2};
    for (size_t i = 0; i < sizeof(kicks) / sizeof(kicks[0]); i++) {
        if (tetris_fits(tetris.kind, next, tetris.x + kicks[i], tetris.y)) {
            tetris.rot = next;
            tetris.x += kicks[i];
            return true;
        }
    }
    if (tetris.kind == 0) {
        for (size_t i = 0; i < sizeof(kicks) / sizeof(kicks[0]); i++) {
            if (tetris_fits(tetris.kind, next, tetris.x + kicks[i], tetris.y - 1)) {
                tetris.rot = next;
                tetris.x += kicks[i];
                tetris.y -= 1;
                return true;
            }
            if (tetris_fits(tetris.kind, next, tetris.x + kicks[i], tetris.y + 1)) {
                tetris.rot = next;
                tetris.x += kicks[i];
                tetris.y += 1;
                return true;
            }
        }
    }
    return false;
}

static void tetris_hard_drop(void)
{
    while (tetris_fits(tetris.kind, tetris.rot, tetris.x, tetris.y + 1)) {
        tetris.y++;
        tetris.score += 2U;
    }
    tetris_lock();
}

static void tetris_reset(solar_os_gfx_t *gfx)
{
    const uint32_t best = tetris.best;
    memset(&tetris, 0, sizeof(tetris));
    tetris.best = best;
    tetris.rng = tetris_boot_rng;
    tetris_boot_rng = (tetris_boot_rng * 1664525U) + 1013904223U;
    tetris.bag_i = 7;
    tetris.next_kind = tetris_take_kind();
    tetris.mode = TETRIS_PLAYING;
    tetris_fit_view(gfx);
    tetris_spawn();
}

static void tetris_gravity(void)
{
    if (tetris.mode != TETRIS_PLAYING || tetris.paused) {
        return;
    }
    if (tetris_fits(tetris.kind, tetris.rot, tetris.x, tetris.y + 1)) {
        tetris.y++;
        return;
    }
    tetris_lock();
}

static void tetris_draw_mino(solar_os_gfx_t *gfx,
                             int origin_x,
                             int origin_y,
                             int cell,
                             int kind,
                             int rot,
                             int px,
                             int py,
                             bool active)
{
    const int inset = cell >= 6 ? 1 : 0;
    const int size = cell - (inset * 2) > 0 ? cell - (inset * 2) : 1;
    solar_os_gfx_set_color(gfx, active ? SOLAR_OS_GFX_COLOR_BLACK
                                       : SOLAR_OS_GFX_COLOR_DARK);
    for (int i = 0; i < 4; i++) {
        const int x = origin_x + ((px + TETRIS_SHAPES[kind][rot][i].x) * cell) + inset;
        const int y = origin_y + ((py + TETRIS_SHAPES[kind][rot][i].y) * cell) + inset;
        if (y + size <= origin_y) {
            continue;
        }
        solar_os_gfx_fill_rect(gfx, x, y, size, size);
    }
}

static void tetris_banner(solar_os_gfx_t *gfx, const char *text)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const int text_w = (int)solar_os_gfx_text_width(gfx, text);
    const int x = tetris.origin_x + ((tetris.cell * TETRIS_COLS) - text_w) / 2;
    const int baseline = tetris.origin_y + (tetris.cell * TETRIS_ROWS) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x - 6, baseline - 16, text_w + 12, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x - 6, baseline - 16, text_w + 12, 24);
    solar_os_gfx_text(gfx, x, baseline, text);
}

static void tetris_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    char line[48];
    const int screen_h = (int)solar_os_gfx_height(gfx);
    const int cell = tetris.cell;
    const int inset = cell >= 6 ? 1 : 0;
    const int size = cell - (inset * 2) > 0 ? cell - (inset * 2) : 1;

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    snprintf(line,
             sizeof(line),
             "tetris  %lu  L%lu  lv%d",
             (unsigned long)tetris.score,
             (unsigned long)tetris.lines,
             tetris_level() + 1);
    solar_os_gfx_text(gfx, 4, 12, line);

    solar_os_gfx_rect(gfx,
                      tetris.origin_x - 1,
                      tetris.origin_y - 1,
                      (cell * TETRIS_COLS) + 2,
                      (cell * TETRIS_ROWS) + 2);
    for (int y = 0; y < TETRIS_ROWS; y++) {
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (tetris.board[y][x] == 0) {
                continue;
            }
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_fill_rect(gfx,
                                   tetris.origin_x + (x * cell) + inset,
                                   tetris.origin_y + (y * cell) + inset,
                                   size,
                                   size);
        }
    }
    if (tetris.mode == TETRIS_PLAYING) {
        tetris_draw_mino(gfx,
                         tetris.origin_x,
                         tetris.origin_y,
                         cell,
                         tetris.kind,
                         tetris.rot,
                         tetris.x,
                         tetris.y,
                         true);
    }

    if (tetris.panel_x + (cell * 4) < (int)solar_os_gfx_width(gfx)) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, tetris.panel_x, tetris.origin_y + 12, "next");
        snprintf(line, sizeof(line), "best %lu", (unsigned long)tetris.best);
        solar_os_gfx_text(gfx, tetris.panel_x, tetris.origin_y + 28, line);
        tetris_draw_mino(gfx,
                         tetris.panel_x,
                         tetris.origin_y + 40,
                         cell > 8 ? 8 : cell,
                         tetris.next_kind,
                         0,
                         0,
                         0,
                         true);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 4, screen_h - 3, "A/D move  S drop  W spin  space hard  Q quit");

    if (tetris.paused && tetris.mode == TETRIS_PLAYING) {
        tetris_banner(gfx, "PAUSED");
    } else if (tetris.mode == TETRIS_DEAD) {
        tetris_banner(gfx, "GAME OVER");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t tetris_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    tetris_reset(gfx);
    tetris_render(ctx);
    return ESP_OK;
}

static void tetris_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool tetris_playing_key(solar_os_context_t *ctx, uint8_t ch)
{
    bool moved = false;
    switch (ch) {
    case SOLAR_OS_KEY_LEFT:
    case 'a':
    case 'A':
    case 'h':
    case 'H':
        if (tetris_fits(tetris.kind, tetris.rot, tetris.x - 1, tetris.y)) {
            tetris.x--;
            moved = true;
        }
        break;
    case SOLAR_OS_KEY_RIGHT:
    case 'd':
    case 'D':
    case 'l':
    case 'L':
        if (tetris_fits(tetris.kind, tetris.rot, tetris.x + 1, tetris.y)) {
            tetris.x++;
            moved = true;
        }
        break;
    case SOLAR_OS_KEY_DOWN:
    case 's':
    case 'S':
    case 'j':
    case 'J':
        if (tetris_fits(tetris.kind, tetris.rot, tetris.x, tetris.y + 1)) {
            tetris.y++;
            tetris.score += 1U;
            moved = true;
        }
        break;
    case SOLAR_OS_KEY_UP:
    case 'w':
    case 'W':
    case 'k':
    case 'K':
        moved = tetris_rotate();
        break;
    case ' ':
        tetris_hard_drop();
        moved = true;
        break;
    case 'p':
    case 'P':
        tetris.paused = !tetris.paused;
        moved = true;
        break;
    default:
        return false;
    }
    if (moved) {
        tetris_render(ctx);
    }
    return true;
}

static bool tetris_handle_char(solar_os_context_t *ctx, uint8_t ch)
{
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (tetris.mode != TETRIS_PLAYING) {
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == 'r' || ch == 'R') {
            solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
            if (gfx != NULL) {
                tetris_reset(gfx);
                tetris.timers_started = false;
                tetris_render(ctx);
            }
        }
        return true;
    }
    if (tetris.paused && ch != 'p' && ch != 'P') {
        return true;
    }
    (void)tetris_playing_key(ctx, ch);
    return true;
}

static bool tetris_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return tetris_handle_char(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!tetris.timers_started) {
            tetris.timers_started = true;
            tetris.last_step_ms = event->data.tick_ms;
            return true;
        }
        if (tetris.mode == TETRIS_PLAYING && !tetris.paused &&
            event->data.tick_ms - tetris.last_step_ms >= tetris_step_ms()) {
            tetris.last_step_ms = event->data.tick_ms;
            tetris_gravity();
            tetris_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        tetris_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_tetris_app = {
    .name = "tetris",
    .summary = "classic falling blocks",
    .start = tetris_start,
    .stop = tetris_stop,
    .event = tetris_event,
    .state_slot = &tetris_state,
    .state_size = sizeof(tetris_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .tick_interval_ms = TETRIS_TICK_MS,
    .tick_deadline_ms = 25U,
};
