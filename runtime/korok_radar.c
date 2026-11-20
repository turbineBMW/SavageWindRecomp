// Cheats > Korok radar (korok_radar.h): the Koroks Link hasn't found yet around him, drawn by the overlay
// (runtime/ui/menu.mm) in a corner while he is on the main field. It only reads: nothing in the game or the save changes.
//
// Where they are: the 900 Koroks of runtime/control_places_data.inc (`goto korok H23`), the map's marker positions,
// and their HashIds in runtime/korok_radar_data.inc (both made by tools/gen_places.py). Whether one is found: its
// game data flag, named after the HashId of its actor, MainField_Npc_HiddenKorokGround_<id> for one hidden on the
// ground (under a rock, in a flower...), MainField_Npc_HiddenKorokFly_<id> for one a puzzle makes appear; a Korok has
// only one of the two. Each is a bool flag object on the game's heap (ksys::gdt, see runtime/rupees.c for the s32
// ones): 0x18 bytes, +0 its class (vtable, in main), +8 a word whose bit 24 is the value, +0xc the crc32 of its name.
// The thread finds the bool class from the first Korok's flag, then all 900 objects in one pass over the heap, and
// reads them every KOROK_POLL_MS; an object that is no longer the same flag (a new game, a load) sends it back to the
// pass. (1.9.0: the class at 0x7001d21c20, found by reading a flag before and after loading a save with it set.)
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "control.h"
#include "control_places.h"
#include "game_place.h"
#include "game_sites.h"
#include "hle.h"
#include "kernel.h"
#include "korok_radar.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"

#define KOROK_POLL_MS 500  // (a Korok found shows as found within this)
#define KOROK_RETRY_MS 10000  // (a pass that found no flags: the next one)
#define PLAY_FRESH_MS 250  // Link's damage handling ran this recently: in play (no menu, no loading screen)

typedef struct { const char *key; uint32_t hash_id; } KorokId;
static const KorokId ids[] = {
#define KOROK(key, hash_id) {key, hash_id},
#include "korok_radar_data.inc"
#undef KOROK
};
#define KOROKS ((int)(sizeof ids / sizeof *ids))
enum { K_UNKNOWN, K_LEFT, K_FOUND };

static float pos[KOROKS][3];
static uint32_t name_hash[KOROKS][2];  // crc32 of its ..Ground_<id> and ..Fly_<id> flag names
static struct { uint32_t hash; uint16_t i; } by_hash[KOROKS * 2];  // (sorted by hash: the pass looks each object up)
static bool have_data;                 // (ids and places matched up; once)
static uint64_t vt_bool, obj[KOROKS];  // the bool class and each Korok's flag object (thread only)
static uint32_t obj_hash[KOROKS];      // which of its two names the object has
static uint8_t state[KOROKS];          // (atomic) K_*
static int ready;                      // (atomic) state[] was read from the flags since the option went on

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool running, stop_req;
static uint64_t play_ms;  // (atomic) korok_radar_play_tick's last call

static uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static uint64_t rd64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static bool stopping(void) { return __atomic_load_n(&stop_req, __ATOMIC_RELAXED); }
static bool in_main(uint64_t v) { return v >= 0x7000000000ull && g_site.main_end && v < g_site.main_end && !(v & 7); }
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int by_hash_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// The positions from the places table and both flag names of each Korok. false: the two tables don't match
static bool load_data(void) {
    if (place_kind_count(PLACE_KOROK) != KOROKS) return false;
    for (int i = 0; i < KOROKS; i++) {
        const Place *p = place_by_number(PLACE_KOROK, i + 1);  // (the same order: tools/gen_places.py)
        if (!p || strcmp(p->key, ids[i].key)) return false;
        pos[i][0] = p->x, pos[i][1] = p->y, pos[i][2] = p->z;
        for (int k = 0; k < 2; k++) {
            char name[64];
            int n = snprintf(name, sizeof name, "MainField_Npc_HiddenKorok%s_%u", k ? "Fly" : "Ground", (unsigned)ids[i].hash_id);
            name_hash[i][k] = (uint32_t)crc32(0, (const Bytef *)name, (uInt)n);
            by_hash[i * 2 + k].hash = name_hash[i][k], by_hash[i * 2 + k].i = (uint16_t)i;
        }
    }
    qsort(by_hash, KOROKS * 2, sizeof *by_hash, by_hash_cmp);
    return true;
}

static bool still_flag(int i) {
    uint64_t a, e, o = obj[i];
    return o && plat_committed_range(o, o + 0x18, &a, &e) && a == o && e == o + 0x18 && rd64(o) == vt_bool &&
           rd32(o + 0xc) == obj_hash[i];
}

// One pass over the heap: the bool class (from the first Korok's flag, whichever name it has: the hash at +0xc, the
// class at +0), then every Korok's flag object. Returns how many were found.
static int find_flags(void) {
    uint64_t lo = HEAP_BASE, hi = HEAP_BASE + kernel_heap_size(), a, e;
    if (hi <= lo) return 0;
    for (uint64_t at = lo; !vt_bool && !stopping() && plat_committed_range(at, hi, &a, &e); at = e)
        for (uint64_t p = ((a + 0xc + 7) & ~7ull) - 4; p + 4 <= e; p += 8) {  // (p - 0xc 8-aligned)
            uint32_t h = rd32(p);
            if (h != name_hash[0][0] && h != name_hash[0][1]) continue;
            uint64_t vt = rd64(p - 0xc);
            if (in_main(vt)) { vt_bool = vt; break; }
        }
    if (!vt_bool) return 0;
    memset(obj, 0, sizeof obj);
    int n = 0;
    for (uint64_t at = lo; !stopping() && plat_committed_range(at, hi, &a, &e); at = e)
        for (uint64_t p = (a + 7) & ~7ull; p + 0x18 <= e; p += 8) {
            if (rd64(p) != vt_bool) continue;
            uint32_t h = rd32(p + 0xc);
            const void *hit = bsearch(&h, by_hash, KOROKS * 2, sizeof *by_hash, by_hash_cmp);
            if (!hit) continue;
            int i = ((const __typeof__(*by_hash) *)hit)->i;
            if (!obj[i]) obj[i] = p, obj_hash[i] = h, n++;
        }
    return n;
}

// In a loaded game: Link's PlayerInfo is there (at the title the save's flags are not loaded yet)
static bool in_game(void) {
    return kernel_heap_size() && g_site.player_info && *(volatile uint64_t *)(uintptr_t)g_site.player_info;
}

static void *radar_thread(void *arg) {
    (void)arg;
    int have = 0;           // flag objects the last pass found
    uint64_t searched = 0;  // when it ran (0: not since the option went on); none found: again after KOROK_RETRY_MS
    pthread_mutex_lock(&lock);
    while (!stop_req) {
        pthread_mutex_unlock(&lock);
        if (g_settings.korok_radar && have_data && in_game()) {
            bool moved = false;
            for (int i = 0; i < KOROKS && !moved; i++) moved = obj[i] && !still_flag(i);
            if (moved || !searched || (!have && now_ms() - searched > KOROK_RETRY_MS)) {
                have = find_flags();
                searched = now_ms();
                LOG("[cheats] korok radar: %d of %d Korok flags found on the heap\n", have, KOROKS);
            }
            for (int i = 0; i < KOROKS; i++)
                __atomic_store_n(&state[i], !obj[i] ? K_UNKNOWN : (rd32(obj[i] + 8) >> 24 & 1) ? K_FOUND : K_LEFT, __ATOMIC_RELAXED);
            __atomic_store_n(&ready, have > 0, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&ready, 0, __ATOMIC_RELAXED);
            if (!g_settings.korok_radar) searched = 0;
        }
        pthread_mutex_lock(&lock);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        long long ns = until.tv_nsec + KOROK_POLL_MS * 1000000LL;
        until.tv_sec += (time_t)(ns / 1000000000LL), until.tv_nsec = (long)(ns % 1000000000LL);
        while (!stop_req && pthread_cond_timedwait(&wake, &lock, &until) != ETIMEDOUT) {}
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

// The game thread, every frame in play (runtime/items.c rt_item_tick)
void korok_radar_play_tick(void) { __atomic_store_n(&play_ms, now_ms(), __ATOMIC_RELAXED); }

// Link's position, the copy at PlayerInfo +0x88 the game rewrites every frame (runtime/control_cells.c)
static bool link_pos(float p[3]) {
    uint64_t pinfo = g_site.player_info ? rd64(g_site.player_info) : 0, a, e;
    if (!pinfo || !plat_committed_range(pinfo + 0x88, pinfo + 0x94, &a, &e) || a != pinfo + 0x88) return false;
    memcpy(p, (const void *)(uintptr_t)(pinfo + 0x88), 12);
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

bool korok_radar_shown(void) {
    return g_settings.korok_radar && __atomic_load_n(&ready, __ATOMIC_ACQUIRE) && !game_place_inside() &&
           now_ms() - __atomic_load_n(&play_ms, __ATOMIC_RELAXED) < PLAY_FRESH_MS;
}

bool control_camera_angles(double *yaw, double *pitch);  // runtime/control_game.c

int korok_radar_blips(float range, KorokBlip *out, int max, KorokBlip *nearest, int *found, int *total) {
    float me[3];
    double yaw, pitch;
    if (!korok_radar_shown() || !link_pos(me) || !control_camera_angles(&yaw, &pitch)) return -1;
    // The camera's forward on the ground (yaw 0 looks along +z, 90 along +x) and its right, forward x up
    double y = yaw * (M_PI / 180), fx = sin(y), fz = cos(y), rx = -fz, rz = fx;
    int n = 0, nfound = 0;
    nearest->dist = -1;
    for (int i = 0; i < KOROKS; i++) {
        uint8_t s = __atomic_load_n(&state[i], __ATOMIC_RELAXED);
        if (s == K_FOUND) nfound++;
        if (s != K_LEFT) continue;
        float dx = pos[i][0] - me[0], dz = pos[i][2] - me[2];
        KorokBlip b = {(float)(dx * rx + dz * rz), (float)(dx * fx + dz * fz), pos[i][1] - me[1], sqrtf(dx * dx + dz * dz)};
        if (nearest->dist < 0 || b.dist < nearest->dist) *nearest = b;
        if (b.dist <= range && n < max) out[n++] = b;
    }
    *found = nfound, *total = KOROKS;
    return n;
}

// ---- `koroks`: what the radar knows (developer mode; it only reads)
static void cmd_koroks(int argc, char **argv, ControlReply *r) {
    (void)argc, (void)argv;
    if (!have_data) { control_fail(r, "No Korok table (tools/gen_places.py)"); return; }
    int found = 0, left = 0, nearest = -1;
    float me[3], best = 0;
    bool has_me = link_pos(me);
    for (int i = 0; i < KOROKS; i++) {
        uint8_t s = __atomic_load_n(&state[i], __ATOMIC_RELAXED);
        found += s == K_FOUND, left += s == K_LEFT;
        if (s != K_LEFT || !has_me) continue;
        float dx = pos[i][0] - me[0], dz = pos[i][2] - me[2], d = sqrtf(dx * dx + dz * dz);
        if (nearest < 0 || d < best) nearest = i, best = d;
    }
    control_field_bool(r, "on", g_settings.korok_radar);
    control_field_bool(r, "shown", korok_radar_shown());
    control_field_int(r, "found", found);
    control_field_int(r, "left", left);
    control_field_int(r, "total", KOROKS);
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        control_text(r, g_settings.korok_radar ? "Korok radar: the flags aren't read yet (load a game)" : "Korok radar is off (set korok_radar 1)");
        return;
    }
    control_text(r, "Koroks found: %d of %d (%d left, %d unknown)", found, KOROKS, left, KOROKS - found - left);
    if (nearest >= 0) {
        control_field_str(r, "nearest", ids[nearest].key);
        control_field_num(r, "nearest_m", best);
        control_text(r, "Nearest not found: %s, %.0f m away (%.1f %.1f %.1f)", ids[nearest].key, best, pos[nearest][0],
                     pos[nearest][1], pos[nearest][2]);
    }
}

// (runtime/control_game.c: developer mode only, with the other cheats)
void korok_radar_control_init(void) {
    static const ControlCmd cmds[] = {
        {"koroks", "", "Korok radar: how many Koroks are found, and the nearest one not found yet", cmd_koroks, 0},
    };
    control_register(cmds, 1);
}

// (runtime/main.c boot: each game start)
void korok_radar_start(void) {
    if (running) return;
    if (!have_data && !(have_data = load_data())) LOG("[cheats] korok radar: the Korok tables don't match, no radar\n");
    stop_req = false;
    if (pthread_create(&thread, NULL, radar_thread, NULL) == 0) running = true;
}

// Reboot: the thread is gone before the guest memory is reset under it; the next boot starts it again
RT_ON_RESET(korok_radar_reset) {
    if (running) {
        pthread_mutex_lock(&lock);
        __atomic_store_n(&stop_req, true, __ATOMIC_RELAXED);
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
        pthread_join(thread, NULL);
        running = false;
    }
    vt_bool = 0;
    memset(obj, 0, sizeof obj);
    memset(state, 0, sizeof state);
    __atomic_store_n(&ready, 0, __ATOMIC_RELAXED);
}
