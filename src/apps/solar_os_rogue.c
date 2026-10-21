#include "solar_os_rogue.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define RG_W 40
#define RG_H 22
#define RG_ROOMS 6
#define RG_MONS 14
#define RG_ITEMS 12
#define RG_DEPTHS 8

enum {
    RG_WALL = 0,
    RG_FLOOR,
    RG_DOWN,
    RG_UP,
};

enum {
    RG_PLAY,
    RG_DEAD,
    RG_WON,
};

enum {
    IT_GOLD = 1,
    IT_FOOD,
    IT_POTION,
    IT_WEAPON,
    IT_ARMOR,
    IT_AMULET,
};

typedef struct {
    char glyph;
    const char *name;
    uint8_t hp;
    uint8_t atk;
    uint8_t xp;
    uint8_t min_d;
    uint8_t max_d;
} rg_kind_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    bool on;
} rg_room_t;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t kind;
    uint8_t hp;
    bool alive;
} rg_mon_t;

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t kind;
    uint8_t amount;
    bool on;
} rg_item_t;

typedef struct {
    uint8_t kind[RG_H][RG_W];
    uint8_t seen[RG_H][RG_W];
    rg_mon_t mons[RG_MONS];
    rg_item_t items[RG_ITEMS];
    int px;
    int py;
    int hp;
    int max_hp;
    int level;
    int xp;
    int gold;
    int food;
    int potions;
    int weapon;
    int armor;
    int hunger;
    int depth;
    int mode;
    bool amulet;
    bool hungry_said;
    uint32_t rng;
    char msg[72];
} rg_state_t;

static void *rg_state;
#define rg (*(rg_state_t *)rg_state)
static uint32_t rg_boot = 0x4060e001u;

static const rg_kind_t rg_kinds[] = {
    {'k', "kobold", 4, 2, 3, 1, 4},
    {'s', "snake", 5, 2, 4, 1, 6},
    {'o', "orc", 8, 3, 7, 2, 8},
    {'Z', "zombie", 11, 3, 9, 3, 8},
    {'T', "troll", 18, 5, 16, 5, 8},
    {'D', "dragon", 28, 7, 40, 8, 8},
};

#define RG_KINDS ((int)(sizeof(rg_kinds) / sizeof(rg_kinds[0])))

static uint32_t rg_random(void)
{
    rg.rng = (rg.rng * 1664525U) + 1013904223U;
    return rg.rng;
}

static void rg_say(const char *text)
{
    snprintf(rg.msg, sizeof(rg.msg), "%s", text);
}

static int rg_abs(int v)
{
    return v < 0 ? -v : v;
}

static int rg_cheb(int x, int y)
{
    const int dx = rg_abs(x - rg.px);
    const int dy = rg_abs(y - rg.py);
    return dx > dy ? dx : dy;
}

static bool rg_los(int x1, int y1)
{
    int x0 = rg.px;
    int y0 = rg.py;
    const int dx = rg_abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -rg_abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    if (x0 == x1 && y0 == y1) {
        return true;
    }
    while (x0 != x1 || y0 != y1) {
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
        if (x0 == x1 && y0 == y1) {
            return true;
        }
        if (x0 < 0 || y0 < 0 || x0 >= RG_W || y0 >= RG_H ||
            rg.kind[y0][x0] == RG_WALL) {
            return false;
        }
    }
    return true;
}

static bool rg_visible(int x, int y)
{
    if (rg_cheb(x, y) > 7) {
        return false;
    }
    return rg_los(x, y);
}

static int rg_mon_at(int x, int y)
{
    for (int i = 0; i < RG_MONS; i++) {
        if (rg.mons[i].alive && rg.mons[i].x == x && rg.mons[i].y == y) {
            return i;
        }
    }
    return -1;
}

static int rg_item_at(int x, int y)
{
    for (int i = 0; i < RG_ITEMS; i++) {
        if (rg.items[i].on && rg.items[i].x == x && rg.items[i].y == y) {
            return i;
        }
    }
    return -1;
}

static bool rg_floor_like(int x, int y)
{
    const uint8_t kind = rg.kind[y][x];
    return kind == RG_FLOOR || kind == RG_DOWN || kind == RG_UP;
}

static void rg_gain_xp(int amount)
{
    rg.xp += amount;
    while (rg.xp >= rg.level * 18) {
        rg.xp -= rg.level * 18;
        rg.level++;
        rg.max_hp += 4;
        rg.hp = rg.max_hp;
        rg_say("you feel stronger");
    }
}

static void rg_hurt(int amount)
{
    if (amount < 1) {
        amount = 1;
    }
    rg.hp -= amount;
    if (rg.hp <= 0) {
        rg.hp = 0;
        rg.mode = RG_DEAD;
        rg_say("you die");
    }
}

static void rg_attack(int index)
{
    rg_mon_t *mon = &rg.mons[index];
    const rg_kind_t *kind = &rg_kinds[mon->kind];
    const int dmg = 1 + rg.weapon + (int)(rg_random() % (uint32_t)(rg.level + 2));
    char line[72];
    mon->hp = (uint8_t)(mon->hp > dmg ? mon->hp - dmg : 0);
    if (mon->hp == 0) {
        mon->alive = false;
        rg.gold += 1 + (int)(rg_random() % 6U);
        rg_gain_xp(kind->xp);
        snprintf(line, sizeof(line), "you slay the %s", kind->name);
        if (rg.mode == RG_PLAY) {
            rg_say(line);
        }
        return;
    }
    snprintf(line, sizeof(line), "you hit the %s", kind->name);
    rg_say(line);
}

static void rg_monsters_act(void)
{
    if (rg.mode != RG_PLAY) {
        return;
    }
    for (int i = 0; i < RG_MONS; i++) {
        int best = 99;
        int bx = 0;
        int by = 0;
        bool step = false;
        rg_mon_t *mon;
        if (!rg.mons[i].alive) {
            continue;
        }
        mon = &rg.mons[i];
        if (rg_cheb(mon->x, mon->y) > 8) {
            continue;
        }
        if (rg_cheb(mon->x, mon->y) <= 1) {
            const int back = (int)rg_kinds[mon->kind].atk - (rg.armor / 2);
            char line[72];
            snprintf(line, sizeof(line), "the %s hits", rg_kinds[mon->kind].name);
            rg_say(line);
            rg_hurt(back);
            if (rg.mode != RG_PLAY) {
                return;
            }
            continue;
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = (int)mon->x + dx;
                const int ny = (int)mon->y + dy;
                int dist;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= RG_W || ny >= RG_H) {
                    continue;
                }
                if (!rg_floor_like(nx, ny) || rg_mon_at(nx, ny) >= 0) {
                    continue;
                }
                if (nx == rg.px && ny == rg.py) {
                    continue;
                }
                dist = rg_abs(nx - rg.px) + rg_abs(ny - rg.py);
                if (dist < best) {
                    best = dist;
                    bx = nx;
                    by = ny;
                    step = true;
                }
            }
        }
        if (step) {
            mon->x = (uint8_t)bx;
            mon->y = (uint8_t)by;
        }
    }
}

static void rg_hunger_tick(void)
{
    if (rg.mode != RG_PLAY) {
        return;
    }
    rg.hunger++;
    if (rg.hunger == 45 && !rg.hungry_said) {
        rg.hungry_said = true;
        rg_say("you are hungry");
    }
    if (rg.hunger > 70) {
        rg_hurt(1);
        if (rg.mode == RG_PLAY) {
            rg_say("you are starving");
        }
    }
}

static void rg_take(int index)
{
    rg_item_t *item = &rg.items[index];
    char line[72];
    if (item->kind == IT_GOLD) {
        rg.gold += item->amount;
        snprintf(line, sizeof(line), "gold +%u", (unsigned)item->amount);
        rg_say(line);
        item->on = false;
        return;
    }
    if (item->kind == IT_FOOD) {
        if (rg.food >= 9) {
            rg_say("no room for food");
            return;
        }
        rg.food++;
        item->on = false;
        rg_say("you pick up food");
        return;
    }
    if (item->kind == IT_POTION) {
        if (rg.potions >= 9) {
            rg_say("no room for a potion");
            return;
        }
        rg.potions++;
        item->on = false;
        rg_say("you pick up a potion");
        return;
    }
    if (item->kind == IT_WEAPON) {
        if (rg.weapon >= 3) {
            rg_say("you leave the spare blade");
            return;
        }
        rg.weapon++;
        item->on = false;
        rg_say("you take a sharper blade");
        return;
    }
    if (item->kind == IT_ARMOR) {
        if (rg.armor >= 3) {
            rg_say("you leave the spare mail");
            return;
        }
        rg.armor++;
        item->on = false;
        rg_say("you pull on mail");
        return;
    }
    if (item->kind == IT_AMULET) {
        rg.amulet = true;
        item->on = false;
        rg_say("the amulet is warm in your hand");
    }
}

static bool rg_place_item(int x, int y, int kind, int amount)
{
    if (rg_item_at(x, y) >= 0) {
        return false;
    }
    for (int i = 0; i < RG_ITEMS; i++) {
        if (rg.items[i].on) {
            continue;
        }
        rg.items[i].on = true;
        rg.items[i].x = (uint8_t)x;
        rg.items[i].y = (uint8_t)y;
        rg.items[i].kind = (uint8_t)kind;
        rg.items[i].amount = (uint8_t)amount;
        return true;
    }
    return false;
}

static bool rg_random_floor(int *x, int *y)
{
    for (int tries = 0; tries < 80; tries++) {
        const int cx = (int)(rg_random() % RG_W);
        const int cy = (int)(rg_random() % RG_H);
        if (!rg_floor_like(cx, cy) || (cx == rg.px && cy == rg.py)) {
            continue;
        }
        if (rg_mon_at(cx, cy) >= 0 || rg_item_at(cx, cy) >= 0) {
            continue;
        }
        *x = cx;
        *y = cy;
        return true;
    }
    return false;
}

static void rg_carve_room(const rg_room_t *room)
{
    for (int y = room->y; y < room->y + room->h; y++) {
        for (int x = room->x; x < room->x + room->w; x++) {
            rg.kind[y][x] = RG_FLOOR;
        }
    }
}

static void rg_carve_hall(int x0, int y0, int x1, int y1)
{
    while (x0 != x1) {
        if (rg.kind[y0][x0] == RG_WALL) {
            rg.kind[y0][x0] = RG_FLOOR;
        }
        x0 += x0 < x1 ? 1 : -1;
    }
    while (y0 != y1) {
        if (rg.kind[y0][x0] == RG_WALL) {
            rg.kind[y0][x0] = RG_FLOOR;
        }
        y0 += y0 < y1 ? 1 : -1;
    }
    if (rg.kind[y1][x1] == RG_WALL) {
        rg.kind[y1][x1] = RG_FLOOR;
    }
}

static void rg_build(void)
{
    rg_room_t rooms[RG_ROOMS];
    int made = 0;
    memset(rg.kind, 0, sizeof(rg.kind));
    memset(rg.seen, 0, sizeof(rg.seen));
    memset(rg.mons, 0, sizeof(rg.mons));
    memset(rg.items, 0, sizeof(rg.items));
    for (int i = 0; i < RG_ROOMS; i++) {
        rooms[i].on = false;
        for (int tries = 0; tries < 30; tries++) {
            const int w = 4 + (int)(rg_random() % 6U);
            const int h = 3 + (int)(rg_random() % 3U);
            const int x = 1 + (int)(rg_random() % (uint32_t)(RG_W - w - 2));
            const int y = 1 + (int)(rg_random() % (uint32_t)(RG_H - h - 2));
            bool blocked = false;
            for (int j = 0; j < i; j++) {
                if (!rooms[j].on) {
                    continue;
                }
                if (x < rooms[j].x + rooms[j].w + 1 && x + w + 1 > rooms[j].x &&
                    y < rooms[j].y + rooms[j].h + 1 && y + h + 1 > rooms[j].y) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                continue;
            }
            rooms[i].x = x;
            rooms[i].y = y;
            rooms[i].w = w;
            rooms[i].h = h;
            rooms[i].on = true;
            rg_carve_room(&rooms[i]);
            made++;
            break;
        }
    }
    if (made == 0) {
        rooms[0].x = 4;
        rooms[0].y = 4;
        rooms[0].w = 10;
        rooms[0].h = 6;
        rooms[0].on = true;
        rg_carve_room(&rooms[0]);
        made = 1;
    }
    {
        int previous = -1;
        int first = -1;
        int last = 0;
        for (int i = 0; i < RG_ROOMS; i++) {
            if (!rooms[i].on) {
                continue;
            }
            if (first < 0) {
                first = i;
            }
            if (previous >= 0) {
                rg_carve_hall(rooms[previous].x + rooms[previous].w / 2,
                              rooms[previous].y + rooms[previous].h / 2,
                              rooms[i].x + rooms[i].w / 2,
                              rooms[i].y + rooms[i].h / 2);
            }
            previous = i;
            last = i;
        }
        rg.px = rooms[first].x + rooms[first].w / 2;
        rg.py = rooms[first].y + rooms[first].h / 2;
        rg.kind[rg.py][rg.px] = RG_UP;
        if (rg.depth < RG_DEPTHS) {
            rg.kind[rooms[last].y + rooms[last].h / 2][rooms[last].x + rooms[last].w / 2] =
                RG_DOWN;
        }
    }
    for (int n = 0; n < 4 + rg.depth && n < RG_MONS; n++) {
        int choices[RG_KINDS];
        int nchoices = 0;
        int x = 0;
        int y = 0;
        int pick;
        for (int k = 0; k < RG_KINDS; k++) {
            if (rg.depth >= rg_kinds[k].min_d && rg.depth <= rg_kinds[k].max_d) {
                choices[nchoices++] = k;
            }
        }
        if (nchoices == 0 || !rg_random_floor(&x, &y)) {
            break;
        }
        pick = choices[rg_random() % (uint32_t)nchoices];
        rg.mons[n].alive = true;
        rg.mons[n].x = (uint8_t)x;
        rg.mons[n].y = (uint8_t)y;
        rg.mons[n].kind = (uint8_t)pick;
        rg.mons[n].hp = rg_kinds[pick].hp;
    }
    for (int n = 0; n < 4; n++) {
        int x = 0;
        int y = 0;
        int kind = IT_GOLD;
        int amount = 2 + (int)(rg_random() % 10U);
        const uint32_t roll = rg_random() % 5U;
        if (!rg_random_floor(&x, &y)) {
            break;
        }
        if (roll == 0) {
            kind = IT_FOOD;
            amount = 1;
        } else if (roll == 1) {
            kind = IT_POTION;
            amount = 1;
        } else if (roll == 2 && rg.depth > 2) {
            kind = IT_WEAPON;
            amount = 1;
        } else if (roll == 3 && rg.depth > 3) {
            kind = IT_ARMOR;
            amount = 1;
        }
        rg_place_item(x, y, kind, amount);
    }
    if (rg.depth == RG_DEPTHS && !rg.amulet) {
        int x = 0;
        int y = 0;
        if (rg_random_floor(&x, &y)) {
            rg_place_item(x, y, IT_AMULET, 1);
        }
    }
    rg_say(rg.depth == 1 ? "the dungeon smells of dust" : "you climb onward");
}

static void rg_new_run(void)
{
    memset(&rg, 0, sizeof(rg));
    rg.rng = rg_boot;
    rg_boot += 0x9e3779b9U;
    rg.hp = 16;
    rg.max_hp = 16;
    rg.level = 1;
    rg.food = 1;
    rg.depth = 1;
    rg.mode = RG_PLAY;
    rg_build();
}

static int rg_dir(uint8_t ch)
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

static void rg_move(int dir)
{
    int dx = 0;
    int dy = 0;
    int nx;
    int ny;
    int mon;
    int item;
    if (dir == 0) {
        dy = -1;
    } else if (dir == 1) {
        dx = 1;
    } else if (dir == 2) {
        dy = 1;
    } else {
        dx = -1;
    }
    nx = rg.px + dx;
    ny = rg.py + dy;
    if (nx < 0 || ny < 0 || nx >= RG_W || ny >= RG_H || rg.kind[ny][nx] == RG_WALL) {
        return;
    }
    mon = rg_mon_at(nx, ny);
    if (mon >= 0) {
        rg_attack(mon);
        rg_hunger_tick();
        rg_monsters_act();
        return;
    }
    rg.px = nx;
    rg.py = ny;
    item = rg_item_at(nx, ny);
    if (item >= 0 && rg.items[item].kind == IT_GOLD) {
        rg_take(item);
    } else if (item >= 0) {
        rg_say("something is here. g gets it");
    }
    rg_hunger_tick();
    rg_monsters_act();
}

static void rg_use_stair(void)
{
    const uint8_t here = rg.kind[rg.py][rg.px];
    if (here == RG_DOWN) {
        rg.depth++;
        rg_build();
        return;
    }
    if (here != RG_UP) {
        rg_say("no stair here");
        return;
    }
    if (rg.depth == 1) {
        if (rg.amulet) {
            rg.mode = RG_WON;
            rg_say("you emerge with the amulet");
        } else {
            rg_say("the way out waits for the amulet");
        }
        return;
    }
    rg.depth--;
    rg_build();
}

static char rg_item_glyph(int kind)
{
    switch (kind) {
    case IT_GOLD:
        return '$';
    case IT_FOOD:
        return '%';
    case IT_POTION:
        return '!';
    case IT_WEAPON:
        return ')';
    case IT_ARMOR:
        return '[';
    case IT_AMULET:
        return '*';
    default:
        return '?';
    }
}

static char rg_tile_glyph(int x, int y)
{
    switch (rg.kind[y][x]) {
    case RG_DOWN:
        return '>';
    case RG_UP:
        return '<';
    case RG_WALL:
        return '#';
    default:
        return '.';
    }
}

static void rg_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    char hud[80];
    char glyph[2];
    int cell = 14;
    int cols;
    int rows;
    int left;
    int top;
    if (gfx == NULL) {
        return;
    }
    cols = ((int)solar_os_gfx_width(gfx) - 8) / cell;
    rows = ((int)solar_os_gfx_height(gfx) - 46) / cell;
    if (cols < 8) {
        cols = 8;
    }
    if (rows < 6) {
        rows = 6;
    }
    left = rg.px - (cols / 2);
    top = rg.py - (rows / 2);
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (left > RG_W - cols) {
        left = RG_W - cols;
    }
    if (top > RG_H - rows) {
        top = RG_H - rows;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    snprintf(hud,
             sizeof(hud),
             "rogue d%d  hp %d/%d  lv%d  $%d  food %d",
             rg.depth,
             rg.hp,
             rg.max_hp,
             rg.level,
             rg.gold,
             rg.food);
    solar_os_gfx_text(gfx, 4, 12, hud);
    solar_os_gfx_text(gfx, 4, 26, rg.msg);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO);
    for (int sy = 0; sy < rows; sy++) {
        for (int sx = 0; sx < cols; sx++) {
            const int x = left + sx;
            const int y = top + sy;
            const int px = 4 + (sx * cell);
            const int py = 34 + (sy * cell);
            bool vis;
            int mon;
            int item;
            char ch;
            if (x < 0 || y < 0 || x >= RG_W || y >= RG_H) {
                continue;
            }
            vis = rg_visible(x, y);
            if (vis) {
                rg.seen[y][x] = 1;
            }
            if (!vis && !rg.seen[y][x]) {
                continue;
            }
            ch = rg_tile_glyph(x, y);
            mon = vis ? rg_mon_at(x, y) : -1;
            item = vis ? rg_item_at(x, y) : -1;
            if (mon >= 0) {
                ch = rg_kinds[rg.mons[mon].kind].glyph;
            } else if (item >= 0) {
                ch = rg_item_glyph(rg.items[item].kind);
            }
            if (x == rg.px && y == rg.py) {
                ch = '@';
            }
            glyph[0] = ch;
            glyph[1] = '\0';
            solar_os_gfx_set_color(gfx, vis ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_text(gfx, px, py + cell - 2, glyph);
        }
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx,
                      4,
                      (int)solar_os_gfx_height(gfx) - 3,
                      "WASD move  g get  e eat  q drink  Enter stair  Esc quit");
    if (rg.mode == RG_DEAD) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 130, 150, "YOU DIED");
    } else if (rg.mode == RG_WON) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 90, 150, "AMULET RECOVERED");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t rg_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    rg_new_run();
    rg_render(ctx);
    return ESP_OK;
}

static void rg_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool rg_handle(solar_os_context_t *ctx, uint8_t ch)
{
    const int dir = rg_dir(ch);
    if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_APP_EXIT || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (rg.mode != RG_PLAY) {
        if (ch == 'r' || ch == 'R' || ch == ' ' || ch == '\n' || ch == '\r') {
            rg_new_run();
            rg_render(ctx);
        }
        return true;
    }
    if (dir >= 0) {
        rg_move(dir);
        rg_render(ctx);
        return true;
    }
    if (ch == 'g' || ch == 'G') {
        const int item = rg_item_at(rg.px, rg.py);
        if (item >= 0) {
            rg_take(item);
        } else {
            rg_say("nothing here");
        }
        rg_render(ctx);
        return true;
    }
    if (ch == 'e' || ch == 'E') {
        if (rg.food <= 0) {
            rg_say("no food");
        } else {
            rg.food--;
            rg.hunger = 0;
            rg.hungry_said = false;
            if (rg.hp + 6 < rg.max_hp) {
                rg.hp += 6;
            } else {
                rg.hp = rg.max_hp;
            }
            rg_say("you eat");
        }
        rg_render(ctx);
        return true;
    }
    if (ch == 'q') {
        int heal;
        if (rg.potions <= 0) {
            rg_say("no potion");
        } else {
            rg.potions--;
            heal = (rg.max_hp - rg.hp) / 2 + 4;
            rg.hp += heal;
            if (rg.hp > rg.max_hp) {
                rg.hp = rg.max_hp;
            }
            rg_say("you drink");
        }
        rg_render(ctx);
        return true;
    }
    if (ch == '\n' || ch == '\r') {
        rg_use_stair();
        rg_render(ctx);
    }
    return true;
}

static bool rg_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return rg_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        rg_render(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_rogue_app = {
    .name = "rogue",
    .summary = "descend for the amulet",
    .start = rg_start,
    .stop = rg_stop,
    .event = rg_event,
    .state_slot = &rg_state,
    .state_size = sizeof(rg_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 250U,
    .tick_deadline_ms = 30U,
};
