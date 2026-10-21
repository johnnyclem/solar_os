#include "solar_os_mines.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define MINE_W 16
#define MINE_H 12
#define MINE_COUNT 28

typedef struct {
    uint8_t bomb[MINE_H][MINE_W];
    uint8_t adj[MINE_H][MINE_W];
    uint8_t open[MINE_H][MINE_W];
    int cx;
    int cy;
    int cell;
    int origin_x;
    int origin_y;
    int left;
    uint32_t rng;
    uint32_t seconds;
    uint32_t last_tick;
    bool armed;
    bool dead;
    bool won;
    bool clock;
} mine_state_t;

static void *mine_state;
#define mine (*(mine_state_t *)mine_state)
static uint32_t mine_boot = 0x51a7c0deu;

static uint32_t mine_random(void)
{
    mine.rng = (mine.rng * 1664525U) + 1013904223U;
    return mine.rng;
}

static int mine_dir(uint8_t ch)
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

static void mine_fit(const solar_os_gfx_t *gfx)
{
    const int avail_w = (int)solar_os_gfx_width(gfx) - 8;
    const int avail_h = (int)solar_os_gfx_height(gfx) - 32;
    int cell = avail_w / MINE_W;
    const int ch = avail_h / MINE_H;
    if (ch < cell) {
        cell = ch;
    }
    if (cell < 8) {
        cell = 8;
    }
    if (cell > 24) {
        cell = 24;
    }
    mine.cell = cell;
    mine.origin_x = ((int)solar_os_gfx_width(gfx) - (MINE_W * cell)) / 2;
    mine.origin_y = 18;
}

static void mine_reset(void)
{
    memset(mine.bomb, 0, sizeof(mine.bomb));
    memset(mine.adj, 0, sizeof(mine.adj));
    memset(mine.open, 0, sizeof(mine.open));
    mine.cx = MINE_W / 2;
    mine.cy = MINE_H / 2;
    mine.left = (MINE_W * MINE_H) - MINE_COUNT;
    mine.seconds = 0;
    mine.armed = false;
    mine.dead = false;
    mine.won = false;
    mine.clock = false;
    mine.rng = mine_boot;
    mine_boot += 0x9e3779b9U;
}

static void mine_place(int safe_x, int safe_y)
{
    int placed = 0;
    int guard = 0;
    while (placed < MINE_COUNT && guard < 4000) {
        const int x = (int)(mine_random() % MINE_W);
        const int y = (int)(mine_random() % MINE_H);
        const int dx = x - safe_x;
        const int dy = y - safe_y;
        const int adx = dx < 0 ? -dx : dx;
        const int ady = dy < 0 ? -dy : dy;
        guard++;
        if (adx <= 1 && ady <= 1) {
            continue;
        }
        if (mine.bomb[y][x]) {
            continue;
        }
        mine.bomb[y][x] = 1;
        placed++;
    }
    mine.left = 0;
    for (int y = 0; y < MINE_H; y++) {
        for (int x = 0; x < MINE_W; x++) {
            if (!mine.bomb[y][x]) {
                mine.left++;
            }
        }
    }
    for (int y = 0; y < MINE_H; y++) {
        for (int x = 0; x < MINE_W; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= MINE_W || ny >= MINE_H) {
                        continue;
                    }
                    n += mine.bomb[ny][nx] ? 1 : 0;
                }
            }
            mine.adj[y][x] = (uint8_t)n;
        }
    }
    mine.armed = true;
}

static void mine_open_at(int x, int y)
{
    typedef struct {
        int x;
        int y;
    } mine_pt_t;
    mine_pt_t stack[MINE_W * MINE_H];
    int sp = 0;
    if (mine.open[y][x] == 2) {
        return;
    }
    stack[sp].x = x;
    stack[sp].y = y;
    sp++;
    while (sp > 0) {
        sp--;
        const int cx = stack[sp].x;
        const int cy = stack[sp].y;
        if (mine.open[cy][cx] == 1 || mine.open[cy][cx] == 2) {
            continue;
        }
        if (mine.bomb[cy][cx]) {
            continue;
        }
        mine.open[cy][cx] = 1;
        mine.left--;
        if (mine.adj[cy][cx] != 0) {
            continue;
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = cx + dx;
                const int ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= MINE_W || ny >= MINE_H) {
                    continue;
                }
                if (mine.open[ny][nx] == 0 && !mine.bomb[ny][nx] && sp < (MINE_W * MINE_H)) {
                    stack[sp].x = nx;
                    stack[sp].y = ny;
                    sp++;
                }
            }
        }
    }
    if (mine.left <= 0) {
        mine.won = true;
    }
}

static void mine_reveal(int x, int y)
{
    if (!mine.armed) {
        mine_place(x, y);
        mine.clock = true;
    }
    if (mine.open[y][x] == 2) {
        return;
    }
    if (mine.bomb[y][x]) {
        mine.dead = true;
        mine.open[y][x] = 1;
        return;
    }
    mine_open_at(x, y);
}

static void mine_banner(solar_os_gfx_t *gfx, const char *text)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const int text_w = (int)solar_os_gfx_text_width(gfx, text);
    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);
    const int bx = (screen_w - text_w) / 2;
    const int baseline = screen_h / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, bx - 8, baseline - 18, text_w + 16, 28);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, bx - 8, baseline - 18, text_w + 16, 28);
    solar_os_gfx_text(gfx, bx, baseline, text);
}

static void mine_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    char hud[64];
    char digit[2];
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    snprintf(hud, sizeof(hud), "mines  %lu s   left %d", (unsigned long)mine.seconds, mine.left);
    solar_os_gfx_text(gfx, 4, 12, hud);

    for (int y = 0; y < MINE_H; y++) {
        for (int x = 0; x < MINE_W; x++) {
            const int px = mine.origin_x + (x * mine.cell);
            const int py = mine.origin_y + (y * mine.cell);
            const bool show_mine = mine.bomb[y][x] && (mine.dead || mine.won);
            if (mine.open[y][x] == 1 || show_mine) {
                solar_os_gfx_set_color(gfx,
                                       show_mine && mine.dead ? SOLAR_OS_GFX_COLOR_BLACK
                                                              : SOLAR_OS_GFX_COLOR_WHITE);
                solar_os_gfx_fill_rect(gfx, px, py, mine.cell, mine.cell);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
                solar_os_gfx_rect(gfx, px, py, mine.cell, mine.cell);
                if (show_mine) {
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_fill_circle(gfx,
                                             px + (mine.cell / 2),
                                             py + (mine.cell / 2),
                                             mine.cell / 5);
                } else if (mine.adj[y][x] > 0) {
                    digit[0] = (char)('0' + mine.adj[y][x]);
                    digit[1] = '\0';
                    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                    solar_os_gfx_text(gfx, px + 3, py + mine.cell - 3, digit);
                }
            } else {
                solar_os_gfx_set_color(gfx,
                                       mine.open[y][x] == 2 ? SOLAR_OS_GFX_COLOR_DARK
                                                            : SOLAR_OS_GFX_COLOR_LIGHT);
                solar_os_gfx_fill_rect(gfx, px, py, mine.cell, mine.cell);
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, px, py, mine.cell, mine.cell);
            }
            if (x == mine.cx && y == mine.cy) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, px + 1, py + 1, mine.cell - 2, mine.cell - 2);
            }
        }
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_text(gfx,
                      4,
                      (int)solar_os_gfx_height(gfx) - 3,
                      "WASD move   Space open   F flag   Q quit");
    if (mine.dead) {
        mine_banner(gfx, "BOOM");
    } else if (mine.won) {
        mine_banner(gfx, "CLEAR");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t mine_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    mine_reset();
    mine_fit(gfx);
    mine_render(ctx);
    return ESP_OK;
}

static void mine_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool mine_handle(solar_os_context_t *ctx, uint8_t ch)
{
    if (ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (mine.dead || mine.won) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == 'r' || ch == 'R') {
            mine_reset();
            mine_render(ctx);
        }
        return true;
    }
    const int dir = mine_dir(ch);
    if (dir >= 0) {
        if (dir == 0 && mine.cy > 0) {
            mine.cy--;
        } else if (dir == 1 && mine.cx + 1 < MINE_W) {
            mine.cx++;
        } else if (dir == 2 && mine.cy + 1 < MINE_H) {
            mine.cy++;
        } else if (dir == 3 && mine.cx > 0) {
            mine.cx--;
        }
        mine_render(ctx);
        return true;
    }
    if (ch == ' ' || ch == '\n' || ch == '\r') {
        mine_reveal(mine.cx, mine.cy);
        mine_render(ctx);
        return true;
    }
    if (ch == 'f' || ch == 'F') {
        if (!mine.open[mine.cy][mine.cx]) {
            mine.open[mine.cy][mine.cx] = 2;
        } else if (mine.open[mine.cy][mine.cx] == 2) {
            mine.open[mine.cy][mine.cx] = 0;
        }
        mine_render(ctx);
    }
    return true;
}

static bool mine_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return mine_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!mine.clock) {
            mine.last_tick = event->data.tick_ms;
            return true;
        }
        if (event->data.tick_ms - mine.last_tick >= 1000U && !mine.dead && !mine.won) {
            mine.last_tick = event->data.tick_ms;
            mine.seconds++;
            mine_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        mine_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_mines_app = {
    .name = "mines",
    .summary = "uncover the field",
    .start = mine_start,
    .stop = mine_stop,
    .event = mine_event,
    .state_slot = &mine_state,
    .state_size = sizeof(mine_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .tick_interval_ms = 200U,
    .tick_deadline_ms = 25U,
};
