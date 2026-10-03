/*
 * textauto -- text-input fixture for test/text_test.c.
 *
 * The same cart as wasmcart's test/fixtures/textcart.c, except that it turns
 * text input on by itself during its first wc_render. text_test drives the C
 * host only through wc_host_push_text / wc_host_run_frame and cannot call the
 * cart's wc_begin export, so the cart has to enable input itself. Text pushed
 * before that first frame must be dropped, which is what text_test checks.
 *
 * Rebuild (emcc + the wasmcart repo checkout for wasmcart.h / wc_cart.h):
 *   emcc test/textauto.c -O2 -I../wasmcart/include -s STANDALONE_WASM=1 --no-entry \
 *     -s EXPORTED_FUNCTIONS='["_wc_init","_wc_render","_wc_get_info","_wc_debug_state","_wc_on_text","_wc_begin","_wc_end","_malloc","_free"]' \
 *     -s ERROR_ON_UNDEFINED_SYMBOLS=0 -o textauto.wasm
 *   npx wasmcart-pack --wasm textauto.wasm --name textauto -o test/textauto.wasc
 * then read the new debug-field offsets with wasmcart's readDebugState() and
 * update the text_test line in README.md.
 */
#include "wasmcart.h"
#include "wc_cart.h"

_Static_assert(WC_ABI_VERSION == 4, "textauto expects wasmcart ABI v4");

#define WIDTH  32
#define HEIGHT 32
#define BUF_MAX 256

static uint32_t framebuffer[WIDTH * HEIGHT];
static wc_pad_t pads[4];
static wc_time_t time_info;
static wc_info_t info;
static wc_host_info_t host_info;

/* Received text, accumulated across calls. */
static uint8_t buf[BUF_MAX];
static uint32_t buf_len;
static uint32_t call_count;
static uint32_t active;

WC_DEBUG_FIELDS(
    WC_DBG("buf_len",    buf_len,    WC_DBG_U32),
    WC_DBG("call_count", call_count, WC_DBG_U32),
    WC_DBG("active",     active,     WC_DBG_U32),
    WC_DBG("buf",        buf[0],     WC_DBG_U32)
)

__attribute__((export_name("wc_get_info")))
wc_info_t* wc_get_info(void) {
    info.version = WC_ABI_VERSION;
    info.width = WIDTH;
    info.height = HEIGHT;
    info.fb_ptr = (uint32_t)framebuffer;
    info.audio_ptr = 0;
    info.audio_cap = 0;
    info.audio_write_ptr = 0;
    info.input_ptr = (uint32_t)pads;
    info.save_ptr = 0;
    info.save_size = 0;
    info.time_ptr = (uint32_t)&time_info;
    info.host_info_ptr = (uint32_t)&host_info;
    info.flags = WC_FLAG_DEBUG;
    return &info;
}

__attribute__((export_name("wc_init")))
void wc_init(void) { }

/* Test hooks so the harness can drive begin/end from outside. */
__attribute__((export_name("wc_begin")))
void wc_begin(void) { wc_text_input_begin(); active = wc_text_input_active(); }

__attribute__((export_name("wc_end")))
void wc_end(void) { wc_text_input_end(); active = wc_text_input_active(); }

__attribute__((export_name("wc_on_text")))
void wc_on_text(const char* utf8, uint32_t len) {
    call_count++;
    /* Copy immediately: the pointer is only valid for this call. */
    for (uint32_t i = 0; i < len && buf_len < BUF_MAX; i++) {
        buf[buf_len++] = (uint8_t)utf8[i];
    }
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
    static int started;
    if (!started) { started = 1; wc_begin(); }
    for (int i = 0; i < WIDTH * HEIGHT; i++) framebuffer[i] = 0x00202020;
}
