/*
 * bada-tide, keyboard side (HHKB Hybrid, HHKB_TIDE) (bada-tide v1, team-mirae/bada docs/bada-tide-v1.md). Portable: no hardware access, compiled for the keyboard and for
 * host tests. The caller passes the time in milliseconds.
 *
 * Stateless by design: nothing here is ever written to flash or EEPROM. A harness holds a lease that it
 * starts with hello and renews with every state message; when it stops (crash, quit, unplug) the lease
 * runs out and the keyboard behaves as if tide never happened. A reset or power cycle clears everything.
 *
 *   AA AA F1 01 02 <major> <minor>          hello: start or renew the lease
 *       reply 55 55 F1 <st> 01 06 <major> <minor> <features> <lease ms LE16> <reserved 0>
 *       st 0 = ok; 1 = unsupported major (the reply still carries ours); 4 = blocked (another program
 *       used the vendor channel in the last TIDE_BLOCK_MS)
 *   AA AA F1 02 04 <mode> <profile> <caps> <take>  state: the harness's full current state; renews the lease
 *       (len 3, without <take>, takes nothing)
 *       reply 55 55 F1 <st> 02 06 <mode> <profile> <caps> <take> <remaining ms LE16>
 *       st 0 = ok; 2 = no lease (send hello first); 3 = bad value
 *   AA AA F1 03 00                          status: read-only, does not renew
 *       reply 55 55 F1 00 03 07 <active> <remaining ms LE16> <mode> <profile> <caps> <take>
 *   55 55 F1 00 10 09 <usage> <edge> <mods> <seq> <time ms LE32> <flags>
 *       key event, sent by the keyboard unasked while a lease holds: a language key (or Caps Lock) went down
 *       (edge 1) or up (edge 0), as sent after the keymap; mods = the report's modifier byte; seq counts
 *       from 0 after each hello; flags bit 0 = events were dropped before this one
 * Backstop: any other vendor command (the HHKB Keymap Tool's, a flasher's) ends the lease at once and
 * hello answers status 4 (blocked) for TIDE_BLOCK_MS after the last such command.
 * mode: 0 = muhenkan, 1 = henkan. profile: index into the harness's cycle, 0xFF = none. caps: bit 0 = on.
 * take: the language keys the harness takes while the lease holds: they are left out of the USB keyboard
 * reports and reach the harness only as key events. Bit 0 半角/全角, 1 無変換, 2 変換, 3 カタカナ, 4 英数,
 * 5 かな, 6 Caps Lock; bit 7 must be 0. When the lease ends nothing is taken.
 * Any other sub-command: status 1. Replies are always 64 bytes.
 */
#include "config.h"

#if defined(HHKB_HYBRID) && HHKB_PFU_CHANNEL && HHKB_TIDE

#include <stdint.h>
#include <string.h>

#include "hhkb_tide.h"

static uint32_t s_until;   /* lease end, ms */
static uint8_t s_held;     /* a lease was granted since boot (s_until is meaningful) */
static uint8_t s_mode, s_profile = 0xFF, s_caps, s_take;
static uint32_t s_block_until; /* backstop: hello is refused until then */
static uint8_t s_blocked;

/* Key events waiting to be sent (RAM only, cleared by hello and when the lease ends). */
typedef struct { uint8_t usage, edge, mods; uint32_t time; } tide_event_t;
static tide_event_t s_q[TIDE_QUEUE];
static uint8_t s_q_head, s_q_count, s_seq, s_dropped;

static void clear_queue(void) {
    s_q_head = s_q_count = 0;
    s_seq = 0;
    s_dropped = 0;
}

static uint32_t remaining(uint32_t now) {
    if (!s_held) return 0;
    int32_t left = (int32_t)(s_until - now);
    if (left > 0) return (uint32_t)left;
    s_held = 0; /* forget it, so the 32-bit clock can't make an old lease look live weeks later */
    return 0;
}

int hhkb_tide_active(uint32_t now_ms) { return remaining(now_ms) > 0; }

void hhkb_tide_state(uint32_t now_ms, uint8_t *mode, uint8_t *profile, uint8_t *caps) {
    int on = hhkb_tide_active(now_ms);
    if (mode) *mode = on ? s_mode : 0;
    if (profile) *profile = on ? s_profile : 0xFF;
    if (caps) *caps = on ? s_caps : 0;
}

static int blocked(uint32_t now) {
    if (s_blocked && (int32_t)(s_block_until - now) > 0) return 1;
    s_blocked = 0;
    return 0;
}

static void renew(uint32_t now) {
    s_until = now + TIDE_LEASE_MS;
    s_held = 1;
}

int hhkb_tide_handle(const uint8_t *req, uint8_t *reply, uint32_t now) {
    if (req[0] != 0xAA || req[1] != 0xAA || req[2] != 0xF1) return 0;
    const uint8_t sub = req[3], len = req[4];
    memset(reply, 0, 64);
    reply[0] = 0x55;
    reply[1] = 0x55;
    reply[2] = 0xF1;
    reply[4] = sub;
    uint8_t *out = reply + 6, st = 1, n = 0;

    if (!hhkb_tide_active(now)) { /* an expired lease forgets the harness's state and its events */
        s_mode = 0;
        s_profile = 0xFF;
        s_caps = 0;
        s_take = 0;
        clear_queue();
    }
    if (sub == 0x01 && len == 2) {
        st = req[5] != TIDE_MAJOR ? 1 : blocked(now) ? 4 : 0;
        if (st == 0) {
            renew(now);
            clear_queue();
        }
        out[0] = TIDE_MAJOR;
        out[1] = TIDE_MINOR;
        out[2] = TIDE_FEATURES;
        out[3] = (uint8_t)TIDE_LEASE_MS;
        out[4] = (uint8_t)(TIDE_LEASE_MS >> 8);
        n = 6;
    } else if (sub == 0x02 && (len == 3 || len == 4)) {
        uint8_t take = len == 4 ? req[8] : 0;
        if (!hhkb_tide_active(now)) {
            st = 2;
        } else if (req[5] > 1 || (req[7] & ~1U) || (take & 0x80)) {
            st = 3;
        } else {
            s_mode = req[5];
            s_profile = req[6];
            s_caps = req[7];
            s_take = take;
            renew(now);
            st = 0;
        }
        uint32_t left = remaining(now);
        out[0] = s_mode;
        out[1] = s_profile;
        out[2] = s_caps;
        out[3] = s_take;
        out[4] = (uint8_t)left;
        out[5] = (uint8_t)(left >> 8);
        n = 6;
    } else if (sub == 0x03 && len == 0) {
        uint32_t left = remaining(now);
        st = 0;
        out[0] = left > 0;
        out[1] = (uint8_t)left;
        out[2] = (uint8_t)(left >> 8);
        out[3] = s_mode;
        out[4] = s_profile;
        out[5] = s_caps;
        out[6] = s_take;
        n = 7;
    }
    reply[3] = st;
    reply[5] = n;
    return 1;
}

/* Language keys and Caps Lock, as HID usages, in take-bit order: 半角/全角, 無変換, 変換, カタカナ, 英数, かな,
 * Caps Lock. Returns the bit index, or -1. */
static int language_bit(uint16_t usage) {
    static const uint8_t keys[7] = {0x35, 0x8B, 0x8A, 0x88, 0x91, 0x90, 0x39};
    for (int i = 0; i < 7; i++)
        if (usage == keys[i]) return i;
    return -1;
}
static int is_language_key(uint16_t usage) { return language_bit(usage) >= 0; }

int hhkb_tide_taken(uint16_t usage, uint32_t now) {
    int b = language_bit(usage);
    return b >= 0 && hhkb_tide_active(now) && (s_take >> b & 1);
}

void hhkb_tide_key(uint16_t usage, int pressed, uint8_t mods, uint32_t now) {
    if (!hhkb_tide_active(now) || !is_language_key(usage)) return;
    if (s_q_count == TIDE_QUEUE) { /* full: drop the oldest, flag the next one sent */
        s_q_head = (uint8_t)((s_q_head + 1) % TIDE_QUEUE);
        s_q_count--;
        s_dropped = 1;
    }
    tide_event_t *e = &s_q[(s_q_head + s_q_count) % TIDE_QUEUE];
    e->usage = (uint8_t)usage;
    e->edge = pressed ? 1 : 0;
    e->mods = mods;
    e->time = now;
    s_q_count++;
}

int hhkb_tide_next_event(uint8_t *report, uint32_t now) {
    if (!hhkb_tide_active(now)) {
        clear_queue();
        return 0;
    }
    if (!s_q_count) return 0;
    const tide_event_t *e = &s_q[s_q_head];
    memset(report, 0, 64);
    report[0] = 0x55;
    report[1] = 0x55;
    report[2] = 0xF1;
    report[4] = 0x10;
    report[5] = 9;
    report[6] = e->usage;
    report[7] = e->edge;
    report[8] = e->mods;
    report[9] = s_seq++;
    report[10] = (uint8_t)e->time;
    report[11] = (uint8_t)(e->time >> 8);
    report[12] = (uint8_t)(e->time >> 16);
    report[13] = (uint8_t)(e->time >> 24);
    report[14] = s_dropped;
    s_dropped = 0;
    s_q_head = (uint8_t)((s_q_head + 1) % TIDE_QUEUE);
    s_q_count--;
    return 1;
}

void hhkb_tide_other_command(uint32_t now) {
    s_held = 0; /* the lease ends now */
    s_mode = 0;
    s_profile = 0xFF;
    s_caps = 0;
    s_take = 0;
    clear_queue();
    s_block_until = now + TIDE_BLOCK_MS;
    s_blocked = 1;
}

#endif
