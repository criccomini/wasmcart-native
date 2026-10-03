// peer_addr_test.c — the allowlist sees the host the connection goes to.
//
// No V8 or SDL:  cc -Isrc -o peer_addr_test test/peer_addr_test.c

#include "peer_addr.h"
#include <stdio.h>

static int failures = 0;

static void expect(const char* addr, const char* want) {
    char host[256];
    bool ok = peer_addr_hostname(addr, host, sizeof(host));
    if (want == NULL ? ok : (!ok || strcmp(host, want) != 0)) {
        fprintf(stderr, "FAIL: %s -> %s, want %s\n", addr, ok ? host : "(refused)",
                want ? want : "(refused)");
        failures++;
    }
}

int main(void) {
    expect("ws://allowed.com/", "allowed.com");
    expect("wss://allowed.com:443/play?room=1", "allowed.com");
    expect("ws://allowed.com", "allowed.com");
    expect("ws://allowed.com?x=1", "allowed.com");
    // The bypass: a URL parser reads "allowed.com:1" as userinfo here and
    // connects to evil.com.
    expect("ws://allowed.com:1@evil.com/", NULL);
    expect("ws://allowed.com@evil.com/", NULL);
    expect("ws://user:pass@allowed.com/", NULL);
    // Characters a URL parser strips or reinterprets.
    expect("ws://evil.com\\@allowed.com/", NULL);
    expect("ws://allow\ted.com/", NULL);
    expect("ws://allowed.com\n/", NULL);
    expect(" ws://allowed.com/", NULL);
    // Not ws/wss, or no host at all.
    expect("http://allowed.com/", NULL);
    expect("ws://", NULL);
    expect("ws://:80/", NULL);
    // '@' after the authority is part of the path, which is fine.
    expect("ws://allowed.com/rooms/@lobby", "allowed.com");
    if (failures) return 1;
    printf("PASS: the hostname checked is the hostname reached\n");
    return 0;
}
