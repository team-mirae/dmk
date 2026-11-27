/* bada-tide's keyboard side (hhkb_tide.c) on a fake clock: lease, key events, take, backstop. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hhkb_tide.h"

static uint8_t rep[64];

static int req(uint32_t now, const char *hex) {
    uint8_t r[64] = {0};
    for (size_t i = 0; hex[2 * i]; i++) sscanf(hex + 2 * i, "%2hhx", &r[i]);
    return hhkb_tide_handle(r, rep, now);
}
static int ev(uint32_t now) { return hhkb_tide_next_event(rep, now); }
static int state(uint32_t now, unsigned mode, unsigned profile, unsigned caps, unsigned take) {
    char hex[32];
    snprintf(hex, sizeof hex, "aaaaf10204%02x%02x%02x%02x", mode, profile, caps, take);
    return req(now, hex);
}

int main(void) {
    const char *hello = "aaaaf101020100", *status = "aaaaf10300";
    assert(req(0, "aaaa020000") == 0);                                   /* not tide's */
    req(1000, status);
    assert(rep[3] == 0 && rep[6] == 0);                                  /* no lease at boot */
    state(1000, 1, 0, 0, 0);
    assert(rep[3] == 2);                                                 /* state before hello */
    req(1000, hello);
    assert(rep[3] == 0 && rep[6] == 1 && rep[7] == 0 && rep[8] == TIDE_FEATURES && (rep[9] | rep[10] << 8) == 3000);
    req(1000, "aaaaf101020200");
    assert(rep[3] == 1 && rep[6] == 1);                                  /* another major */
    state(1500, 1, 2, 1, 0);
    assert(rep[3] == 0 && rep[6] == 1 && rep[7] == 2 && rep[8] == 1 && (rep[10] | rep[11] << 8) == 3000);
    state(1500, 2, 0, 0, 0);
    assert(rep[3] == 3);                                                 /* bad mode */
    req(4499, status);
    assert(rep[6] == 1);
    req(4500, status);
    assert(rep[6] == 0 && rep[9] == 0 && rep[10] == 0xFF);               /* expired, forgotten */

    req(10000, hello);                                                   /* key events */
    hhkb_tide_key(0x88, 1, 0x02, 10010);
    hhkb_tide_key(0x04, 1, 0, 10011);                                    /* a letter */
    hhkb_tide_key(0x88, 0, 0x02, 10050);
    assert(ev(10060) && rep[4] == 0x10 && rep[6] == 0x88 && rep[7] == 1 && rep[8] == 0x02 && rep[9] == 0);
    assert(ev(10060) && rep[6] == 0x88 && rep[7] == 0 && rep[9] == 1);
    assert(!ev(10060));
    for (int i = 0; i < 8; i++) hhkb_tide_key(0x90, i & 1, 0, 10200 + i);
    assert(ev(10300) && rep[14] == 1);                                   /* oldest dropped, flagged */
    for (int i = 0; i < 5; i++) assert(ev(10300) && rep[14] == 0);
    assert(!ev(10300));

    state(10400, 0, 0xFF, 0, 0x7F);                                      /* take */
    assert(hhkb_tide_taken(0x90, 10401) && hhkb_tide_taken(0x39, 10401) && !hhkb_tide_taken(0x04, 10401));
    state(10402, 0, 0xFF, 0, 0x80);
    assert(rep[3] == 3 && hhkb_tide_taken(0x90, 10402));                 /* bit 7: refused, kept */
    assert(!hhkb_tide_taken(0x90, 13403));                               /* lease over: nothing taken */

    req(20000, hello);                                                   /* backstop */
    state(20001, 0, 0xFF, 0, 0x7F);
    hhkb_tide_other_command(20010);
    assert(!hhkb_tide_taken(0x90, 20010));
    req(20020, hello);
    assert(rep[3] == 4);
    req(30010, hello);
    assert(rep[3] == 0);

    req(33100, status);                                                  /* the 32-bit clock */
    req(33010 + 0x80000100u, status);
    assert(rep[6] == 0);
    printf("bada-tide keyboard side tests passed\n");
    return 0;
}
