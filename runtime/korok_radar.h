// Cheats > Korok radar (runtime/korok_radar.c): the Koroks Link hasn't found yet around him, for the overlay to draw.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float right, ahead;  // metres from Link, turned with the camera: right of and ahead of where it looks
    float up;            // metres above Link (below: negative)
    float dist;          // metres from Link on the ground (x and z)
} KorokBlip;

// Whether the radar shows now: the option is on, the flags are read, and Link is on the main field in play (not in a
// menu, a shrine or the title)
bool korok_radar_shown(void);

// The Koroks not found yet within range metres, at most max of them, and the nearest one wherever it is (dist < 0: none
// left). found and total: how many Koroks Link has found, of how many. -1: nothing to show (korok_radar_shown false).
int korok_radar_blips(float range, KorokBlip *out, int max, KorokBlip *nearest, int *found, int *total);

#ifdef __cplusplus
}
#endif
