#ifndef NSD_NETTUNE_H
#define NSD_NETTUNE_H
/* The console's TCP receive window is tiny by default (about 8 KiB), so a download from a far away server crawls: speed is
 * window / round-trip time, 25-30 KiB/s to a server 200 ms away, however fast the Wi-Fi is. Measured on a real console against
 * an HTTP server on the internet (KiB/s for 1 / 2 / 4 / 8 connections):
 *   default 29 / 57 / ~110 / ~110    16 KiB 57 / 106 / 174 / 287    32 KiB 115 / 204 / 316 / 456
 *   48 KiB 147 / 290 / 474 / 733     56 KiB (with 4 MiB of socket memory) 146 / 346 / 518 / 765
 * and 64 KiB or more does not work at all (no byte arrives). Every curl handle of the app asks for 56 KiB. */
#include <curl/curl.h>
#include <sys/socket.h>

#define NSD_RCVBUF (56 * 1024)

static inline int nsd_sockopt(void *p, curl_socket_t fd, curlsocktype purpose) {
  int size = NSD_RCVBUF;
  (void)p;
  (void)purpose;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
  return CURL_SOCKOPT_OK;
}
#endif
