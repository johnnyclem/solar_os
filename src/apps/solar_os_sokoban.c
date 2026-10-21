#include "solar_os_sokoban.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define SK_W 14
#define SK_H 8
#define SK_UNDO 48

typedef struct {
    uint8_t px;
    uint8_t py;
    uint8_t box[SK_H][SK_W];
} sk_snap_t;

typedef struct {
    char base[SK_H][SK_W];
    uint8_t box[SK_H][SK_W];
    uint8_t goal[SK_H][SK_W];
    sk_snap_t undo[SK_UNDO];
    int undo_n;
    int w;
    int h;
    int px;
    int py;
    int level;
    int moves;
    int cell;
    int origin_x;
    int origin_y;
    bool solved;
    bool won;
} sk_state_t;

static void *sk_state;
#define sk (*(sk_state_t *)sk_state)

static const char *const sk_levels[] = {
    "#######\n"
    "#@ $ .#\n"
    "#######\n",
    "########\n"
    "#      #\n"
    "#@$  . #\n"
    "#      #\n"
    "########\n",
    "########\n"
    "#  .   #\n"
    "#  $   #\n"
    "#@$ .  #\n"
    "#      #\n"
    "########\n",
    "#########\n"
    "# . . . #\n"
    "# $ $ $ #\n"
    "#@      #\n"
    "#########\n",
    "#########\n"
    "#   #   #\n"
    "# $   $ #\n"
    "#@ . .  #\n"
    "#########\n",
    "##########\n"
    "#   . .  #\n"
    "# $    $ #\n"
    "#   ##   #\n"
    "#@       #\n"
    "##########\n",
    "#########\n"
    "#@  #   #\n"
    "# $   $ #\n"
    "# #. .# #\n"
    "#       #\n"
    "#########\n",
    "##########\n"
    "#  . .   #\n"
    "#  $$    #\n"
    "# ##     #\n"
    "#@       #\n"
    "#        #\n"
    "##########\n",
};

#define SK_LEVELS ((int)(sizeof(sk_levels) / sizeof(sk_levels[0])))

static int sk_dir(uint8_t ch)
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

static bool sk_quit(uint8_t ch)
{
    return ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ESCAPE ||
           ch == SOLAR_OS_KEY_APP_EXIT;
}

static bool sk_inside(int x, int y)
{
    return x >= 0 && y >= 0 && x < sk.w && y < sk.h;
}

static bool sk_load(int level)
{
    if (level < 0 || level >= SK_LEVELS) {
        return false;
    }
    memset(sk.base, 0, sizeof(sk.base));
    memset(sk.box, 0, sizeof(sk.box));
    memset(sk.goal, 0, sizeof(sk.goal));
    sk.undo_n = 0;
    sk.moves = 0;
    sk.solved = false;
    sk.won = false;
    sk.level = level;
    sk.px = 1;
    sk.py = 1;

    int x = 0;
    int y = 0;
    int width = 0;
    for (const char *s = sk_levels[level]; *s != '\0'; s++) {
        if (*s == '\n') {
            if (width == 0) {
                width = x;
            }
            y++;
            x = 0;
            if (y >= SK_H) {
                break;
            }
            continue;
        }
        if (y >= SK_H || x >= SK_W) {
            x++;
            continue;
        }
        const char ch = *s;
        sk.base[y][x] = (ch == '#') ? '#' : ' ';
        if (ch == '.' || ch == '*' || ch == '+') {
            sk.goal[y][x] = 1;
        }
        if (ch == '$' || ch == '*') {
            sk.box[y][x] = 1;
        }
        if (ch == '@' || ch == '+') {
            sk.px = x;
            sk.py = y;
        }
        x++;
    }
    sk.w = width > 0 ? width : 1;
    sk.h = y > 0 ? y : 1;
    return true;
}

static bool sk_all_goals(void)
{
    for (int y = 0; y < sk.h; y++) {
        for (int x = 0; x < sk.w; x++) {
            if (sk.goal[y][x] && !sk.box[y][x]) {
                return false;
            }
        }
    }
    return true;
}

static void sk_push_undo(void)
{
    if (sk.undo_n == SK_UNDO) {
        memmove(&sk.undo[0], &sk.undo[1], sizeof(sk.undo) - sizeof(sk.undo[0]));
        sk.undo_n--;
    }
    sk_snap_t *snap = &sk.undo[sk.undo_n++];
    snap->px = (uint8_t)sk.px;
    snap->py = (uint8_t)sk.py;
    memcpy(snap->box, sk.box, sizeof(sk.box));
}

static void sk_undo(void)
{
    if (sk.undo_n <= 0 || sk.solved) {
        return;
    }
    const sk_snap_t *snap = &sk.undo[--sk.undo_n];
    sk.px = snap->px;
    sk.py = snap->py;
    memcpy(sk.box, snap->box, sizeof(sk.box));
    if (sk.moves > 0) {
        sk.moves--;
    }
}

static bool sk_move(int dir)
{
    if (sk.solved) {
        return false;
    }
    int dx = 0;
    int dy = 0;
    if (dir == 0) {
        dy = -1;
    } else if (dir == 1) {
        dx = 1;
    } else if (dir == 2) {
        dy = 1;
    } else {
        dx = -1;
    }
    const int nx = sk.px + dx;
    const int ny = sk.py + dy;
    if (!sk_inside(nx, ny) || sk.base[ny][nx] == '#') {
        return false;
    }
    if (sk.box[ny][nx]) {
        const int bx = nx + dx;
        const int by = ny + dy;
        if (!sk_inside(bx, by) || sk.base[by][bx] == '#' || sk.box[by][bx]) {
            return false;
        }
        sk_push_undo();
        sk.box[by][bx] = 1;
        sk.box[ny][nx] = 0;
    } else {
        sk_push_undo();
    }
    sk.px = nx;
    sk.py = ny;
    sk.moves++;
    if (sk_all_goals()) {
        sk.solved = true;
        if (sk.level + 1 >= SK_LEVELS) {
            sk.won = true;
        }
    }
    return true;
}

static void sk_fit(const solar_os_gfx_t *gfx)
{
    const int avail_w = (int)solar_os_gfx_width(gfx) - 8;
    const int avail_h = (int)solar_os_gfx_height(gfx) - 32;
    int cell = 8;
    if (sk.w > 0 && sk.h > 0) {
        const int cw = avail_w / sk.w;
        const int ch = avail_h / sk.h;
        cell = cw < ch ? cw : ch;
    }
    if (cell < 6) {
        cell = 6;
    }
    if (cell > 28) {
        cell = 28;
    }
    sk.cell = cell;
    sk.origin_x = ((int)solar_os_gfx_width(gfx) - (sk.w * cell)) / 2;
    sk.origin_y = 20 + ((avail_h - (sk.h * cell)) / 2);
}

static void sk_banner(solar_os_gfx_t *gfx, const char *text)
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

static void sk_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }
    char hud[64];
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    snprintf(hud,
             sizeof(hud),
             "sokoban  %d/%d  moves %d",
             sk.level + 1,
             SK_LEVELS,
             sk.moves);
    solar_os_gfx_text(gfx, 4, 12, hud);

    for (int y = 0; y < sk.h; y++) {
        for (int x = 0; x < sk.w; x++) {
            const int px = sk.origin_x + (x * sk.cell);
            const int py = sk.origin_y + (y * sk.cell);
            const int inset = sk.cell > 8 ? 1 : 0;
            if (sk.base[y][x] == '#') {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_rect(gfx, px, py, sk.cell, sk.cell);
                continue;
            }
            if (sk.goal[y][x]) {
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
                solar_os_gfx_fill_rect(gfx,
                                       px + inset,
                                       py + inset,
                                       sk.cell - (inset * 2),
                                       sk.cell - (inset * 2));
            }
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
            solar_os_gfx_rect(gfx, px, py, sk.cell, sk.cell);
            if (sk.box[y][x]) {
                const int pad = sk.cell / 5;
                solar_os_gfx_set_color(gfx,
                                       sk.goal[y][x] ? SOLAR_OS_GFX_COLOR_BLACK
                                                     : SOLAR_OS_GFX_COLOR_DARK);
                solar_os_gfx_fill_rect(gfx,
                                       px + pad,
                                       py + pad,
                                       sk.cell - (pad * 2),
                                       sk.cell - (pad * 2));
            }
            if (x == sk.px && y == sk.py) {
                const int pad = sk.cell / 4;
                solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
                solar_os_gfx_fill_circle(gfx,
                                         px + (sk.cell / 2),
                                         py + (sk.cell / 2),
                                         (sk.cell / 2) - pad);
            }
        }
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx,
                      4,
                      (int)solar_os_gfx_height(gfx) - 3,
                      "WASD/HJKL push   U undo   R retry   Q quit");
    if (sk.won) {
        sk_banner(gfx, "ALL STORED");
    } else if (sk.solved) {
        sk_banner(gfx, "STORED");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t sk_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    sk_load(0);
    sk_fit(gfx);
    sk_render(ctx);
    return ESP_OK;
}

static void sk_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool sk_handle(solar_os_context_t *ctx, uint8_t ch)
{
    if (sk_quit(ch)) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (sk.won) {
        if (ch == 'r' || ch == 'R' || ch == ' ' || ch == '\n' || ch == '\r') {
            sk_load(0);
            sk_render(ctx);
        }
        return true;
    }
    if (sk.solved) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == 'n' || ch == 'N') {
            sk_load(sk.level + 1);
            sk_render(ctx);
        } else if (ch == 'r' || ch == 'R') {
            sk_load(sk.level);
            sk_render(ctx);
        }
        return true;
    }
    if (ch == 'u' || ch == 'U' || ch == '\b') {
        sk_undo();
        sk_render(ctx);
        return true;
    }
    if (ch == 'r' || ch == 'R') {
        sk_load(sk.level);
        sk_render(ctx);
        return true;
    }
    const int dir = sk_dir(ch);
    if (dir >= 0 && sk_move(dir)) {
        sk_render(ctx);
    }
    return true;
}

static bool sk_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return sk_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        sk_render(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_sokoban_app = {
    .name = "sokoban",
    .summary = "push the crates onto the marks",
    .start = sk_start,
    .stop = sk_stop,
    .event = sk_event,
    .state_slot = &sk_state,
    .state_size = sizeof(sk_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 250U,
    .tick_deadline_ms = 25U,
};
