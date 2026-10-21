#include "solar_os_moria.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gfx.h"
#include "solar_os_keys.h"

#define MO_W 48
#define MO_H 24
#define MO_ROOMS 6
#define MO_MONS 16
#define MO_ITEMS 14
#define MO_BOTTOM 16

enum {
    MO_WALL = 0,
    MO_FLOOR,
    MO_DOWN,
    MO_UP,
    MO_NPC,
    MO_SHOP,
};

enum {
    MO_PLAY,
    MO_DEAD,
    MO_WON,
};

enum {
    IT_GOLD = 1,
    IT_FOOD,
    IT_POTION,
    IT_LAMP,
    IT_BLADE,
    IT_MAIL,
};

enum {
    Q_BRIEF,
    Q_LAMP,
    Q_AFTER_LAMP,
    Q_CAPTAIN,
    Q_AFTER_CAPTAIN,
    Q_TERROR,
};

enum {
    K_CAPTAIN = 7,
    K_WARDEN = 8,
};

typedef struct {
    char glyph;
    const char *name;
    uint8_t hp;
    uint8_t atk;
    uint8_t xp;
    uint8_t min_d;
    uint8_t max_d;
} mo_kind_t;

typedef struct {
    int x, y, w, h;
    bool on;
} mo_room_t;

typedef struct {
    uint8_t x, y, kind, hp;
    bool alive;
} mo_mon_t;

typedef struct {
    uint8_t x, y, kind, amount;
    bool on;
} mo_item_t;

typedef struct {
    uint8_t kind[MO_H][MO_W];
    uint8_t seen[MO_H][MO_W];
    mo_mon_t mons[MO_MONS];
    mo_item_t items[MO_ITEMS];
    int px, py;
    int hp, max_hp, level, xp;
    int gold, food, potions, weapon, armor, light, hunger;
    int depth, quest, lamp_depth, captain_depth;
    int mode, turn;
    bool lamp, captain_dead, terror_dead, in_town, shop, hungry_said;
    uint32_t rng;
    char msg[80];
} mo_state_t;

static void *mo_state;
#define mo (*(mo_state_t *)mo_state)
static uint32_t mo_boot = 0x1011a001u;

static const mo_kind_t mo_kinds[] = {
    {'r', "cave rat", 3, 1, 2, 1, 6},
    {'b', "bat", 4, 2, 3, 1, 9},
    {'g', "goblin", 6, 2, 4, 2, 12},
    {'o', "orc", 8, 3, 6, 3, 15},
    {'w', "warg", 12, 4, 10, 6, 15},
    {'T', "cave troll", 20, 6, 18, 9, 16},
    {'K', "tentacle", 14, 4, 12, 1, 4},
    {'U', "uruk captain", 24, 6, 30, 0, 0},
    {'&', "bridge-warden", 36, 8, 80, 0, 0},
};

#define MO_KINDS ((int)(sizeof(mo_kinds) / sizeof(mo_kinds[0])))

static const char *const mo_town[] = {
    "########################",
    "#......................#",
    "#..N..............S....#",
    "#......................#",
    "#......................#",
    "#.....................>#",
    "#......................#",
    "########################",
};

static const char *const mo_lines[] = {
    "Runes name a king who shut the gate.",
    "The air smells of old smoke.",
    "Claw marks climb higher than a hand.",
    "A pale glint in the rock is only mica.",
    "Someone scratched: the bridge is the end.",
    "Dry wells ring the hall.",
};

static uint32_t mo_random(void)
{
    mo.rng = (mo.rng * 1664525U) + 1013904223U;
    return mo.rng;
}

static void mo_say(const char *text)
{
    snprintf(mo.msg, sizeof(mo.msg), "%s", text);
}

static int mo_abs(int v)
{
    return v < 0 ? -v : v;
}

static int mo_cheb(int x, int y)
{
    const int dx = mo_abs(x - mo.px);
    const int dy = mo_abs(y - mo.py);
    return dx > dy ? dx : dy;
}

static int mo_radius(void)
{
    if (mo.in_town) {
        return 20;
    }
    if (mo.light <= 0) {
        return 1;
    }
    return 2 + (mo.light / 20);
}

static bool mo_passable(int kind)
{
    return kind == MO_FLOOR || kind == MO_DOWN || kind == MO_UP;
}

static bool mo_los(int x1, int y1)
{
    int x0 = mo.px;
    int y0 = mo.py;
    const int dx = mo_abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -mo_abs(y1 - y0);
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
        if (x0 < 0 || y0 < 0 || x0 >= MO_W || y0 >= MO_H || !mo_passable(mo.kind[y0][x0])) {
            return false;
        }
    }
    return true;
}

static bool mo_visible(int x, int y)
{
    if (mo_cheb(x, y) > mo_radius()) {
        return false;
    }
    return mo_los(x, y);
}

static int mo_mon_at(int x, int y)
{
    for (int i = 0; i < MO_MONS; i++) {
        if (mo.mons[i].alive && mo.mons[i].x == x && mo.mons[i].y == y) {
            return i;
        }
    }
    return -1;
}

static int mo_item_at(int x, int y)
{
    for (int i = 0; i < MO_ITEMS; i++) {
        if (mo.items[i].on && mo.items[i].x == x && mo.items[i].y == y) {
            return i;
        }
    }
    return -1;
}

static bool mo_near(int kind)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int x = mo.px + dx;
            const int y = mo.py + dy;
            if (dx == 0 && dy == 0) {
                continue;
            }
            if (x < 0 || y < 0 || x >= MO_W || y >= MO_H) {
                continue;
            }
            if (mo.kind[y][x] == kind) {
                return true;
            }
        }
    }
    return false;
}

static void mo_gain(int amount)
{
    mo.xp += amount;
    while (mo.xp >= mo.level * 20) {
        mo.xp -= mo.level * 20;
        mo.level++;
        mo.max_hp += 5;
        mo.hp = mo.max_hp;
        mo_say("you feel the dark recede");
    }
}

static void mo_hurt(int amount)
{
    if (amount < 1) {
        amount = 1;
    }
    mo.hp -= amount;
    if (mo.hp <= 0) {
        mo.hp = 0;
        mo.mode = MO_DEAD;
        mo_say("you fall in the dark");
    }
}

static void mo_attack(int index)
{
    mo_mon_t *mon = &mo.mons[index];
    const mo_kind_t *kind = &mo_kinds[mon->kind];
    const int dmg = 1 + mo.weapon + (int)(mo_random() % (uint32_t)(mo.level + 2));
    char line[80];
    mon->hp = (uint8_t)(mon->hp > dmg ? mon->hp - dmg : 0);
    if (mon->hp == 0) {
        mon->alive = false;
        mo.gold += 1 + (int)(mo_random() % 8U);
        if (mon->kind == K_CAPTAIN) {
            mo.captain_dead = true;
            if (mo.quest == Q_CAPTAIN) {
                mo.quest = Q_AFTER_CAPTAIN;
            }
            mo_say("the captain drops. the road is quiet");
        } else if (mon->kind == K_WARDEN) {
            mo.terror_dead = true;
            mo_say("the warden breaks on the bridge");
        } else {
            mo_gain(kind->xp);
            snprintf(line, sizeof(line), "you slay the %s", kind->name);
            if (mo.mode == MO_PLAY) {
                mo_say(line);
            }
        }
        if (mon->kind == K_CAPTAIN || mon->kind == K_WARDEN) {
            mo_gain(kind->xp);
        }
        return;
    }
    snprintf(line, sizeof(line), "you hit the %s", kind->name);
    mo_say(line);
}

static void mo_monsters_act(void)
{
    if (mo.mode != MO_PLAY || mo.in_town) {
        return;
    }
    for (int i = 0; i < MO_MONS; i++) {
        mo_mon_t *mon;
        int best = 99;
        int bx = 0;
        int by = 0;
        bool step = false;
        if (!mo.mons[i].alive) {
            continue;
        }
        mon = &mo.mons[i];
        if (mo_cheb(mon->x, mon->y) > mo_radius() + 2) {
            continue;
        }
        if (mo_cheb(mon->x, mon->y) <= 1) {
            char line[80];
            snprintf(line, sizeof(line), "the %s hits", mo_kinds[mon->kind].name);
            mo_say(line);
            mo_hurt((int)mo_kinds[mon->kind].atk - (mo.armor / 2));
            if (mo.mode != MO_PLAY) {
                return;
            }
            continue;
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                const int nx = (int)mon->x + dx;
                const int ny = (int)mon->y + dy;
                int dist;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= MO_W || ny >= MO_H) {
                    continue;
                }
                if (!mo_passable(mo.kind[ny][nx]) || mo_mon_at(nx, ny) >= 0 ||
                    (nx == mo.px && ny == mo.py)) {
                    continue;
                }
                dist = mo_abs(nx - mo.px) + mo_abs(ny - mo.py);
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

static bool mo_place_item(int x, int y, int kind, int amount)
{
    if (mo_item_at(x, y) >= 0) {
        return false;
    }
    for (int i = 0; i < MO_ITEMS; i++) {
        if (mo.items[i].on) {
            continue;
        }
        mo.items[i].on = true;
        mo.items[i].x = (uint8_t)x;
        mo.items[i].y = (uint8_t)y;
        mo.items[i].kind = (uint8_t)kind;
        mo.items[i].amount = (uint8_t)amount;
        return true;
    }
    return false;
}

static bool mo_place_mon(int x, int y, int kind)
{
    int slot = -1;
    if (mo_mon_at(x, y) >= 0) {
        return false;
    }
    for (int i = 0; i < MO_MONS; i++) {
        if (!mo.mons[i].alive) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return false;
    }
    mo.mons[slot].alive = true;
    mo.mons[slot].x = (uint8_t)x;
    mo.mons[slot].y = (uint8_t)y;
    mo.mons[slot].kind = (uint8_t)kind;
    mo.mons[slot].hp = mo_kinds[kind].hp;
    return true;
}

static bool mo_random_floor(int *x, int *y)
{
    for (int tries = 0; tries < 80; tries++) {
        const int cx = (int)(mo_random() % MO_W);
        const int cy = (int)(mo_random() % MO_H);
        if (!mo_passable(mo.kind[cy][cx]) || (cx == mo.px && cy == mo.py)) {
            continue;
        }
        if (mo_mon_at(cx, cy) >= 0 || mo_item_at(cx, cy) >= 0) {
            continue;
        }
        *x = cx;
        *y = cy;
        return true;
    }
    return false;
}

static void mo_clear_level(void)
{
    memset(mo.kind, 0, sizeof(mo.kind));
    memset(mo.seen, 0, sizeof(mo.seen));
    memset(mo.mons, 0, sizeof(mo.mons));
    memset(mo.items, 0, sizeof(mo.items));
    mo.shop = false;
}

static void mo_carve_room(const mo_room_t *room)
{
    for (int y = room->y; y < room->y + room->h; y++) {
        for (int x = room->x; x < room->x + room->w; x++) {
            mo.kind[y][x] = MO_FLOOR;
        }
    }
}

static void mo_hall(int x0, int y0, int x1, int y1)
{
    while (x0 != x1) {
        if (mo.kind[y0][x0] == MO_WALL) {
            mo.kind[y0][x0] = MO_FLOOR;
        }
        x0 += x0 < x1 ? 1 : -1;
    }
    while (y0 != y1) {
        if (mo.kind[y0][x0] == MO_WALL) {
            mo.kind[y0][x0] = MO_FLOOR;
        }
        y0 += y0 < y1 ? 1 : -1;
    }
    if (mo.kind[y1][x1] == MO_WALL) {
        mo.kind[y1][x1] = MO_FLOOR;
    }
}

static void mo_stock_dungeon(void)
{
    const int count = mo.depth >= MO_BOTTOM ? 0 : 3 + (mo.depth / 2);
    for (int n = 0; n < count && n < MO_MONS - 2; n++) {
        int choices[MO_KINDS];
        int nchoices = 0;
        int x = 0;
        int y = 0;
        for (int k = 0; k < MO_KINDS; k++) {
            if (mo_kinds[k].max_d > 0 && mo.depth >= mo_kinds[k].min_d &&
                mo.depth <= mo_kinds[k].max_d) {
                choices[nchoices++] = k;
            }
        }
        if (nchoices == 0 || !mo_random_floor(&x, &y)) {
            break;
        }
        mo_place_mon(x, y, choices[mo_random() % (uint32_t)nchoices]);
    }
    for (int n = 0; n < 4; n++) {
        int x = 0;
        int y = 0;
        int kind = IT_GOLD;
        int amount = 3 + (int)(mo_random() % 12U);
        const uint32_t roll = mo_random() % 5U;
        if (!mo_random_floor(&x, &y)) {
            break;
        }
        if (roll == 0) {
            kind = IT_FOOD;
            amount = 1;
        } else if (roll == 1) {
            kind = IT_POTION;
            amount = 1;
        } else if (roll == 2) {
            kind = IT_BLADE;
            amount = 1;
        } else if (roll == 3) {
            kind = IT_MAIL;
            amount = 1;
        }
        mo_place_item(x, y, kind, amount);
    }
    if (mo.quest == Q_LAMP && !mo.lamp && mo.depth == mo.lamp_depth) {
        int x = 0;
        int y = 0;
        if (mo_random_floor(&x, &y)) {
            mo_place_item(x, y, IT_LAMP, 1);
            mo_say("a lamp glints in this hall");
        }
    }
    if (mo.quest == Q_CAPTAIN && !mo.captain_dead && mo.depth == mo.captain_depth) {
        int x = 0;
        int y = 0;
        if (mo_random_floor(&x, &y)) {
            mo_place_mon(x, y, K_CAPTAIN);
            mo_say("an uruk voice echoes here");
        }
    }
}

static void mo_load_town(void)
{
    const int rows = (int)(sizeof(mo_town) / sizeof(mo_town[0]));
    const int ox = (MO_W - 24) / 2;
    const int oy = 6;
    mo_clear_level();
    mo.in_town = true;
    mo.hp = mo.max_hp;
    mo.hunger = 0;
    mo.hungry_said = false;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < 24; x++) {
            const char ch = mo_town[y][x];
            const int mx = ox + x;
            const int my = oy + y;
            if (ch == '#') {
                mo.kind[my][mx] = MO_WALL;
            } else if (ch == 'N') {
                mo.kind[my][mx] = MO_NPC;
            } else if (ch == 'S') {
                mo.kind[my][mx] = MO_SHOP;
            } else if (ch == '>') {
                mo.kind[my][mx] = MO_DOWN;
                mo.px = mx - 1;
                mo.py = my;
                mo.kind[my][mx - 1] = MO_FLOOR;
            } else {
                mo.kind[my][mx] = MO_FLOOR;
            }
        }
    }
    mo_say("the west-gate. N knows the mine. S sells gear");
}

static void mo_load_rooms(void)
{
    mo_room_t rooms[MO_ROOMS];
    int made = 0;
    int previous = -1;
    int first = -1;
    int last = 0;
    mo_clear_level();
    mo.in_town = false;
    for (int i = 0; i < MO_ROOMS; i++) {
        rooms[i].on = false;
        for (int tries = 0; tries < 24; tries++) {
            const int w = 4 + (int)(mo_random() % 7U);
            const int h = 3 + (int)(mo_random() % 3U);
            const int x = 1 + (int)(mo_random() % (uint32_t)(MO_W - w - 2));
            const int y = 1 + (int)(mo_random() % (uint32_t)(MO_H - h - 2));
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
            mo_carve_room(&rooms[i]);
            made++;
            break;
        }
    }
    if (made == 0) {
        rooms[0].x = 6;
        rooms[0].y = 6;
        rooms[0].w = 12;
        rooms[0].h = 6;
        rooms[0].on = true;
        mo_carve_room(&rooms[0]);
    }
    for (int i = 0; i < MO_ROOMS; i++) {
        if (!rooms[i].on) {
            continue;
        }
        if (first < 0) {
            first = i;
        }
        if (previous >= 0) {
            mo_hall(rooms[previous].x + rooms[previous].w / 2,
                    rooms[previous].y + rooms[previous].h / 2,
                    rooms[i].x + rooms[i].w / 2,
                    rooms[i].y + rooms[i].h / 2);
        }
        previous = i;
        last = i;
    }
    mo.px = rooms[first].x + rooms[first].w / 2;
    mo.py = rooms[first].y + rooms[first].h / 2;
    mo.kind[rooms[first].y + 1][rooms[first].x + 1] = MO_UP;
    if (mo.depth < MO_BOTTOM) {
        mo.kind[rooms[last].y + rooms[last].h / 2][rooms[last].x + rooms[last].w / 2] = MO_DOWN;
    }
    mo_stock_dungeon();
    if (mo.msg[0] == '\0' || mo.quest == Q_BRIEF) {
        mo_say(mo_lines[mo_random() % (uint32_t)(sizeof(mo_lines) / sizeof(mo_lines[0]))]);
    }
}

static void mo_load_bridge(void)
{
    const int y = MO_H / 2;
    mo_clear_level();
    mo.in_town = false;
    for (int x = 2; x < MO_W - 2; x++) {
        mo.kind[y][x] = MO_FLOOR;
    }
    mo.kind[y][3] = MO_UP;
    mo.px = 5;
    mo.py = y;
    if (!mo.terror_dead) {
        mo_place_mon(MO_W - 8, y, K_WARDEN);
        mo_say("the last bridge. the warden is waiting");
    } else {
        mo_say("the bridge is empty");
    }
}

static void mo_enter(int depth)
{
    mo.depth = depth;
    mo.msg[0] = '\0';
    if (depth <= 0) {
        mo.depth = 0;
        mo_load_town();
        return;
    }
    if (depth >= MO_BOTTOM) {
        mo.depth = MO_BOTTOM;
        mo_load_bridge();
        return;
    }
    mo_load_rooms();
}

static void mo_new_run(void)
{
    memset(&mo, 0, sizeof(mo));
    mo.rng = mo_boot;
    mo_boot += 0x9e3779b9U;
    mo.hp = 18;
    mo.max_hp = 18;
    mo.level = 1;
    mo.gold = 20;
    mo.food = 2;
    mo.light = 70;
    mo.mode = MO_PLAY;
    mo.quest = Q_BRIEF;
    mo_enter(0);
}

static const char *mo_quest_text(void)
{
    if (mo.terror_dead) {
        return "The warden is gone. Speak to N.";
    }
    switch (mo.quest) {
    case Q_LAMP:
        return "Find the miner's lamp in the upper halls.";
    case Q_AFTER_LAMP:
        return "Bring word of the lamp back to N.";
    case Q_CAPTAIN:
        return "Slay the uruk captain on the deep road.";
    case Q_AFTER_CAPTAIN:
        return "The captain is dead. Return to N.";
    case Q_TERROR:
        return "The bridge-warden waits at the bottom.";
    default:
        return "Speak with N at the west-gate.";
    }
}

static void mo_talk(void)
{
    if (mo.shop) {
        mo.shop = false;
        mo_say("you step back from the counter");
        return;
    }
    if (mo_near(MO_SHOP) && !mo_near(MO_NPC)) {
        mo.shop = true;
        mo_say("S nods. 1 food  2 torch  3 mail  4 blade  5 potion");
        return;
    }
    if (!mo_near(MO_NPC)) {
        mo_say("nobody is close enough to hear you");
        return;
    }
    if (mo.terror_dead) {
        mo.mode = MO_WON;
        mo_say("N opens the gate. the mine is quiet");
        return;
    }
    if (mo.quest == Q_BRIEF) {
        mo.lamp_depth = 2 + (int)(mo_random() % 3U);
        mo.quest = Q_LAMP;
        mo_say("N: a miner's lamp is lost in the upper halls");
        return;
    }
    if (mo.quest == Q_LAMP) {
        mo_say("N: the lamp is still below");
        return;
    }
    if (mo.quest == Q_AFTER_LAMP) {
        mo.captain_depth = 8 + (int)(mo_random() % 3U);
        mo.quest = Q_CAPTAIN;
        mo_say("N: an uruk captain holds the deeper road");
        return;
    }
    if (mo.quest == Q_CAPTAIN) {
        mo_say("N: the captain still walks");
        return;
    }
    if (mo.quest == Q_AFTER_CAPTAIN) {
        mo.quest = Q_TERROR;
        mo_say("N: the last bridge keeps a warden of flame");
        return;
    }
    mo_say("N: end the warden, then come home");
}

static void mo_buy(int price, int which)
{
    if (mo.gold < price) {
        mo_say("not enough gold");
        return;
    }
    if (which == 1 && mo.food >= 9) {
        mo_say("you cannot carry more food");
        return;
    }
    if (which == 3 && mo.armor >= 4) {
        mo_say("the mail you have is enough");
        return;
    }
    if (which == 4 && mo.weapon >= 4) {
        mo_say("that blade is no better");
        return;
    }
    if (which == 5 && mo.potions >= 6) {
        mo_say("no room for another potion");
        return;
    }
    mo.gold -= price;
    if (which == 1) {
        mo.food++;
        mo_say("you buy a ration");
    } else if (which == 2) {
        mo.light += 40;
        if (mo.light > 99) {
            mo.light = 99;
        }
        mo_say("you buy a torch");
    } else if (which == 3) {
        mo.armor++;
        mo_say("you buy mail");
    } else if (which == 4) {
        mo.weapon++;
        mo_say("you buy a blade");
    } else if (which == 5) {
        mo.potions++;
        mo_say("you buy a potion");
    }
}

static void mo_take(int index)
{
    mo_item_t *item = &mo.items[index];
    char line[80];
    if (item->kind == IT_GOLD) {
        mo.gold += item->amount;
        snprintf(line, sizeof(line), "gold +%u", (unsigned)item->amount);
        mo_say(line);
        item->on = false;
        return;
    }
    if (item->kind == IT_FOOD && mo.food < 9) {
        mo.food++;
        item->on = false;
        mo_say("you take food");
        return;
    }
    if (item->kind == IT_POTION && mo.potions < 6) {
        mo.potions++;
        item->on = false;
        mo_say("you take a potion");
        return;
    }
    if (item->kind == IT_LAMP) {
        mo.lamp = true;
        item->on = false;
        if (mo.quest == Q_LAMP) {
            mo.quest = Q_AFTER_LAMP;
        }
        mo.light += 20;
        if (mo.light > 99) {
            mo.light = 99;
        }
        mo_say("the lamp still burns. bring word to N");
        return;
    }
    if (item->kind == IT_BLADE && mo.weapon < 4) {
        mo.weapon++;
        item->on = false;
        mo_say("you take a blade");
        return;
    }
    if (item->kind == IT_MAIL && mo.armor < 4) {
        mo.armor++;
        item->on = false;
        mo_say("you take mail");
        return;
    }
    mo_say("you leave it");
}

static void mo_spend_turn(void)
{
    if (mo.in_town || mo.mode != MO_PLAY) {
        return;
    }
    mo.turn++;
    if ((mo.turn % 2) == 0 && mo.light > 0) {
        mo.light--;
    }
    mo.hunger++;
    if (mo.hunger == 50) {
        mo.hungry_said = true;
        mo_say("you are hungry");
    }
    if (mo.hunger > 80 && mo.mode == MO_PLAY) {
        mo_hurt(1);
        if (mo.mode == MO_PLAY) {
            mo_say("you are starving");
        }
    }
}

static void mo_move(int dir)
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
    nx = mo.px + dx;
    ny = mo.py + dy;
    if (nx < 0 || ny < 0 || nx >= MO_W || ny >= MO_H || !mo_passable(mo.kind[ny][nx])) {
        return;
    }
    mon = mo_mon_at(nx, ny);
    if (mon >= 0) {
        mo_attack(mon);
        mo_spend_turn();
        mo_monsters_act();
        return;
    }
    mo.px = nx;
    mo.py = ny;
    item = mo_item_at(nx, ny);
    if (item >= 0 && mo.items[item].kind == IT_GOLD) {
        mo_take(item);
    } else if (item >= 0) {
        mo_say("something lies here. g takes it");
    }
    mo_spend_turn();
    mo_monsters_act();
}

static void mo_stair(void)
{
    const uint8_t here = mo.kind[mo.py][mo.px];
    if (here == MO_DOWN) {
        if (mo.depth + 1 >= MO_BOTTOM && !mo.captain_dead) {
            mo_say("the deep stair is shut until the captain falls");
            return;
        }
        mo_enter(mo.depth + 1);
        return;
    }
    if (here == MO_UP) {
        mo_enter(mo.depth - 1);
        return;
    }
    if (mo_near(MO_NPC) || mo_near(MO_SHOP)) {
        mo_talk();
        return;
    }
    mo_say("no stair here");
}

static int mo_dir(uint8_t ch)
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

static char mo_item_glyph(int kind)
{
    switch (kind) {
    case IT_GOLD:
        return '$';
    case IT_FOOD:
        return '%';
    case IT_POTION:
        return '!';
    case IT_LAMP:
        return '(';
    case IT_BLADE:
        return ')';
    case IT_MAIL:
        return '[';
    default:
        return '?';
    }
}

static char mo_tile_glyph(int kind)
{
    switch (kind) {
    case MO_DOWN:
        return '>';
    case MO_UP:
        return '<';
    case MO_NPC:
        return 'N';
    case MO_SHOP:
        return 'S';
    case MO_WALL:
        return '#';
    default:
        return '.';
    }
}

static void mo_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    char hud[96];
    char glyph[2];
    const int cell = 14;
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
    left = mo.px - (cols / 2);
    top = mo.py - (rows / 2);
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (left > MO_W - cols) {
        left = MO_W - cols;
    }
    if (top > MO_H - rows) {
        top = MO_H - rows;
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
    if (mo.in_town) {
        snprintf(hud,
                 sizeof(hud),
                 "moria gate  hp %d/%d  lv%d  $%d  lit %d",
                 mo.hp,
                 mo.max_hp,
                 mo.level,
                 mo.gold,
                 mo.light);
    } else {
        snprintf(hud,
                 sizeof(hud),
                 "moria hall %d  hp %d/%d  lv%d  $%d  lit %d",
                 mo.depth,
                 mo.hp,
                 mo.max_hp,
                 mo.level,
                 mo.gold,
                 mo.light);
    }
    solar_os_gfx_text(gfx, 4, 12, hud);
    solar_os_gfx_text(gfx, 4, 26, mo.msg);
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
            if (x < 0 || y < 0 || x >= MO_W || y >= MO_H) {
                continue;
            }
            vis = mo_visible(x, y);
            if (vis) {
                mo.seen[y][x] = 1;
            }
            if (!vis && !mo.seen[y][x]) {
                continue;
            }
            ch = mo_tile_glyph(mo.kind[y][x]);
            mon = vis ? mo_mon_at(x, y) : -1;
            item = vis ? mo_item_at(x, y) : -1;
            if (mon >= 0) {
                ch = mo_kinds[mo.mons[mon].kind].glyph;
            } else if (item >= 0) {
                ch = mo_item_glyph(mo.items[item].kind);
            }
            if (x == mo.px && y == mo.py) {
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
    if (mo.shop) {
        solar_os_gfx_text(gfx,
                          4,
                          (int)solar_os_gfx_height(gfx) - 3,
                          "1 food12  2 torch10  3 mail40  4 blade55  5 pot22");
    } else {
        solar_os_gfx_text(gfx,
                          4,
                          (int)solar_os_gfx_height(gfx) - 3,
                          "WASD  g get  e eat  q drink  t talk  v quest  Esc quit");
    }
    if (mo.mode == MO_DEAD) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 110, 150, "YOU DIED");
    } else if (mo.mode == MO_WON) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 70, 150, "THE GATE OPENS");
    }
    solar_os_gfx_present(gfx);
}

static esp_err_t mo_start(solar_os_context_t *ctx)
{
    if (solar_os_context_gfx(ctx) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_context_set_graphics_active(ctx, true);
    mo_new_run();
    mo_render(ctx);
    return ESP_OK;
}

static void mo_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool mo_handle(solar_os_context_t *ctx, uint8_t ch)
{
    const int dir = mo_dir(ch);
    if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_APP_EXIT || ch == 'Q') {
        if (mo.shop) {
            mo.shop = false;
            mo_say("you leave the counter");
            mo_render(ctx);
            return true;
        }
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (mo.mode != MO_PLAY) {
        if (ch == 'r' || ch == 'R' || ch == ' ' || ch == '\n' || ch == '\r') {
            mo_new_run();
            mo_render(ctx);
        }
        return true;
    }
    if (mo.shop) {
        if (ch == '1') {
            mo_buy(12, 1);
        } else if (ch == '2') {
            mo_buy(10, 2);
        } else if (ch == '3') {
            mo_buy(40, 3);
        } else if (ch == '4') {
            mo_buy(55, 4);
        } else if (ch == '5') {
            mo_buy(22, 5);
        } else if (ch == 't' || ch == 'T') {
            mo.shop = false;
            mo_say("you leave the counter");
        }
        mo_render(ctx);
        return true;
    }
    if (dir >= 0) {
        mo_move(dir);
        mo_render(ctx);
        return true;
    }
    if (ch == 'g' || ch == 'G') {
        const int item = mo_item_at(mo.px, mo.py);
        if (item >= 0) {
            mo_take(item);
        } else {
            mo_say("nothing here");
        }
        mo_render(ctx);
        return true;
    }
    if (ch == 'e' || ch == 'E') {
        if (mo.food <= 0) {
            mo_say("no food");
        } else {
            mo.food--;
            mo.hunger = 0;
            mo.hungry_said = false;
            mo.hp += 6;
            if (mo.hp > mo.max_hp) {
                mo.hp = mo.max_hp;
            }
            mo_say("you eat");
        }
        mo_render(ctx);
        return true;
    }
    if (ch == 'q') {
        if (mo.potions <= 0) {
            mo_say("no potion");
        } else {
            mo.potions--;
            mo.hp += (mo.max_hp - mo.hp) / 2 + 4;
            if (mo.hp > mo.max_hp) {
                mo.hp = mo.max_hp;
            }
            mo_say("you drink");
        }
        mo_render(ctx);
        return true;
    }
    if (ch == 't' || ch == 'T') {
        mo_talk();
        mo_render(ctx);
        return true;
    }
    if (ch == 'v' || ch == 'V') {
        mo_say(mo_quest_text());
        mo_render(ctx);
        return true;
    }
    if (ch == '\n' || ch == '\r') {
        mo_stair();
        mo_render(ctx);
    }
    return true;
}

static bool mo_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return mo_handle(ctx, (uint8_t)event->data.ch);
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        mo_render(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        return true;
    }
    return false;
}

const solar_os_app_t solar_os_moria_app = {
    .name = "moria",
    .summary = "the mines beneath the west-gate",
    .start = mo_start,
    .stop = mo_stop,
    .event = mo_event,
    .state_slot = &mo_state,
    .state_size = sizeof(mo_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 250U,
    .tick_deadline_ms = 30U,
};
