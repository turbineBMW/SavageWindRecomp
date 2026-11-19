// Options > Accessibility (settings.h): the assists, each off by default. Their patches in the game code
// (tools/patches.py) ask here while it runs; a game version whose code has no place for one (tools/game_sites.py
// OPTIONAL_GROUPS) goes without it.
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "settings.h"

// The damage multipliers: d percent of a loss, rounded, and at least 1 while there is a loss
static int32_t scaled(int32_t d, int pct) {
    if (d <= 0 || pct == 100) return d;
    int32_t s = (int32_t)(((int64_t)d * pct + 50) / 100);
    return s < 1 ? 1 : s;
}

// The cost a hit takes off a life (tools/patches.py _no_wear_hit: every life goes through that function), by whose life
// it is: a weapon's, bow's or shield's durability (LIFE_WEAPON: Unbreakable weapons, from #296, else Weapon wear's share
// of the game's cost, rounded down: a light hit at a low setting can cost nothing), Link's hearts
// (LIFE_LINK: left to rt_link_life, which sees every loss of his), or anything else's (One-hit kill, the damage dealt).
enum { LIFE_OTHER, LIFE_WEAPON, LIFE_LINK };
uint32_t rt_life_cost(uint32_t cost, int32_t life, int whose) {
    if (whose == LIFE_WEAPON) {
        if (g_settings.no_weapon_wear || g_settings.weapon_wear <= 0) return 0;
        int pct = g_settings.weapon_wear;
        return pct == 100 || (int32_t)cost <= 0 ? cost : (uint32_t)((uint64_t)cost * (uint32_t)pct / 100);
    }
    if (whose == LIFE_LINK || (int32_t)cost <= 0) return cost;
    if (g_settings.one_hit_kill) return life > (int32_t)cost ? (uint32_t)life : cost;
    return (uint32_t)scaled((int32_t)cost, g_settings.damage_dealt);
}

// Unbreakable weapons (from #296), the pouch's copy: its setter for an equipped item's value doesn't store a lower one
// (tools/patches.py _no_wear). Arrows (type 2) are counts, not wear, and are left alone.
int rt_keep_wear(uint32_t type, int32_t newv, int32_t oldv) {
    return (g_settings.no_weapon_wear || g_settings.weapon_wear <= 0) && type != 2 && newv < oldv;
}

// God mode, the damage taken: Link's damage handling stores his new life, in quarter hearts, through here (tools/patches.py
// _link_life): God mode keeps the old one, the multiplier scales the loss.
int32_t rt_link_life(int32_t oldv, int32_t newv) {
    if (newv >= oldv) return newv;
    if (g_settings.god_mode) return oldv;
    int32_t v = oldv - scaled(oldv - newv, g_settings.damage_taken);
    return v < 0 ? 0 : v;
}

// Infinite stamina: the stamina setter keeps the old value instead of storing a lower one (tools/patches.py _stamina)
uint32_t rt_stamina(uint32_t oldv, uint32_t newv) {
    float o, n;
    memcpy(&o, &oldv, 4), memcpy(&n, &newv, 4);
    return g_settings.infinite_stamina && n < o ? oldv : newv;
}

// Infinite arrows: the bow's shot takes its arrows off the pouch through one function, which returns before taking
// any (tools/patches.py _remove_arrows). One arrow is still needed to shoot.
int rt_infinite_arrows(void) { return g_settings.infinite_arrows; }

// Drop rate and Rare drop rate (tools/patches.py _drop_rolls, _drop_roll, _drop_single). A drop rolls its table a
// number of times (RepeatNumMin..RepeatNumMax at random: rt_drop_rolls), and each roll r, 0..100, takes the first item
// whose probability p, in percent, is more than what is left of r after the ones before it, or nothing when r is past
// them all. So an item's real chance is the part of 0..100 its p covers in the table's order (drop_share: an item past
// 100 never drops). Each item's chance is scaled by its own rate, Rare drop rate for the rare ones (p up to
// RARE_DROP_PCT), Drop rate for the others. Where the scaled chances come to more than 100 (always, above 100%, for a
// table that drops something every roll), the table rolls m times as often (m = their sum / 100, rounded at random to a
// whole number of rolls, m times the game's on average) and each roll takes 1/m of every chance: every item drops its
// own rate times as often, and one rate never changes the other's items. A roll picks an item by those weights and the
// game is handed a roll in the middle of that item's own range, or past them all.
// The game also picks exactly one item from a table, with no roll count, in a couple of places (_drop_single): those
// keep the game's own pick.
#define RARE_DROP_PCT 10.0f
#define DROP_ROLLS_MAX 30  // (a drop's rolls, unless the game's own are more)
static float drop_p(uint64_t items, uint32_t count, uint32_t i) {
    float p;
    memcpy(&p, (const void *)(uintptr_t)(items + (uint64_t)(i < count ? i : 0) * 0x48 + 0x40), 4);
    return p;
}
static double drop_share(double before, float p) {
    return (before + p < 100 ? before + p : 100) - (before < 100 ? before : 100);
}
static double drop_scale(float p) {
    return (p <= RARE_DROP_PCT ? g_settings.rare_drop_rate : g_settings.drop_rate) / 100.0;
}
// The sum of a table's scaled chances, percent; -1: the game's own drops (both rates at 100%, an empty table, or a
// negative or NaN probability)
static double drop_total(uint64_t items, uint32_t count, uint32_t columns) {
    if ((g_settings.drop_rate == 100 && g_settings.rare_drop_rate == 100) || !items || !count || !columns) return -1;
    double t = 0, before = 0;
    for (uint32_t i = 0; i < columns; i++) {
        float p = drop_p(items, count, i);
        if (!(p >= 0)) return -1;
        t += drop_share(before, p) * drop_scale(p), before += p;
    }
    return t;
}

static __thread int drop_single;  // in one of the game's single picks (rt_drop_single)
void rt_drop_single(int on) { drop_single += on ? 1 : -1; }

float rt_drop_roll(float r, uint64_t items, uint32_t count, uint32_t columns) {
    double t = drop_single ? -1 : drop_total(items, count, columns);
    if (t < 0) return r;
    double m = t > 100 ? t / 100 : 1, left = r, before = 0;
    for (uint32_t i = 0; i < columns; i++) {
        float p = drop_p(items, count, i);
        double e = drop_share(before, p), w = e * drop_scale(p) / m;
        if (e > 0 && left < w) return (float)(before + e / 2);
        left -= w, before += p;
    }
    return (float)(before + 1);  // (past every item: nothing)
}

// 0 <= x < 1 (splitmix64 per thread: the extra rolls' rounding and count need no particular sequence)
static __thread uint64_t drop_rng;
static double drop_random(void) {
    if (!drop_rng) drop_rng = ((uint64_t)(uintptr_t)&drop_rng ^ (uint64_t)time(NULL) << 20) | 1;
    uint64_t z = (drop_rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return (double)((z ^ (z >> 31)) >> 11) * 0x1p-53;
}

// A drop table's roll count: table is the game's 0x108-byte table (ColumnNum +0xf0, the items' count +0xf8 and array
// +0x100), lo and hi its RepeatNumMin and RepeatNumMax. -1: the game's own count (it fits its rolls).
int64_t rt_drop_rolls(uint64_t table, uint32_t lo, uint32_t hi) {
    uint64_t items;
    uint32_t count, columns;
    memcpy(&items, (const void *)(uintptr_t)(table + 0x100), 8);
    memcpy(&count, (const void *)(uintptr_t)(table + 0xf8), 4);
    memcpy(&columns, (const void *)(uintptr_t)(table + 0xf0), 4);
    double t = drop_total(items, count, columns);
    if (t <= 100 || (int32_t)lo < 0 || (int32_t)hi < (int32_t)lo) return -1;
    uint32_t n = lo + (uint32_t)(drop_random() * ((double)hi - lo + 1));
    double want = n * t / 100;
    uint32_t k = (uint32_t)want, max = n > DROP_ROLLS_MAX ? n : DROP_ROLLS_MAX;
    if (drop_random() < want - k) k++;
    return k < max ? k : max;
}
