/*
 * scan.h
 *
 * Looking for a hotspot on the local network, for when its address has
 * changed (a DHCP lease that moved, a router that was swapped...).
 *
 * It tries every host number 1-254 in one /24 - "10.0.0.1" to "10.0.0.254"
 * for the prefix "10.0.0." - with the same non-blocking HTTP client as the
 * rest of the program, asking each for the dashboard's radio status page.
 * A host counts as a hotspot only if the answer is a page the parsers
 * recognise, which is also exactly what the program needs from it.
 *
 * It is a state machine like the HTTP client: scan_step() does whatever can
 * be done without waiting and is called from the Wimp poll loop. About a
 * dozen hosts are tried at a time, each giving up after a second or so, so a
 * whole subnet takes a few seconds to perhaps twenty.
 */

#ifndef HS_SCAN_H
#define HS_SCAN_H

#include <stddef.h>

#define SCAN_FIRST_HOST   1
#define SCAN_LAST_HOST    254
#define SCAN_HOSTS        (SCAN_LAST_HOST - SCAN_FIRST_HOST + 1)

typedef struct hs_scan hs_scan;

/* Turns what a person typed ("10.0.0.27", "10.0.0", "10.0.0.") into the
 * prefix to search ("10.0.0."). Returns 0 if it is not the start of a dotted
 * quad address. */
int scan_prefix_from(const char *text, char *out, size_t cap);

/* Starts a search of `prefix` on `port`. NULL if the prefix is invalid or
 * out of memory. */
hs_scan *scan_new(const char *prefix, int port);

/* Pumps the search; returns 1 once every host has been tried. */
int scan_step(hs_scan *s);

void scan_progress(const hs_scan *s, int *done, int *total, int *found);

/* Hotspots found so far: their address and a short description (the modem
 * type or frequency). Returns 0 if `i` is out of range. */
int scan_result(const hs_scan *s, int i, char *addr, size_t acap, char *info,
                size_t icap);

void scan_free(hs_scan *s);

#endif /* HS_SCAN_H */
