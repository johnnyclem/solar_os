#include "solar_os_pipes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define PIPE_W 8
#define PIPE_H 6
#define PIPE_QUEUE 4

#define PIPE_NORTH 1U
#define PIPE_EAST 2U
#define PIPE_SOUTH 4U
#define PIPE_WEST 8U

typedef enum {
    PIPES_PLAY,
    PIPES_LEAK,
    PIPES_DONE,
} pipes_mode_t;

typedef struct {
    uint8_t cell[PIPE_H][PIPE_W];
    uint8_t wet[PIPE_H][PIPE_W];
    uint8_t queue[PIPE_QUEUE];
    int cursor_x;
    int cursor_y;
    int head_x;
    int head_y;
    uint8_t entry;
    int score;
    int best;
    int cell_px;
    int origin_x;
    int origin_y;
    uint32_t rng;
    uint32_t last_tick;
    uint32_t flow_ms;
    bool clock;
    pipes_mode_t mode;
} pipes_state_t;

static void *pipes_state;
#define pipes (*(pipes_state_t *)pipes_state)
static uint32_t pipes_boot = 0xc0ffee11u;

static const uint8_t pipes_mask[] = {
    0,
    PIPE_EAST | PIPE_WEST,
    PIPE_NORTH | PIPE_SOUTH,
    PIPE_NORTH | PIPE_EAST,
    PIPE_SOUTH | PIPE_EAST,
    PIPE_SOUTH | PIPE_WEST,
    PIPE_NORTH | PIPE_WEST,
};

static uint32_t pipes_random(void)
{
    pipes.rng = (pipes.rng * 1664525U) + 1013904223U;
    return pipes.rng;
}

static uint8_t pipes_roll(void)
{
    return (uint8_t)(1U + (pipes_random() % 6U));
}

static int pipes_dir(uint8_t ch)
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

static uint8_t pipes_opposite(uint8_t bit)
{
    if (bit == PIPE_NORTH) {
        return PIPE_SOUTH;
    }
    if (bit == PIPE_EAST) {
        return PIPE_WEST;
    }
    if (bit == PIPE_SOUTH) {
        return PIPE_NORTH;
    }
    return PIPE_EAST;
}

static uint8_t pipes_other(uint8_t mask, uint8_t entry)
{
    const uint8_t rest = (uint8_t)(mask & (uint8_t)~entry);
    if ((rest & PIPE_NORTH) != 0) {
        return PIPE_NORTH;
    }
    if ((rest & PIPE_EAST) != 0) {
        return PIPE_EAST;
    }
    if ((rest & PIPE_SOUTH) != 0) {
        return PIPE_SOUTH;
    }
    return PIPE_WEST;
}

static void pipes_step_delta(uint8_t bit, int *dx, int *dy)
{
    *dx = 0;
    *dy = 0;
    if (bit == PIPE_NORTH) {
        *dy = -1;
    } else if (bit == PIPE_EAST) {
        *dx = 1;
    } else if (bit == PIPE_SOUTH) {
        *dy = 1;
    } else {
        *dx = -1;
    }
}

static void pipes_reset(void)
{
    memset(pipes.cell, 0, sizeof(pipes.cell));
    memset(pipes.wet, 0, sizeof(pipes.wet));
    pipes.rng = pipes_boot;
    pipes_boot += 0x9e3779b9U;
    pipes.head_x = 0;
    pipes.head_y = PIPE_H / 2;
    pipes.cell[pipes.head_y][pipes.head_x] = 1;
    pipes.wet[pipes.head_y][pipes.head_x] = 1;
    pipes.entry = PIPE_WEST;
    pipes.cursor_x = 1;
    pipes.cursor_y = pipes.head_y;
    pipes.score = 1;
    pipes.mode = PIPES_PLAY;
    pipes.flow_ms = 1500U;
    pipes.clock = false;
    for (int i = 0; i < PIPE_QUEUE; i++) {
        pipes.queue[i] = pipes_roll();
    }
}

static void pipes_fit(const solar_os_gfx_t *gfx)
{
    const int avail_w = (int)solar_os_gfx_width(gfx) - 90;
    const int avail_h = (int)solar_os_gfx_height(gfx) - 36;
    int cell = avail_w / PIPE_W;
    const int ch = avail_h / PIPE_H;
    if (ch < cell) {
        cell = ch;
    }
    if (cell < 16) {
        cell = 16;
    }
    if (cell > 36) {
        cell = 36;
    }
    pipes.cell_px = cell;
    pipes.origin_x = 8;
    pipes.origin_y = 20;
}

static void pipes_draw_piece(solar_os_gfx_t *gfx, int x, int y, int size, uint8_t kind, bool wet)
{
    const int cx = x + (size / 2);
    const int cy = y + (size / 2);
    const uint8_t mask = kind < 7 ? pipes_mask[kind] : 0;
    solar_os_gfx_set_color(gfx, wet ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_DARK);
    if ((mask & PIPE_NORTH) != 0) {
        solar_os_gfx_line(gfx, cx, cy, cx, y + 2);
    }
    if ((mask & PIPE_SOUTH) != 0) {
        solar_os_gfx_line(gfx, cx, cy, cx, y + size - 3);
    }
    if ((mask & PIPE_EAST) != 0) {
        solar_os_gfx_line(gfx, cx, cy, x + size - 3, cy);
    }
    if ((mask & PIPE_WEST) != 0) {
        solar_os_gfx_line(gfx, cx, cy, x + 2, cy);
    }
}

static void pipes_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    char hud[48];
    if (gfx == NULL) {
        return;
    }
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    snprintf(hud, sizeof(hud), "pipes  %d   best %d", pipes.score, pipes.best);
    solar_os_gfx_text(gfx, 4, 12, hud);

    for (int y = 0; y < PIPE_H; y++) {
        for (int x = 0; x < PIPE_W; x++) {
            const int px = pipes.origin_x + (x * pipes.cell_px);
            const int py = pipes.origin_y + (y * pipes.cell_px);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_rect(gfx, px, py, pipes.cell_px, pipes.cell_px);
            if (pipes.cell[y][x] != 0) {
                pipes_draw_piece(gfx,
                                 px,
                                 py,
                                 pipes.cell_px,
                                 pipes.cell[y][x],
                                 pipes.wet[y][x] != 0);
            }
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx,
                      pipes.origin_x + (pipes.cursor_x * pipes.cell_px) + 1,
                      pipes.origin_y + (pipes.cursor_y * pipes.cell_px) + 1,
                      pipes.cell_px - 2,
                      pipes.cell_px - 2);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, pipes.origin_x + (PIPE_W * pipes.cell_px) + 12, 40, "next");
    for (int i = 0; i < PIPE_QUEUE; i++) {
        const int px = pipes.origin_x + (PIPE_W * pipes.cell_px) + 8;
        const int py = 48 + (i * (pipes.cell_px + 4));
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
        solar_os_gfx_rect(gfx, px, py, pipes.cell_px, pipes.cell_px);
        pipes_draw_piece(gfx, px, py, pipes.cell_px, pipes.queue[i], false);
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx,
                      4,
                      (int)solar_os_gfx_height(gfx) - 3,
                      "WASD aim   Space lay   Q quit");
    if (pipes.mode == PIPES_LEAK) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, pipes.origin_x + 8, pipes.origin_y + 28, "LEAK");
    }
    solar_os_gfx_present(gfx);
}

static void pipes_remember(void)
{
    if (pipes.score > pipes.best) {
        pipes.best = pipes.score;
    }
}

static void pipes_place(void)
{
    const uint8_t piece = pipes.queue[0];
    if (pipes.mode != PIPES_PLAY) {
        return;
    }
    if (pipes.cell[pipes.cursor_y][pipes.cursor_x] != 0) {
        return;
    }
    pipes.cell[pipes.cursor_y][pipes.cursor_x] = piece;
    for (int i = 0; i < PIPE_QUEUE - 1; i++) {
        pipes.queue[i] = pipes.queue[i + 1];
    }
    pipes.queue[PIPE_QUEUE - 1] = pipes_roll();
}

static void pipes_flow(void)
{
    const uint8_t kind = pipes.cell[pipes.head_y][pipes.head_x];
    const uint8_t mask = kind < 7 ? pipes_mask[kind] : 0;
    const uint8_t exit_bit = pipes_other(mask, pipes.entry);
    int dx = 0;
    int dy = 0;
    int nx;
    int ny;
    uint8_t need;
    uint8_t next_mask;
    if (pipes.mode != PIPES_PLAY) {
        return;
    }
    if ((mask & pipes.entry) == 0) {
        pipes.mode = PIPES_LEAK;
        pipes_remember();
        return;
    }
    pipes_step_delta(exit_bit, &dx, &dy);
    nx = pipes.head_x + dx;
    ny = pipes.head_y + dy;
    if (nx < 0 || ny < 0 || nx >= PIPE_W || ny >= PIPE_H) {
        pipes.mode = PIPES_LEAK;
        pipes_remember();
        return;
    }
    need = pipes_opposite(exit_bit);
    next_mask = pipes.cell[ny][nx] < 7 ? pipes_mask[pipes.cell[ny][nx]] : 0;
    if (pipes.cell[ny][nx] == 0 || (next_mask & need) == 0 || pipes.wet[ny][nx]) {
        pipes.mode = PIPES_LEAK;
        pipes_remember();
        return;
    }
    pipes.head_x = nx;
    pipes.head_y = ny;
    pipes.entry = need;
    pipes.wet[ny][nx] = 1;
    pipes.score++;
    if (pipes.score >= (PIPE_W * PIPE_H)) {
        pipes.mode = PIPES_DONE;
        pipes_remember();
    } else if (pipes.flow_ms > 650U) {
        pipes.flow_ms -= 30U;
    }
}

static esp_err_t pipes_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    pipes_reset();
    pipes_fit(gfx);
    pipes_render(ctx);
    return ESP_OK;
}

static void pipes_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool pipes_handle(solar_os_context_t *ctx, uint8_t ch)
{
    const int dir = pipes_dir(ch);
    if (ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (pipes.mode != PIPES_PLAY) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == 'r' || ch == 'R') {
            pipes_reset();
            pipes_render(ctx);
        }
        return true;
    }
    if (dir == 0 && pipes.cursor_y > 0) {
        pipes.cursor_y--;
    } else if (dir == 1 && pipes.cursor_x + 1 < PIPE_W) {
        pipes.cursor_x++;
    } else if (dir == 2 && pipes.cursor_y + 1 < PIPE_H) {
        pipes.cursor_y++;
    } else if (dir == 3 && pipes.cursor_x > 0) {
        pipes.cursor_x--;
    } else if (ch == ' ' || ch == '\n' || ch == '\r') {
        pipes_place();
    } else {
        return true;
    }
    pipes_render(ctx);
    return true;
}

static bool pipes_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return pipes_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!pipes.clock) {
            pipes.last_tick = event->data.tick_ms;
            pipes.clock = true;
            return true;
        }
        if (pipes.mode == PIPES_PLAY &&
            event->data.tick_ms - pipes.last_tick >= pipes.flow_ms) {
            pipes.last_tick = event->data.tick_ms;
            pipes_flow();
            pipes_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        pipes_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_pipes_app = {
    .name = "pipes",
    .summary = "lay pipe ahead of the flow",
    .start = pipes_start,
    .stop = pipes_stop,
    .event = pipes_event,
    .state_slot = &pipes_state,
    .state_size = sizeof(pipes_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .tick_interval_ms = 40U,
    .tick_deadline_ms = 25U,
};
