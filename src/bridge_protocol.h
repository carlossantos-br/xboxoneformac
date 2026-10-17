/* xboxoneformac — file protocol shared by the bridge service and the SDL
 * driver that runs inside Wine. Each record is written with a sequence number
 * at both ends; a reader only accepts a record whose two copies match. */
#ifndef XBOXONE_BRIDGE_PROTOCOL_H
#define XBOXONE_BRIDGE_PROTOCOL_H

#include <stdint.h>
#include <time.h>

#define XB_MAGIC 0x58424231u /* "XBB1" */
#define XB_DIRECTORY "/Library/Application Support/XboxOneForMac"
#define XB_STATE_PATH XB_DIRECTORY "/state.bin"
#define XB_RUMBLE_PATH XB_DIRECTORY "/rumble.bin"

/* Written by the service (root), read by Wine. */
typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint32_t magic;
    uint32_t connected;
    int16_t axes[6]; /* left x/y, right x/y, left/right trigger (full range) */
    uint16_t buttons; /* SDL_GameControllerButton bit mask, 15 buttons */
    uint16_t reserved;
    uint8_t padding[16];
    uint64_t sequence_end;
} XBState;

/* Written by Wine (the player's user), read by the service. */
typedef struct {
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint16_t low, high, left_trigger, right_trigger;
    uint64_t sequence_end;
} XBRumble;

static inline uint64_t xb_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#endif
