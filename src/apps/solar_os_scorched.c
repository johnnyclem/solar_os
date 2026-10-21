#include "solar_os_scorched.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define SC_W 360
#define SC_GRAVITY 10
#define SC_HIT 8

typedef enum {
    SC_AIM,
    SC_FLY,
    SC_WAIT,
    SC_OVER,
} sc_phase_t;

typedef struct {
    uint8_t ground[SC_W];
    int angle;
    int power;
    int wind;
    int left_x;
    int right_x;
    int left_score;
    int right_score;
    int shot_x;
    int shot_y;
    int shot_vx;
    int shot_vy;
    int shot_step;
    int shooter;
    uint32_t rng;
    uint32_t last_tick;
    bool clock;
    bool cpu_turn;
    sc_phase_t phase;
} sc_state_t;

static void *sc_state;
#define sc (*(sc_state_t *)sc_state)
static uint32_t sc_boot = 0x5c0411edu;

static const int16_t sc_sin90[91] = {
    0, 4, 9, 13, 18, 22, 27, 31, 36, 40, 44, 49, 53, 58, 62, 66, 71, 75, 79, 83,
    88, 92, 96, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 139, 143, 147,
    150, 154, 158, 161, 165, 168, 171, 175, 178, 181, 184, 187, 190, 193, 196,
    199, 202, 204, 207, 210, 212, 215, 217, 219, 222, 224, 226, 228, 230, 232,
    234, 236, 237, 239, 241, 242, 243, 245, 246, 247, 248, 249, 250, 251, 252,
    253, 254, 254, 255, 255, 255, 256, 256, 256, 256,
};

static uint32_t sc_random(void)
{
    sc.rng = (sc.rng * 1664525U) + 1013904223U;
    return sc.rng;
}

static int sc_sin(int deg)
{
    int sign = 1;
    while (deg < 0) {
        deg += 360;
    }
    deg %= 360;
    if (deg > 180) {
        deg = 360 - deg;
        sign = -1;
    }
    if (deg > 90) {
        deg = 180 - deg;
    }
    return sign * (int)sc_sin90[deg];
}

static int sc_cos(int deg)
{
    return sc_sin(deg + 90);
}

static int sc_ground_at(int x)
{
    if (x < 0) {
        x = 0;
    }
    if (x >= SC_W) {
        x = SC_W - 1;
    }
    return sc.ground[x];
}

static void sc_flatten(int x)
{
    const int h = sc_ground_at(x);
    for (int i = -3; i <= 3; i++) {
        const int gx = x + i;
        if (gx >= 0 && gx < SC_W) {
            sc.ground[gx] = (uint8_t)h;
        }
    }
}

static void sc_make_land(void)
{
    int h = 48 + (int)(sc_random() % 20U);
    for (int i = 0; i < SC_W; i++) {
        h += (int)(sc_random() % 7U) - 3;
        if (h < 28) {
            h = 28;
        }
        if (h > 110) {
            h = 110;
        }
        sc.ground[i] = (uint8_t)h;
    }
    for (int pass = 0; pass < 2; pass++) {
        uint8_t next[SC_W];
        for (int i = 0; i < SC_W; i++) {
            const int left = sc.ground[i > 0 ? i - 1 : i];
            const int right = sc.ground[i + 1 < SC_W ? i + 1 : i];
            next[i] = (uint8_t)((left + sc.ground[i] + right) / 3);
        }
        memcpy(sc.ground, next, sizeof(next));
    }
    sc.left_x = 28;
    sc.right_x = SC_W - 36;
    sc_flatten(sc.left_x);
    sc_flatten(sc.right_x);
    sc.wind = (int)(sc_random() % 5U) - 2;
    sc.angle = 52;
    sc.power = 50;
    sc.phase = SC_AIM;
    sc.cpu_turn = false;
}

static void sc_crater(int x)
{
    for (int dx = -10; dx <= 10; dx++) {
        const int gx = x + dx;
        int drop = 10 - (dx < 0 ? -dx : dx);
        int h;
        if (gx < 0 || gx >= SC_W) {
            continue;
        }
        h = (int)sc.ground[gx] - drop;
        if (h < 16) {
            h = 16;
        }
        sc.ground[gx] = (uint8_t)h;
    }
    sc_flatten(sc.left_x);
    sc_flatten(sc.right_x);
}

static int sc_base_y(const solar_os_gfx_t *gfx)
{
    return (int)solar_os_gfx_height(gfx) - 18;
}

static bool sc_hits_tank(int x, int y, int tank_x)
{
    const int adx = x - tank_x;
    const int tank_top = sc_ground_at(tank_x) + 12;
    const int dist = adx < 0 ? -adx : adx;
    return dist <= SC_HIT && y <= tank_top && y >= sc_ground_at(tank_x) - 2;
}

static void sc_launch(int angle, int power, int from_x, int shooter)
{
    sc.shot_x = (from_x + (shooter == 0 ? 8 : -8)) * 256;
    sc.shot_y = (sc_ground_at(from_x) + 14) * 256;
    sc.shot_vx = (sc_cos(angle) * power) / 16 + (sc.wind * 36);
    sc.shot_vy = (sc_sin(angle) * power) / 16;
    sc.shot_step = 0;
    sc.shooter = shooter;
    sc.phase = SC_FLY;
}

static int sc_impact_x(int angle, int power, int from_x, int shooter, bool *hit_player)
{
    int x = (from_x + (shooter == 0 ? 8 : -8)) * 256;
    int y = (sc_ground_at(from_x) + 14) * 256;
    int vx = (sc_cos(angle) * power) / 16 + (sc.wind * 36);
    int vy = (sc_sin(angle) * power) / 16;
    const int enemy = shooter == 0 ? sc.right_x : sc.left_x;
    *hit_player = false;
    for (int step = 0; step < 700; step++) {
        int px;
        int py;
        x += vx;
        y += vy;
        vy -= SC_GRAVITY;
        px = x / 256;
        py = y / 256;
        if (px < 0 || px >= SC_W || py > 240) {
            return px < 0 ? 0 : (px >= SC_W ? SC_W - 1 : px);
        }
        if (step > 6 && sc_hits_tank(px, py, enemy)) {
            *hit_player = true;
            return enemy;
        }
        if (py <= sc_ground_at(px)) {
            return px;
        }
    }
    return from_x;
}

static void sc_cpu_aim(void)
{
    int best_angle = 128;
    int best_power = 50;
    int best_dist = 10000;
    bool best_hit = false;
    for (int angle = 100; angle <= 155; angle += 5) {
        for (int power = 30; power <= 80; power += 8) {
            bool hit = false;
            const int impact = sc_impact_x(angle, power, sc.right_x, 1, &hit);
            const int dist = impact - sc.left_x;
            const int ad = dist < 0 ? -dist : dist;
            if (hit || ad < best_dist) {
                best_dist = hit ? 0 : ad;
                best_angle = angle;
                best_power = power;
                best_hit = hit;
                if (best_hit && ad == 0) {
                    break;
                }
            }
        }
    }
    best_angle += (int)(sc_random() % 9U) - 4;
    if (best_angle < 95) {
        best_angle = 95;
    }
    if (best_angle > 160) {
        best_angle = 160;
    }
    if (best_hit && best_dist == 0) {
        best_angle -= (int)(sc_random() % 3U);
    }
    sc.angle = best_angle;
    sc.power = best_power;
    sc_launch(best_angle, best_power, sc.right_x, 1);
}

static bool sc_step_shot(void)
{
    const int enemy = sc.shooter == 0 ? sc.right_x : sc.left_x;
    sc.shot_x += sc.shot_vx;
    sc.shot_y += sc.shot_vy;
    sc.shot_vy -= SC_GRAVITY;
    sc.shot_step++;
    const int px = sc.shot_x / 256;
    const int py = sc.shot_y / 256;
    if (px < 0 || px >= SC_W || py > 240 || sc.shot_step > 700) {
        sc.phase = SC_WAIT;
        return true;
    }
    if (sc.shot_step > 6 && sc_hits_tank(px, py, enemy)) {
        if (sc.shooter == 0) {
            sc.left_score++;
        } else {
            sc.right_score++;
        }
        sc_crater(enemy);
        sc.phase = SC_WAIT;
        return true;
    }
    if (py <= sc_ground_at(px)) {
        sc_crater(px);
        sc.phase = SC_WAIT;
        return true;
    }
    return false;
}

static void sc_next_round(void)
{
    if (sc.left_score >= 3 || sc.right_score >= 3) {
        sc.phase = SC_OVER;
        return;
    }
    sc_make_land();
}

static void sc_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    char hud[80];
    int base;
    if (gfx == NULL) {
        return;
    }
    base = sc_base_y(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    snprintf(hud,
             sizeof(hud),
             "you %d  cpu %d   angle %d  power %d  wind %s%d",
             sc.left_score,
             sc.right_score,
             sc.cpu_turn ? 180 - sc.angle : sc.angle,
             sc.power,
             sc.wind > 0 ? "+" : "",
             sc.wind);
    solar_os_gfx_text(gfx, 4, 12, hud);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    for (int x = 0; x < SC_W; x++) {
        const int gh = sc.ground[x];
        solar_os_gfx_fill_rect(gfx, 20 + x, base - gh, 1, gh);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_DARK);
    solar_os_gfx_fill_rect(gfx,
                           20 + sc.left_x - 6,
                           base - sc_ground_at(sc.left_x) - 10,
                           12,
                           8);
    solar_os_gfx_fill_rect(gfx,
                           20 + sc.right_x - 6,
                           base - sc_ground_at(sc.right_x) - 10,
                           12,
                           8);
    if (sc.phase == SC_FLY) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_circle(gfx,
                                 20 + (sc.shot_x / 256),
                                 base - (sc.shot_y / 256),
                                 2);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx,
                      4,
                      (int)solar_os_gfx_height(gfx) - 3,
                      "A/D aim   W/S power   Space fire   Q quit");
    if (sc.phase == SC_OVER) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx,
                          120,
                          base - 140,
                          sc.left_score > sc.right_score ? "YOU WIN" : "CPU WINS");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t sc_start(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    sc.rng = sc_boot;
    sc_boot += 0x9e3779b9U;
    sc.left_score = 0;
    sc.right_score = 0;
    sc.clock = false;
    sc_make_land();
    sc_render(ctx);
    return ESP_OK;
}

static void sc_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool sc_handle(solar_os_context_t *ctx, uint8_t ch)
{
    bool dirty = false;
    if (ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (sc.phase == SC_OVER) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == 'r' || ch == 'R') {
            sc.left_score = 0;
            sc.right_score = 0;
            sc_make_land();
            sc_render(ctx);
        }
        return true;
    }
    if (sc.phase != SC_AIM || sc.cpu_turn) {
        return true;
    }
    if (ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H' || ch == SOLAR_OS_KEY_LEFT) {
        if (sc.angle > 20) {
            sc.angle -= 2;
            dirty = true;
        }
    } else if (ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L' || ch == SOLAR_OS_KEY_RIGHT) {
        if (sc.angle < 88) {
            sc.angle += 2;
            dirty = true;
        }
    } else if (ch == 'w' || ch == 'W' || ch == 'k' || ch == 'K' || ch == SOLAR_OS_KEY_UP) {
        if (sc.power < 80) {
            sc.power += 2;
            dirty = true;
        }
    } else if (ch == 's' || ch == 'S' || ch == 'j' || ch == 'J' || ch == SOLAR_OS_KEY_DOWN) {
        if (sc.power > 20) {
            sc.power -= 2;
            dirty = true;
        }
    } else if (ch == ' ' || ch == '\n' || ch == '\r') {
        sc_launch(sc.angle, sc.power, sc.left_x, 0);
        dirty = true;
    }
    if (dirty) {
        sc_render(ctx);
    }
    return true;
}

static bool sc_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return sc_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (!sc.clock) {
            sc.last_tick = event->data.tick_ms;
            sc.clock = true;
            return true;
        }
        if (sc.phase == SC_FLY && event->data.tick_ms - sc.last_tick >= 16U) {
            bool landed = false;
            sc.last_tick = event->data.tick_ms;
            for (int i = 0; i < 2 && sc.phase == SC_FLY; i++) {
                landed = sc_step_shot();
            }
            if (landed && sc.phase == SC_WAIT) {
                if (sc.left_score >= 3 || sc.right_score >= 3) {
                    sc.phase = SC_OVER;
                    sc.cpu_turn = false;
                } else if (!sc.cpu_turn) {
                    sc.cpu_turn = true;
                    sc_cpu_aim();
                } else {
                    sc.cpu_turn = false;
                    sc_next_round();
                }
            }
            sc_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        sc_render(ctx);
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_scorched_app = {
    .name = "scorched",
    .summary = "two-tank artillery",
    .start = sc_start,
    .stop = sc_stop,
    .event = sc_event,
    .state_slot = &sc_state,
    .state_size = sizeof(sc_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .tick_interval_ms = 16U,
    .tick_deadline_ms = 20U,
};
