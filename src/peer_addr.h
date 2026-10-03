// peer_addr.h — the hostname a ws:// or wss:// address will really reach.
//
// The allowlist check compares this hostname with the manifest's grants, and
// the connection itself is made by node's WebSocket, which parses the address
// as a WHATWG URL. The two must agree on the host, or a cart can name an
// allowed host and connect to another one. They didn't: the check took
// everything up to the first ':' as the hostname, so in
// "ws://allowed.com:1@evil.com/" it saw allowed.com, while the URL parser
// reads "allowed.com:1" as a username and password and connects to evil.com.
//
// So this refuses anything it can't read the same way a URL parser would:
// userinfo ('@'), whitespace and control characters (which WHATWG strips or
// rejects), and backslashes (a path separator for ws/wss in WHATWG URLs). An
// address it refuses is denied. No SDL, no V8: test/peer_addr_test.c.

#ifndef WC_PEER_ADDR_H
#define WC_PEER_ADDR_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Writes the hostname of addr into out (NUL-terminated) and returns true,
// or returns false for an address that isn't a plain ws:// or wss:// URL.
static inline bool peer_addr_hostname(const char* addr, char* out, size_t out_size) {
    const char* rest;
    if (strncmp(addr, "ws://", 5) == 0)       rest = addr + 5;
    else if (strncmp(addr, "wss://", 6) == 0) rest = addr + 6;
    else return false;

    for (const char* c = addr; *c; c++)
        if ((unsigned char)*c <= ' ' || *c == 127 || *c == '\\') return false;

    // The authority runs to the path, query or fragment.
    size_t auth = strcspn(rest, "/?#");
    if (memchr(rest, '@', auth)) return false;  // userinfo: never in a grant

    // Hostname, then an optional :port.
    size_t n = 0;
    while (n < auth && rest[n] != ':') n++;
    if (n == 0 || n >= out_size) return false;
    memcpy(out, rest, n);
    out[n] = '\0';
    return true;
}

#endif
