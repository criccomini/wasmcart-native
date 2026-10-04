/*
 * peer_test — the wc_peer_* family through the C host, against a real server.
 *
 * Two things are being checked, and the second matters more:
 *
 *   1. A granted cart can dial out, gets wc_peer_on_connect, sends bytes and
 *      receives them back through wc_peer_on_message.
 *   2. An UNGRANTED cart is refused -- against the same server the granted cart
 *      just used, so the refusal cannot be the server being down. A network
 *      security test against a dead port passes for entirely the wrong reason.
 *
 * Also covers host-supplied peers (wc_host_add_peer), which deliberately need
 * NO manifest grant: the host already chose that peer, so requiring it to also
 * write a manifest key permitting its own action would be ceremony.
 *
 * Build (needs a built libwasmcart.a and libnode):
 *   gcc -O0 -o peer_test test/peer_test.c -Iinclude -Isrc \
 *       build/libwasmcart.a deps/libnode/libnode.a -lstdc++ -lm -lpthread -ldl
 *
 * Run (server from the wasmcart repo):
 *   node ../wasmcart/test/wsserver.mjs --port 8796 &
 *   ./peer_test 8796 ../wasmcart/test/fixtures/peernet_net.wasc \
 *                    ../wasmcart/test/fixtures/peernet.wasc
 */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define nap_ms(n) Sleep(n)
#else
#include <unistd.h>
#define nap_ms(n) usleep((n) * 1000)
#endif
#include "wasmcart_host.h"

int32_t wc_test_call_export(wc_host_t*, const char*, uint32_t, uint32_t, uint32_t, int);
int     wc_test_poke(wc_host_t*, uint32_t, const void*, uint32_t);

static int failures = 0;

static void check(const char* what, long got, long want) {
    if (got == want) printf("  ok    %-44s %ld\n", what, got);
    else { printf("*** FAIL %-44s got %ld, want %ld\n", what, got, want); failures++; }
}

/* Run frames until `probe` reports non-zero, or we give up. 10 ms apart, as
 * a player's frames would be: this cart's render does nothing, so back to
 * back the 300 frames were over in a few milliseconds, and on a busy machine
 * the local WebSocket hadn't opened by then (2 runs in 8 failed). */
static int pump_until(wc_host_t* h, const char* probe, int frames) {
    for (int i = 0; i < frames; i++) {
        wc_host_run_frame(h);
        if (wc_test_call_export(h, probe, 0, 0, 0, 0) > 0) return 1;
        nap_ms(10);
    }
    return 0;
}

static wc_host_t* load(const char* path) {
    wc_host_t* h = wc_host_create();
    wc_host_options_t o; memset(&o, 0, sizeof o);
    if (wc_host_load_file(h, path, &o) != 0) return NULL;
    return h;
}

/* A host-supplied peer's send callback just records that it was called. */
static int sent_bytes = 0;
static int fake_send(void* user, const uint8_t* data, uint32_t len) {
    (void)user; (void)data;
    sent_bytes += (int)len;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <granted.wasc> <ungranted.wasc>\n", argv[0]);
        return 2;
    }
    const char* port = argv[1];

    /* ── 1. granted cart dials out ─────────────────────────────────────── */
    /* create() BEFORE enter_v8(): create() is what initialises V8, and calling
     * enter_v8() first segfaults before main() prints anything. I documented
     * that trap in text_test.c and then walked straight into it here. */
    wc_host_t* h = load(argv[2]);
    if (!h) { printf("load failed: %s\n", argv[2]); return 1; }
    wc_host_enter_v8();

    char addr[256];
    snprintf(addr, sizeof addr, "ws://127.0.0.1:%s/echo", port);
    uint32_t scratch = (uint32_t)wc_test_call_export(h, "t_scratch", 0, 0, 0, 0);
    wc_test_poke(h, scratch, addr, (uint32_t)strlen(addr));
    int32_t id = wc_test_call_export(h, "t_open", scratch, (uint32_t)strlen(addr), 0, 2);
    check("granted cart: wc_peer_open returns an id", id >= 0, 1);

    check("wc_peer_on_connect fires", pump_until(h, "t_connects", 300), 1);
    check("peer_count is 1", wc_test_call_export(h, "t_count", 0, 0, 0, 0), 1);

    const char* msg = "native-peer-hello";
    wc_test_poke(h, scratch, msg, (uint32_t)strlen(msg));
    wc_test_call_export(h, "t_send", (uint32_t)id, scratch, (uint32_t)strlen(msg), 3);
    check("wc_peer_on_message fires", pump_until(h, "t_messages", 300), 1);
    check("echoed byte count", wc_test_call_export(h, "t_last_msg_len", 0, 0, 0, 0),
          (long)strlen(msg));
    wc_host_destroy(h);

    /* ── 2. ungranted cart is refused, same live server ────────────────── */
    h = load(argv[3]);
    if (!h) { printf("load failed: %s\n", argv[3]); return 1; }
    scratch = (uint32_t)wc_test_call_export(h, "t_scratch", 0, 0, 0, 0);
    wc_test_poke(h, scratch, addr, (uint32_t)strlen(addr));
    int32_t denied = wc_test_call_export(h, "t_open", scratch, (uint32_t)strlen(addr), 0, 2);
    check("ungranted cart refused (-1)", denied, -1);
    wc_host_destroy(h);

    /* ── 3. host-supplied peer needs no grant ──────────────────────────── */
    h = load(argv[3]);            /* the UNGRANTED cart on purpose */
    if (!h) { printf("load failed\n"); return 1; }
    int32_t hp = wc_host_add_peer(h, "host-chosen", fake_send, NULL,
                                  WC_TRANSPORT_RELIABLE);
    check("host-supplied peer registers without a grant", hp >= 0, 1);
    check("cart sees the connect", pump_until(h, "t_connects", 60), 1);
    check("peer_count is 1", wc_test_call_export(h, "t_count", 0, 0, 0, 0), 1);

    /* inbound bytes from the embedder */
    const char* in = "from-the-host";
    wc_host_peer_recv(h, hp, (const uint8_t*)in, (uint32_t)strlen(in));
    check("host-injected message reaches the cart", pump_until(h, "t_messages", 60), 1);

    /* outbound bytes go to the embedder's callback */
    scratch = (uint32_t)wc_test_call_export(h, "t_scratch", 0, 0, 0, 0);
    wc_test_poke(h, scratch, "abc", 3);
    wc_test_call_export(h, "t_send", (uint32_t)hp, scratch, 3, 3);
    check("cart send reaches the embedder callback", sent_bytes, 3);

    wc_host_remove_peer(h, hp);
    check("cart sees the disconnect", pump_until(h, "t_disconnects", 60), 1);
    wc_host_destroy(h);

    wc_host_exit_v8();
    printf(failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
