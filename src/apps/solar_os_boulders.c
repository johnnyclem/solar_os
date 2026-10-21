#include "solar_os_boulders.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define BD_W 16
#define BD_H 8

typedef enum {
    BD_PLAY,
    BD_CLEAR,
    BD_DEAD,
    BD_WON,
} bd_mode_t;

typedef struct {
    char board[BD_H][BD_W];
    int w;
    int h;
    int px;
    int py;
    int need;
    int got;
    int cave;
    int cell;
    int origin_x;
    int origin_y;
    bd_mode_t mode;
    bool busy;
    uint32_t last_tick;
    bool clock;
} bd_state_t;

static void *bd_state;
#define bd (*(bd_state_t *)bd_state)

static const char *const bd_caves[] = {
    "##############\n"
    "#@           #\n"
    "#  *  *  *  X#\n"
    "#  o  o  o   #\n"
    "#            #\n"
    "##############\n",
    "############\n"
    "#@........X#\n"
    "#..***.....#\n"
    "#..........#\n"
    "#..........#\n"
    "############\n",
    "############\n"
    "#@       .X#\n"
    "# *****    #\n"
    "#          #\n"
    "#   ooo    #\n"
    "############\n",
    "##############\n"
    "#@           #\n"
    "# **** **    #\n"
    "#            #\n"
    "#  oo  oo   X#\n"
    "#............#\n"
    "##############\n",
    "##############\n"
    "#@     #     #\n"
    "# ***  #  *  #\n"
    "# ooo  #  .  #\n"
    "#      #  . X#\n"
    "#............#\n"
    "##############\n",
};

#define BD_CAVES ((int)(sizeof(bd_caves) / sizeof(bd_caves[0])))

static int bd_dir(uint8_t ch)
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

static bool bd_physics(void)
{
    bool changed = false;
    for (int y = bd.h - 2; y >= 0; y--) {
        for (int x = 0; x < bd.w; x++) {
            const char ch = bd.board[y][x];
            int tx = -1;
            int ty = -1;
            char below;
            if (ch != 'o' && ch != '*') {
                continue;
            }
            below = bd.board[y + 1][x];
            if (below == ' ' || below == '@') {
                tx = x;
                ty = y + 1;
            } else if (below == 'o' || below == '*' || below == '#' || below == 'X') {
                if (x - 1 >= 0 && bd.board[y][x - 1] == ' ' &&
                    bd.board[y + 1][x - 1] == ' ') {
                    tx = x - 1;
                    ty = y;
                } else if (x + 1 < bd.w && bd.board[y][x + 1] == ' ' &&
                           bd.board[y + 1][x + 1] == ' ') {
                    tx = x + 1;
                    ty = y;
                }
            }
            if (tx < 0) {
                continue;
            }
            if (bd.board[ty][tx] == '@') {
                return true;
            }
            bd.board[y][x] = ' ';
            bd.board[ty][tx] = ch;
            changed = true;
        }
    }
    bd.busy = changed;
    return false;
}

static void bd_settle_now(void)
{
    for (int i = 0; i < 80; i++) {
        if (bd_physics()) {
            bd.mode = BD_DEAD;
            bd.busy = false;
            return;
        }
        if (!bd.busy) {
            return;
        }
    }
    bd.busy = false;
}

static void bd_find_player(void)
{
    bd.px = 1;
    bd.py = 1;
    for (int y = 0; y < bd.h; y++) {
        for (int x = 0; x < bd.w; x++) {
            if (bd.board[y][x] == '@') {
                bd.px = x;
                bd.py = y;
                return;
            }
        }
    }
}

static void bd_count_gems(void)
{
    bd.need = 0;
    bd.got = 0;
    for (int y = 0; y < bd.h; y++) {
        for (int x = 0; x < bd.w; x++) {
            if (bd.board[y][x] == '*') {
                bd.need++;
            }
        }
    }
}

static void bd_load(int cave)
{
    const char *s;
    int x = 0;
    int y = 0;
    int width = 0;
    if (cave < 0) {
        cave = 0;
    }
    if (cave >= BD_CAVES) {
        cave = 0;
    }
    memset(bd.board, 0, sizeof(bd.board));
    s = bd_caves[cave];
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            if (width == 0) {
                width = x;
            }
            y++;
            x = 0;
            if (y >= BD_H) {
                break;
            }
            continue;
        }
        if (y < BD_H && x < BD_W) {
            bd.board[y][x] = *s;
        }
        x++;
    }
    bd.w = width > 0 ? width : 1;
    bd.h = y > 0 ? y : 1;
    bd.cave = cave;
    bd.mode = BD_PLAY;
    bd.busy = false;
    bd.clock = false;
    bd_settle_now();
    bd_find_player();
    bd_count_gems();
}

static void bd_fit(const solar_os_gfx_t *gfx)
{
    const int avail_w = (int)solar_os_gfx_width(gfx) - 8;
    const int avail_h = (int)solar_os_gfx_height(gfx) - 32;
    int cell = bd.w > 0 ? avail_w / bd.w : 8;
    const int ch = bd.h > 0 ? avail_h / bd.h : 8;
    if (ch < cell) {
        cell = ch;
    }
    if (cell < 8) {
        cell = 8;
    }
    if (cell > 28) {
        cell = 28;
    }
    bd.cell = cell;
    bd.origin_x = ((int)solar_os_gfx_width(gfx) - (bd.w * cell)) / 2;
    bd.origin_y = 20;
}

static void bd_banner(solar_os_gfx_t *gfx, const char *text)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    const int text_w = (int)solar_os_gfx_text_width(gfx, text);
    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int baseline = (int)solar_os_gfx_height(gfx) / 2;
    const int x = (screen_w - text_w) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x - 8, baseline - 18, text_w + 16, 28);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x - 8, baseline - 18, text_w + 16, 28);
    solar_os_gfx_text(gfx, x, baseline, text);
}

static void bd_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    char hud[64];
    if (gfx == NULL) {
        return;
    }
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    snprintf(hud,
             sizeof(hud),
             "boulders  %d/%d  gems %d/%d",
             bd.cave + 1,
             BD_CAVES,
             bd.got,
             bd.need);
    solar_os_gfx_text(gfx, 4, 12, hud);

    for (int y = 0; y < bd.h; y++) {
        for (int x = 0; x < bd.w; x++) {
            const int px = bd.origin_x + (x * bd.cell);
            const int py = bd.origin_y + (y * bd.cell);
            const char ch = bd.board[y][x];
            const int pad = bd.cell / 5;
            if (ch == '#') {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, px, py, bd.cell, bd.cell);
                continue;
            }
            if (ch == '.') {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
                solar_os_gfx_fill_rect(gfx, px, py, bd.cell, bd.cell);
            }
            if (ch == 'X') {
                solar_os_gfx_set_color(gfx,
                                       bd.got >= bd.need ? SOLAR_OS_GFX_COLOR_DARK
                                                         : SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_rect(gfx, px + 2, py + 2, bd.cell - 4, bd.cell - 4);
            }
            if (ch == 'o') {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
                solar_os_gfx_fill_circle(gfx, px + (bd.cell / 2), py + (bd.cell / 2), (bd.cell / 2) - 1);
            }
            if (ch == '*') {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx,
                                       px + pad,
                                       py + pad,
                                       bd.cell - (pad * 2),
                                       bd.cell - (pad * 2));
            }
            if (ch == '@') {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_circle(gfx,
                                         px + (bd.cell / 2),
                                         py + (bd.cell / 2),
                                         bd.cell / 4);
            }
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx,
                      4,
                      (int)solar_os_gfx_height(gfx) - 3,
                      "WASD dig   wait for rocks   Q quit");
    if (bd.mode == BD_DEAD) {
        bd_banner(gfx, "CRUSHED");
    } else if (bd.mode == BD_CLEAR) {
        bd_banner(gfx, "CAVE CLEAR");
    } else if (bd.mode == BD_WON) {
        bd_banner(gfx, "ALL CAVES");
    }
    solar_os_gfx_present(gfx);
}

static bool bd_try_move(int dir)
{
    int dx = 0;
    int dy = 0;
    int nx;
    int ny;
    char dest;
    if (bd.mode != BD_PLAY || bd.busy) {
        return false;
    }
    if (dir == 0) {
        dy = -1;
    } else if (dir == 1) {
        dx = 1;
    } else if (dir == 2) {
        dy = 1;
    } else {
        dx = -1;
    }
    nx = bd.px + dx;
    ny = bd.py + dy;
    if (nx < 0 || ny < 0 || nx >= bd.w || ny >= bd.h) {
        return false;
    }
    dest = bd.board[ny][nx];
    if (dest == '#' || dest == 'o') {
        return false;
    }
    if (dest == 'X' && bd.got < bd.need) {
        return false;
    }
    if (dest == 'X') {
        bd.board[bd.py][bd.px] = ' ';
        if (bd.cave + 1 >= BD_CAVES) {
            bd.mode = BD_WON;
        } else {
            bd.mode = BD_CLEAR;
        }
        return true;
    }
    bd.board[bd.py][bd.px] = ' ';
    if (dest == '*') {
        bd.got++;
    }
    bd.board[ny][nx] = '@';
    bd.px = nx;
    bd.py = ny;
    bd.busy = true;
    return true;
}

static esp_err_t bd_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    bd_load(0);
    bd_fit(gfx);
    bd_render(ctx);
    return ESP_OK;
}

static void bd_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool bd_handle(solar_os_context_t *ctx, uint8_t ch)
{
    const int dir = bd_dir(ch);
    if (ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (bd.mode == BD_WON || bd.mode == BD_DEAD) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == 'r' || ch == 'R') {
            bd_load(0);
            bd_render(ctx);
        }
        return true;
    }
    if (bd.mode == BD_CLEAR) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == 'n' || ch == 'N') {
            bd_load(bd.cave + 1);
            bd_fit(solar_os_context_gfx(ctx));
            bd_render(ctx);
        }
        return true;
    }
    if (ch == 'r' || ch == 'R') {
        bd_load(bd.cave);
        bd_render(ctx);
        return true;
    }
    if (dir >= 0 && bd_try_move(dir)) {
        bd_render(ctx);
    }
    return true;
}

static bool bd_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return bd_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!bd.clock) {
            bd.last_tick = event->data.tick_ms;
            bd.clock = true;
            return true;
        }
        if (bd.busy && bd.mode == BD_PLAY &&
            event->data.tick_ms - bd.last_tick >= 140U) {
            bd.last_tick = event->data.tick_ms;
            if (bd_physics()) {
                bd.mode = BD_DEAD;
                bd.busy = false;
            }
            bd_find_player();
            bd_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        bd_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_boulders_app = {
    .name = "boulders",
    .summary = "dig, dodge rocks, take the gems",
    .start = bd_start,
    .stop = bd_stop,
    .event = bd_event,
    .state_slot = &bd_state,
    .state_size = sizeof(bd_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .tick_interval_ms = 40U,
    .tick_deadline_ms = 25U,
};
