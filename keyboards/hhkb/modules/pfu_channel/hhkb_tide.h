/* bada-tide, keyboard side: see hhkb_tide.c and bada-tide v1. */
#ifndef HHKB_TIDE_H
#define HHKB_TIDE_H

#include <stdint.h>

#define TIDE_MAJOR 1
#define TIDE_MINOR 0
#define TIDE_FEATURES 0x11 /* bit 0 key events, bit 4 take; later: bit 1 tap-hold, bit 2 indicator, bit 3 layer */
#define TIDE_LEASE_MS 3000U
#define TIDE_BLOCK_MS 10000U /* backstop: hello refused this long after another program used the channel */
#define TIDE_QUEUE 6

/* Handles one vendor request if it is a tide command (AA AA F1 ...): fills a 64-byte reply, returns 1. */
int hhkb_tide_handle(const uint8_t *req, uint8_t *reply, uint32_t now_ms);
/* 1 while a harness holds the lease. */
int hhkb_tide_active(uint32_t now_ms);
/* The harness's last state (reset to muhenkan / no profile / caps off when the lease ends). */
void hhkb_tide_state(uint32_t now_ms, uint8_t *mode, uint8_t *profile, uint8_t *caps);

/* A key the keyboard sent (dmk key_sent hook): queued if it is a language key and a lease holds. */
void hhkb_tide_key(uint16_t usage, int pressed, uint8_t mods, uint32_t now_ms);
/* The next key event as a 64-byte report; 0 if there is none (or no lease). */
int hhkb_tide_next_event(uint8_t *report, uint32_t now_ms);
/* 1 if a lease holds and the harness takes this usage: leave it out of the USB keyboard reports. */
int hhkb_tide_taken(uint16_t usage, uint32_t now_ms);
/* Another vendor command arrived (keymap tool, CLI): end the lease and refuse hello for TIDE_BLOCK_MS. */
void hhkb_tide_other_command(uint32_t now_ms);

#endif
