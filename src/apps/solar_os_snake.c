#include "solar_os_snake.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define SNAKE_MAX_COLS 40
#define SNAKE_MAX_ROWS 28
#define SNAKE_MAX_LEN (SNAKE_MAX_COLS * SNAKE_MAX_ROWS)
#define SNAKE_HEADER 16
#define SNAKE_FOOTER 14
#define SNAKE_TICK_MS 40U

typedef enum {
    SNAKE_PLAYING,
    SNAKE_DEAD,
    SNAKE_WON,
} snake_mode_t;

typedef struct {
    uint8_t bx[SNAKE_MAX_LEN];
    uint8_t by[SNAKE_MAX_LEN];
    int head;
    int len;
    int dir;
    int pending_dir;
    int cols;
    int rows;
    int cell;
    int origin_x;
    int origin_y;
    int food_x;
    int food_y;
    bool food_on;
    bool paused;
    bool timers_started;
    snake_mode_t mode;
    uint32_t score;
    uint32_t best;
    uint32_t rng;
    uint32_t last_step_ms;
} snake_state_t;

static void *snake_state;
#define snake (*(snake_state_t *)snake_state)
static uint32_t snake_boot_rng = 0x51f15eadu;

static uint32_t snake_random(void)
{
    snake.rng = (snake.rng * 1664525U) + 1013904223U;
    return snake.rng;
}

static int snake_opposite(int dir)
{
    return (dir + 2) & 3;
}

static void snake_delta(int dir, int *dx, int *dy)
{
    *dx = 0;
    *dy = 0;
    if (dir == 0) {
        *dy = -1;
    } else if (dir == 1) {
        *dx = 1;
    } else if (dir == 2) {
        *dy = 1;
    } else {
        *dx = -1;
    }
}

static int snake_dir_from_key(uint8_t ch)
{
    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'w':
    case 'W':
    case 'k':
    case 'K':
        return 0;
    case SOLAR_OS_KEY_RIGHT:
    case 'd':
    case 'D':
    case 'l':
    case 'L':
        return 1;
    case SOLAR_OS_KEY_DOWN:
    case 's':
    case 'S':
    case 'j':
    case 'J':
        return 2;
    case SOLAR_OS_KEY_LEFT:
    case 'a':
    case 'A':
    case 'h':
    case 'H':
        return 3;
    default:
        return -1;
    }
}

static bool snake_hits(int x, int y, bool ignore_tail)
{
    const int limit = snake.len - (ignore_tail ? 1 : 0);
    for (int i = 0; i < limit; i++) {
        const int idx = (snake.head - i + SNAKE_MAX_LEN) % SNAKE_MAX_LEN;
        if ((int)snake.bx[idx] == x && (int)snake.by[idx] == y) {
            return true;
        }
    }
    return false;
}

static void snake_fit(solar_os_gfx_t *gfx)
{
    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const int avail_w = width > 4 ? width - 4 : 1;
    const int chrome = SNAKE_HEADER + SNAKE_FOOTER + 4;
    const int avail_h = height > chrome ? height - chrome : 1;
    int cell = 8;

    for (int candidate = 16; candidate >= 6; candidate--) {
        int cols = avail_w / candidate;
        int rows = avail_h / candidate;
        if (cols > SNAKE_MAX_COLS) {
            cols = SNAKE_MAX_COLS;
        }
        if (rows > SNAKE_MAX_ROWS) {
            rows = SNAKE_MAX_ROWS;
        }
        if (cols >= 16 && rows >= 12) {
            cell = candidate;
            break;
        }
    }

    int cols = avail_w / cell;
    int rows = avail_h / cell;
    if (cols > SNAKE_MAX_COLS) {
        cols = SNAKE_MAX_COLS;
    }
    if (rows > SNAKE_MAX_ROWS) {
        rows = SNAKE_MAX_ROWS;
    }
    if (cols < 4) {
        cols = 4;
    }
    if (rows < 4) {
        rows = 4;
    }
    while (cell > 4 && (cell * cols > avail_w || cell * rows > avail_h)) {
        cell--;
    }
    if (cell * cols > avail_w) {
        cols = avail_w / cell;
    }
    if (cell * rows > avail_h) {
        rows = avail_h / cell;
    }
    if (cols < 4) {
        cols = 4;
    }
    if (rows < 4) {
        rows = 4;
    }
    if (cols > SNAKE_MAX_COLS) {
        cols = SNAKE_MAX_COLS;
    }
    if (rows > SNAKE_MAX_ROWS) {
        rows = SNAKE_MAX_ROWS;
    }

    snake.cell = cell;
    snake.cols = cols;
    snake.rows = rows;
    snake.origin_x = (width - (cell * cols)) / 2;
    snake.origin_y = SNAKE_HEADER +
        ((height - SNAKE_HEADER - SNAKE_FOOTER) - (cell * rows)) / 2;
    if (snake.origin_y < SNAKE_HEADER) {
        snake.origin_y = SNAKE_HEADER;
    }
}

static void snake_place_food(void)
{
    const int cells = snake.cols * snake.rows;
    if (snake.len >= cells) {
        snake.food_on = false;
        snake.mode = SNAKE_WON;
        return;
    }
    for (int attempt = 0; attempt < cells; attempt++) {
        const int pick = (int)((snake_random() >> 16) % (uint32_t)cells);
        const int x = pick % snake.cols;
        const int y = pick / snake.cols;
        if (!snake_hits(x, y, false)) {
            snake.food_x = x;
            snake.food_y = y;
            snake.food_on = true;
            return;
        }
    }
    snake.food_on = false;
    snake.mode = SNAKE_WON;
}

static uint32_t snake_step_ms(void)
{
    const int bonus = (snake.len / 5) * 12;
    int step = 180 - bonus;
    if (step < 70) {
        step = 70;
    }
    return (uint32_t)step;
}

static void snake_remember_best(void)
{
    if (snake.score > snake.best) {
        snake.best = snake.score;
    }
}

static void snake_reset(solar_os_gfx_t *gfx)
{
    const uint32_t best = snake.best;
    memset(&snake, 0, sizeof(snake));
    snake.best = best;
    snake.rng = snake_boot_rng;
    snake_boot_rng = (snake_boot_rng * 1664525U) + 1013904223U;
    snake_fit(gfx);

    int len = 4;
    if (len >= snake.cols) {
        len = snake.cols > 1 ? snake.cols - 1 : 1;
    }
    int head_x = snake.cols / 2;
    if (head_x < len - 1) {
        head_x = len - 1;
    }
    const int y = snake.rows / 2;
    snake.len = len;
    snake.head = len - 1;
    for (int i = 0; i < len; i++) {
        snake.bx[i] = (uint8_t)(head_x - (len - 1) + i);
        snake.by[i] = (uint8_t)y;
    }
    snake.dir = 1;
    snake.pending_dir = 1;
    snake.mode = SNAKE_PLAYING;
    snake_place_food();
}

static void snake_step(void)
{
    if (snake.mode != SNAKE_PLAYING || snake.paused) {
        return;
    }
    snake.dir = snake.pending_dir;
    int dx = 0;
    int dy = 0;
    snake_delta(snake.dir, &dx, &dy);
    const int hx = (int)snake.bx[snake.head];
    const int hy = (int)snake.by[snake.head];
    const int nx = hx + dx;
    const int ny = hy + dy;
    const bool grow = snake.food_on && nx == snake.food_x && ny == snake.food_y;
    if (nx < 0 || ny < 0 || nx >= snake.cols || ny >= snake.rows ||
        snake_hits(nx, ny, !grow)) {
        snake.mode = SNAKE_DEAD;
        snake.paused = false;
        snake_remember_best();
        return;
    }
    snake.head = (snake.head + 1) % SNAKE_MAX_LEN;
    snake.bx[snake.head] = (uint8_t)nx;
    snake.by[snake.head] = (uint8_t)ny;
    if (!grow) {
        return;
    }
    snake.len++;
    snake.score += 10U;
    snake_remember_best();
    snake_place_food();
}

static void snake_cell_rect(int x, int y, int *px, int *py, int *size)
{
    const int inset = snake.cell >= 6 ? 1 : 0;
    *px = snake.origin_x + (x * snake.cell) + inset;
    *py = snake.origin_y + (y * snake.cell) + inset;
    *size = snake.cell - (inset * 2);
    if (*size < 1) {
        *size = 1;
    }
}

static void snake_banner(solar_os_gfx_t *gfx, const char *text)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const int text_w = (int)solar_os_gfx_text_width(gfx, text);
    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);
    const int x = (screen_w - text_w) / 2;
    const int baseline = screen_h / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x - 8, baseline - 18, text_w + 16, 28);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x - 8, baseline - 18, text_w + 16, 28);
    solar_os_gfx_text(gfx, x, baseline, text);
}

static void snake_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    const int screen_h = (int)solar_os_gfx_height(gfx);
    char hud[64];

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    snprintf(hud,
             sizeof(hud),
             "snake  %lu  best %lu",
             (unsigned long)snake.score,
             (unsigned long)snake.best);
    solar_os_gfx_text(gfx, 4, 12, hud);
    solar_os_gfx_rect(gfx,
                      snake.origin_x - 1,
                      snake.origin_y - 1,
                      (snake.cell * snake.cols) + 2,
                      (snake.cell * snake.rows) + 2);

    for (int i = 0; i < snake.len; i++) {
        const int idx = (snake.head - i + SNAKE_MAX_LEN) % SNAKE_MAX_LEN;
        int x = 0;
        int y = 0;
        int size = 1;
        snake_cell_rect((int)snake.bx[idx], (int)snake.by[idx], &x, &y, &size);
        solar_os_gfx_set_color(gfx, i == 0 ? SOLAR_OS_GFX_COLOR_BLACK
                                           : SOLAR_OS_GFX_COLOR_DARK);
        solar_os_gfx_fill_rect(gfx, x, y, size, size);
    }
    if (snake.food_on) {
        int x = 0;
        int y = 0;
        int size = 1;
        snake_cell_rect(snake.food_x, snake.food_y, &x, &y, &size);
        const int bite = size > 5 ? 2 : 1;
        const int food = size - (bite * 2);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        if (food >= 1) {
            solar_os_gfx_fill_rect(gfx, x + bite, y + bite, food, food);
        } else {
            solar_os_gfx_fill_rect(gfx, x, y, size, size);
        }
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 4, screen_h - 3, "WASD/HJKL move   P pause   Q quit");

    if (snake.paused && snake.mode == SNAKE_PLAYING) {
        snake_banner(gfx, "PAUSED");
    } else if (snake.mode == SNAKE_DEAD) {
        snake_banner(gfx, "GAME OVER");
    } else if (snake.mode == SNAKE_WON) {
        snake_banner(gfx, "FILLED");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t snake_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    snake_reset(gfx);
    snake_render(ctx);
    return ESP_OK;
}

static void snake_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool snake_handle_char(solar_os_context_t *ctx, uint8_t ch)
{
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (snake.mode != SNAKE_PLAYING) {
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == 'r' || ch == 'R') {
            solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
            if (gfx != NULL) {
                snake_reset(gfx);
                snake.timers_started = false;
                snake_render(ctx);
            }
        }
        return true;
    }

    const int dir = snake_dir_from_key(ch);
    if (dir >= 0 && dir != snake_opposite(snake.pending_dir)) {
        snake.pending_dir = dir;
        return true;
    }
    if (ch == 'p' || ch == 'P' || ch == ' ') {
        snake.paused = !snake.paused;
        snake_render(ctx);
    }
    return true;
}

static bool snake_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return snake_handle_char(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!snake.timers_started) {
            snake.timers_started = true;
            snake.last_step_ms = event->data.tick_ms;
            return true;
        }
        if (snake.mode == SNAKE_PLAYING && !snake.paused &&
            event->data.tick_ms - snake.last_step_ms >= snake_step_ms()) {
            snake.last_step_ms = event->data.tick_ms;
            snake_step();
            snake_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        snake_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_snake_app = {
    .name = "snake",
    .summary = "classic snake",
    .start = snake_start,
    .stop = snake_stop,
    .event = snake_event,
    .state_slot = &snake_state,
    .state_size = sizeof(snake_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .tick_interval_ms = SNAKE_TICK_MS,
    .tick_deadline_ms = 25U,
};
