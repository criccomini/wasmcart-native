// gl_imports.cpp — Register native GL functions as WASM imports (V8 version)
//
// Native GLES uses integer IDs for GL objects — no object tables needed.
// V8 FunctionCallback wrappers instead of wasmtime callbacks.

#include "v8.h"
#include <string>
#include <vector>
extern "C" {
#include "cart_host.h"
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <string.h>
#include <stdio.h>
}
// AFTER the GLES headers: redirect every glXxx() in this file to a pointer loaded
// from the host's get-proc-address (RetroArch's for the libretro core, EGL's for
// the native host). This file then links NO GL library — no ANGLE, single
// self-contained core. (Placed outside the extern "C" header block so the
// #defines don't rewrite the header declarations.)
#include "gl_procs.h"
#include "wc_log.h"

// V8 context accessor — defined in cart_host.cpp
extern v8::Isolate* g_isolate;
extern v8::Local<v8::Context> ctx();

static wc_host_t* _host = NULL;

// FBO redirect state
static GLuint _redirect_fbo = 0;
static GLuint _redirect_tex = 0;
static uint32_t _redirect_w = 0, _redirect_h = 0;
// Cart VAO — isolates cart's VAO 0 usage from host (RetroArch uses VAO 0 for overlays)
static GLuint _cart_vao = 0;
// The VAO the cart most recently bound (already mapped: cart's 0 -> _cart_vao).
// Element-array binding is VAO state, so a host-side VAO switch between frames
// must put THIS back, not _cart_vao: an engine that binds its own VAO once at
// init (wasmcart-lua's render2d_gl) then draws with glDrawElements(..., 0)
// otherwise draws through _cart_vao, which has no element buffer -- Mesa then
// reads 0 as a client pointer and segfaults in memcpy (Batocera/Pi5).
static GLuint _cart_bound_vao = 0;
static GLuint _last_draw_fbo = 0;
static int _cart_blitted_to_redirect = 0;
static uint32_t _cart_blit_w = 0, _cart_blit_h = 0; // actual render size from cart's blit
static int _fbo_log_frames = 0;
static int _draw_call_count = 0;

// 2D framebuffer upload to redirect FBO (unified GL display path)
static GLuint _fb_upload_tex = 0;
static GLuint _fb_upload_program = 0;
static GLuint _fb_upload_vao = 0;

// Filtered extension list — match WebGL2/Node.js host
// Prevents Skia/Ganesh from requiring ES 3.1+ functions not in the WASM GL import table
static const char* _filtered_extensions[] = {
    "GL_EXT_texture_filter_anisotropic",
    "GL_OES_texture_float_linear",
    "GL_EXT_color_buffer_float",
    "GL_EXT_float_blend",
    "GL_OES_packed_depth_stencil",
    "GL_OES_texture_npot",
    "GL_EXT_texture_norm16",
    NULL
};
static const int _num_filtered_extensions = 7;

// ─── Cart pointers ────────────────────────────────────────────────────────
// Every pointer argument is an offset into the cart's memory that the cart
// chose, and GL follows whatever host address it is handed. So each one is
// checked first for the bytes the call will really touch (NEED, which traps
// the cart if they aren't all in its memory) and only then converted.
//
// A 0 pointer stays NULL where GL gives NULL a meaning (no data for
// glBuffer(Sub)Data and the texture uploads, an optional length out-param,
// no array for glVertexAttribPointer); everywhere else it is cart address 0,
// as it is for the cart's own loads and stores. Handing the driver NULL for
// a pointer it must follow crashes the host.

// Trap and return from the GL_REG callback unless [p, p + len) is cart memory.
#define NEED(p, len) do { if (!wc_cart_range_ok(_host, _import, (p), (len))) return; } while (0)
// The same for a NUL-terminated string, which is then copied into the host
// string `var`: GL gets the copy. Checked in place, a threaded cart's other
// threads could take the terminator away before the driver reads it.
#define NEED_STR(var, p) std::string var; do { if (!_cartStr(_import, (p), &var)) return; } while (0)
static bool _cartStr(const char* import, uint32_t p, std::string* out) {
    if (!wc_cart_str_ok(_host, import, p)) return false;
    const char* s = (const char*)_host->memory + p;
    out->assign(s, strnlen(s, _host->memory_size - p));
    return true;
}

// Host address of a cart pointer that has passed NEED.
static inline void* wmem(uint32_t p) {
    return _host->memory + p;
}
// The same, except that 0 stays NULL.
static inline void* wptr(uint32_t p) {
    return p ? (_host->memory + p) : NULL;
}

// Bytes for n items of `each` bytes. GL's counts are signed, and a negative
// one is an error that touches nothing.
static inline uint64_t _nbytes(int32_t n, uint64_t each) {
    return n > 0 ? (uint64_t)n * each : 0;
}

// Which buffer GL has bound at `binding`. With one bound, a pointer argument
// for that target is an offset into that buffer, not a cart pointer at all.
// With none, it is a client pointer, i.e. cart memory. Asked of GL rather
// than tracked from glBindBuffer: deleting a buffer unbinds it, and the
// element array binding changes with every glBindVertexArray.
static GLuint _boundBuffer(GLenum binding) {
    GLint b = 0;
    glGetIntegerv(binding, &b);
    return (GLuint)b;
}

// ─── Client-side vertex array support ────────────────────────────────────
// gl4es uses client-side arrays (no VBO bound). We must track the WASM pointers
// and upload to temp VBOs before draw calls, just like the Node.js host does.

#define MAX_VERTEX_ATTRIBS 16

typedef struct {
    int active;         // has client-side pointer stored
    int size;           // components per vertex (1-4)
    GLenum type;        // GL_FLOAT, GL_SHORT, etc.
    GLboolean normalized;
    int stride;         // >= 0
    uint32_t wasmPtr;   // WASM memory offset
} client_attrib_t;

static client_attrib_t _clientAttribs[MAX_VERTEX_ATTRIBS] = {0};
static GLuint _tempVBOs[MAX_VERTEX_ATTRIBS] = {0};
static GLuint _tempEBO = 0;
static int _numClientAttribs = 0;

static int _bytesForGLType(GLenum type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
        case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
        case GL_HALF_FLOAT: return 2;
        default: return 4;
    }
}

// Bytes one vertex of an attribute takes up: size components of type, except
// the packed types, which hold all four in one 32-bit word.
static uint64_t _attribBytes(const client_attrib_t* a) {
    if (a->type == GL_INT_2_10_10_10_REV || a->type == GL_UNSIGNED_INT_2_10_10_10_REV) return 4;
    return (uint64_t)a->size * _bytesForGLType(a->type);
}

// Where vertices [first, first + count) of a client array lie in cart memory.
// Exact: the last vertex ends at its own last byte, not a whole stride later,
// so an array that ends where the cart's memory ends still fits. With `fit`,
// count is only a guess (see glDrawElements) and is cut short at the end of
// memory instead; just the first vertex has to be in it.
static void _clientRange(const client_attrib_t* a, uint64_t first, uint64_t count, bool fit,
                         uint64_t* start, uint64_t* len) {
    uint64_t elem = _attribBytes(a);
    uint64_t stride = a->stride ? (uint64_t)a->stride : elem;
    *start = a->wasmPtr + first * stride;
    if (fit && *start + elem <= _host->memory_size) {
        uint64_t room = (_host->memory_size - *start - elem) / stride + 1;
        if (count > room) count = room;
    }
    *len = (count - 1) * stride + elem;
}

// Upload vertices [first, first + count) of every client array to its temp
// VBO. Every array is checked before any is uploaded. Returns false if one
// is outside cart memory, in which case the cart has been trapped.
static bool _uploadClientAttribs(const char* import, uint64_t first, uint64_t count, bool fit) {
    if (_numClientAttribs == 0 || count == 0) return true;
    uint64_t start, len;
    for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
        if (!_clientAttribs[i].active) continue;
        _clientRange(&_clientAttribs[i], first, count, fit, &start, &len);
        if (!wc_cart_range_ok(_host, import, start, len)) return false;
    }
    GLuint prev = _boundBuffer(GL_ARRAY_BUFFER_BINDING);
    for (int i = 0; i < MAX_VERTEX_ATTRIBS; i++) {
        client_attrib_t* a = &_clientAttribs[i];
        if (!a->active) continue;
        _clientRange(a, first, count, fit, &start, &len);
        if (!_tempVBOs[i]) glGenBuffers(1, &_tempVBOs[i]);
        glBindBuffer(GL_ARRAY_BUFFER, _tempVBOs[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)len, _host->memory + start, GL_STREAM_DRAW);
        glVertexAttribPointer(i, a->size, a->type, a->normalized, a->stride, 0);
    }
    // Restore user's binding
    glBindBuffer(GL_ARRAY_BUFFER, prev);
    return true;
}

// glVertexAttrib(I)Pointer with no array buffer bound: ptr is a client array
// in cart memory. It is kept here and uploaded at draw time; GL itself is
// never given it, since with no buffer bound GL would take it as a host
// address.
static void _setClientAttrib(uint32_t idx, GLint size, GLenum type, GLboolean normalized,
                             GLsizei stride, uint32_t ptr) {
    if (idx >= MAX_VERTEX_ATTRIBS) {
        // Past what's tracked here, so there's nothing to upload it from.
        static bool warned = false;
        if (!warned) {
            warned = true;
            wc_log("wasmcart: client-side array on attribute %u ignored; only the "
                   "first %d can have one\n", idx, MAX_VERTEX_ATTRIBS);
        }
        return;
    }
    if (size < 1 || size > 4 || stride < 0) {
        // GL's INVALID_VALUE. Let GL raise it; it changes no state.
        glVertexAttribPointer(idx, size, type, normalized, stride, NULL);
        return;
    }
    if (!_clientAttribs[idx].active) _numClientAttribs++;
    _clientAttribs[idx].active = 1;
    _clientAttribs[idx].size = size;
    _clientAttribs[idx].type = type;
    _clientAttribs[idx].normalized = normalized;
    _clientAttribs[idx].stride = stride;
    _clientAttribs[idx].wasmPtr = ptr;
}

static void _clearClientAttrib(uint32_t idx) {
    if (idx < MAX_VERTEX_ATTRIBS && _clientAttribs[idx].active) {
        _clientAttribs[idx].active = 0;
        _numClientAttribs--;
    }
}

static bool _isIndexType(GLenum type) {
    return type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT;
}

// Copy client-side indices (already checked to be in cart memory, or a host
// copy of them) into _tempEBO and leave it bound; draw with offset 0, then
// rebind 0.
static void _uploadClientIndices(const void* indices, uint64_t bytes) {
    if (!_tempEBO) glGenBuffers(1, &_tempEBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _tempEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)bytes, indices, GL_STREAM_DRAW);
}

// Forward declare — defined in cart_host.cpp
extern "C" void wc_refresh_memory(wc_host_t* host);

// ─── V8 FunctionCallback wrapper macro ─────────────────────────────────────
// Each GL function becomes a v8::FunctionCallback that unpacks args and
// calls the real GL function. Same GL bodies as wasmtime version. The body is
// variadic so a comma in it can't split it into extra macro arguments.

#define GL_REG(name, nparams, nresults, ...) \
    static void _cb_##name(const v8::FunctionCallbackInfo<v8::Value>& _args) { \
        v8::Local<v8::Context> _ctx = ctx(); \
        (void)_ctx; \
        const char* _import = #name; \
        (void)_import; \
        wc_refresh_memory(_host); \
        __VA_ARGS__; \
    }

#define A_I32(n) (_args[n]->Int32Value(_ctx).FromJust())
#define A_U32(n) (_args[n]->Uint32Value(_ctx).FromJust())
#define A_F32(n) ((float)_args[n]->NumberValue(_ctx).FromJust())
#define A_F64(n) (_args[n]->NumberValue(_ctx).FromJust())
// i64 arrives from wasm as a BigInt, which IntegerValue() can't convert.
#define A_I64(n) (_args[n]->IsBigInt() ? _args[n].As<v8::BigInt>()->Int64Value() \
                                       : (int64_t)_args[n]->IntegerValue(_ctx).FromJust())
#define R_I32(v) _args.GetReturnValue().Set((int32_t)(v))
#define R_F32(v) _args.GetReturnValue().Set((double)(v))

// ─── glGet* ───────────────────────────────────────────────────────────────
// Most glGet* calls write a pname-dependent number of values. The driver
// writes into a scratch buffer with room to spare, and only the values the
// pname really has are copied out to the cart, so a pname not listed here (a
// desktop-only or extension one) can't make the driver write past the cart's
// buffer. The cart's current values go into the scratch first, so a call GL
// rejects leaves them as they were.
template <typename T, typename F>
static bool _getInto(const char* import, uint32_t p, uint64_t n, F get) {
    if (!wc_cart_range_ok(_host, import, p, n * sizeof(T))) return false;
    T stack[80];
    std::vector<T> heap;
    T* buf = stack;
    if (n + 64 > 80) {
        heap.resize(n + 64);
        buf = heap.data();
    }
    memcpy(buf, _host->memory + p, n * sizeof(T));
    get(buf);
    memcpy(_host->memory + p, buf, n * sizeof(T));
    return true;
}

// How many values glGet{Integer,Float,Boolean,Integer64}v writes for pname:
// one, except for these (ES 3.2's state tables, plus the two desktop GL
// ranges a libretro desktop context answers).
static uint64_t _getCount(GLenum pname) {
    GLint n = 0;
    switch (pname) {
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_WRITEMASK:
    case GL_COLOR_CLEAR_VALUE: case GL_BLEND_COLOR:
        return 4;
    case GL_DEPTH_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE:
    case GL_ALIASED_POINT_SIZE_RANGE: case GL_MAX_VIEWPORT_DIMS:
    case 0x0B12: /* GL_POINT_SIZE_RANGE */ case 0x0B22: /* GL_LINE_WIDTH_RANGE */
        return 2;
    case GL_PRIMITIVE_BOUNDING_BOX:
        return 8;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
        return n > 0 ? (uint64_t)n : 0;
    case GL_PROGRAM_BINARY_FORMATS:
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &n);
        return n > 0 ? (uint64_t)n : 0;
    case GL_SHADER_BINARY_FORMATS:
        glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, &n);
        return n > 0 ? (uint64_t)n : 0;
    default:
        return 1;
    }
}

// ─── Pixel data ───────────────────────────────────────────────────────────

// Sizes computed from cart-chosen dimensions can pass 64 bits. Saturate
// rather than wrap: a wrapped size could look small enough to fit.
static uint64_t _mulSat(uint64_t a, uint64_t b) {
    return (a && b > UINT64_MAX / a) ? UINT64_MAX : a * b;
}
static uint64_t _addSat(uint64_t a, uint64_t b) {
    return a + b < a ? UINT64_MAX : a + b;
}

// Bytes per pixel of client pixel data in format/type, or 0 for a pair this
// doesn't know.
static uint64_t _pixelBytes(GLenum format, GLenum type) {
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    }
    uint64_t comps;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE:
    case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
        comps = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA:
        comps = 2; break;
    case GL_RGB: case GL_RGB_INTEGER:
        comps = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER: case GL_BGRA_EXT:
        comps = 4; break;
    default:
        return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE:
        return comps;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES:
        return comps * 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT:
        return comps * 4;
    }
    return 0;
}

// Bytes GL reads (unpack) or writes (pack) for a w x h x d image in client
// memory, by ES 3.0's pixel store rules: each row starts ALIGNMENT-aligned,
// rows are ROW_LENGTH pixels apart, SKIP_PIXELS/ROWS/IMAGES move the start,
// and the last row isn't padded. That's also the size WebGL2 requires of the
// array it's given. The store state is read back from GL, since that is what
// the driver will use.
static uint64_t _imageBytes(bool pack, bool three_d, GLsizei w, GLsizei h, GLsizei d,
                            GLenum format, GLenum type) {
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    uint64_t bpp = _pixelBytes(format, type);
    if (!bpp) bpp = 16;  // a pair we don't know: assume ES's widest pixel (RGBA32F)
    GLint align = 4, row_len = 0, skip_rows = 0, skip_px = 0, img_h = 0, skip_img = 0;
    glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &align);
    glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &row_len);
    glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &skip_px);
    if (three_d) {  // there's no pack equivalent
        glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &img_h);
        glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_img);
    }
    uint64_t a = align > 0 ? (uint64_t)align : 1;
    uint64_t row_px = row_len > 0 ? (uint64_t)row_len : (uint64_t)w;
    uint64_t stride = (row_px * bpp + a - 1) / a * a;   // < 2^36, can't overflow
    uint64_t img_stride = _mulSat(img_h > 0 ? (uint64_t)img_h : (uint64_t)h, stride);
    uint64_t bytes = _mulSat((uint64_t)(skip_px > 0 ? skip_px : 0), bpp);
    bytes = _addSat(bytes, _mulSat((uint64_t)(skip_rows > 0 ? skip_rows : 0), stride));
    bytes = _addSat(bytes, _mulSat((uint64_t)(skip_img > 0 ? skip_img : 0), img_stride));
    bytes = _addSat(bytes, _mulSat((uint64_t)d - 1, img_stride));
    bytes = _addSat(bytes, _mulSat((uint64_t)h - 1, stride));
    return _addSat(bytes, (uint64_t)w * bpp);
}

// The pixels argument of glTex(Sub)Image*. With a buffer on
// GL_PIXEL_UNPACK_BUFFER it is an offset into that buffer and is passed on
// as one; otherwise it is cart memory, the image's size of it. NULL stays
// NULL: no data, for glTexImage*. Returns false if the cart was trapped.
static bool _unpackPixels(const char* import, uint32_t p, bool three_d, GLsizei w, GLsizei h,
                          GLsizei d, GLenum format, GLenum type, const void** out) {
    if (_boundBuffer(GL_PIXEL_UNPACK_BUFFER_BINDING)) {
        *out = (const void*)(uintptr_t)p;
        return true;
    }
    *out = wptr(p);
    return !p || wc_cart_range_ok(_host, import, p, _imageBytes(false, three_d, w, h, d, format, type));
}

// The data argument of glCompressedTex(Sub)Image*: the same, but its size is
// imageSize, which the cart states and GL checks against the dimensions.
static bool _unpackCompressed(const char* import, uint32_t p, GLsizei imageSize, const void** out) {
    if (_boundBuffer(GL_PIXEL_UNPACK_BUFFER_BINDING)) {
        *out = (const void*)(uintptr_t)p;
        return true;
    }
    *out = wptr(p);
    return !p || wc_cart_range_ok(_host, import, p, _nbytes(imageSize, 1));
}

// ─── State ─────────────────────────────────────────────────────────────────

GL_REG(glEnable,  1, 0, glEnable(A_U32(0)))
GL_REG(glDisable, 1, 0, glDisable(A_U32(0)))
GL_REG(glGetError, 0, 1, R_I32(glGetError()))
GL_REG(glFinish, 0, 0, glFinish())
GL_REG(glFlush, 0, 0, glFlush())
GL_REG(glHint, 2, 0, glHint(A_U32(0), A_U32(1)))
GL_REG(glPixelStorei, 2, 0, glPixelStorei(A_U32(0), A_I32(1)))
GL_REG(glIsEnabled, 1, 1, R_I32(glIsEnabled(A_U32(0))))

GL_REG(glGetIntegerv, 2, 0, {
    GLenum pname = A_U32(0);
    _getInto<GLint>(_import, A_U32(1), _getCount(pname), [&](GLint* out) {
        glGetIntegerv(pname, out);
        // Ensure correct values for draw buffer caps — Core 3.3 returns correct values
        // but gl4es may query these before the context is fully configured
        if ((pname == 0x8CDF || pname == 0x8824) && *out < 4) {
            *out = 8; // GL_MAX_COLOR_ATTACHMENTS / GL_MAX_DRAW_BUFFERS
        }
    });
})
GL_REG(glGetFloatv, 2, 0, {
    GLenum pname = A_U32(0);
    _getInto<GLfloat>(_import, A_U32(1), _getCount(pname), [&](GLfloat* out) { glGetFloatv(pname, out); });
})
GL_REG(glGetBooleanv, 2, 0, {
    GLenum pname = A_U32(0);
    _getInto<GLboolean>(_import, A_U32(1), _getCount(pname), [&](GLboolean* out) { glGetBooleanv(pname, out); });
})
// GL writes at most bufSize values.
GL_REG(glGetInternalformativ, 5, 0, {
    GLsizei bufSize = A_I32(3);
    uint32_t p = A_U32(4);
    NEED(p, _nbytes(bufSize, 4));
    glGetInternalformativ(A_U32(0), A_U32(1), A_U32(2), bufSize, (GLint*)wmem(p));
})

// glGetString: allocate buffer via cart's malloc (cache per GL_xxx name)
static uint32_t _glstring_cache[8] = {0};
// NOTE: if you change overridden strings (GL_VERSION, GL_EXTENSIONS, etc.),
// clear _glstring_cache or the old value persists.

// Forward declare — defined in cart_host.cpp
extern "C" v8::Global<v8::Function>* wc_get_malloc_fn(wc_host_t* host);
extern "C" void wc_refresh_memory(wc_host_t* host);

static uint32_t _gl_alloc_string(wc_host_t* host, const char* s) {
    size_t len = strlen(s);
    auto* malloc_fn = wc_get_malloc_fn(host);
    if (!malloc_fn || malloc_fn->IsEmpty()) return 0;

    v8::Local<v8::Value> arg = v8::Integer::New(g_isolate, (int32_t)(len + 1));
    auto result = malloc_fn->Get(g_isolate)->Call(ctx(), ctx()->Global(), 1, &arg);
    if (result.IsEmpty()) return 0;

    uint32_t ptr = result.ToLocalChecked()->Uint32Value(ctx()).FromJust();
    // Refresh memory (malloc may grow) — caller must be in V8 scopes
    wc_refresh_memory(host);
    if (ptr && (uint64_t)ptr + len + 1 <= host->memory_size) {
        memcpy(host->memory + ptr, s, len);
        host->memory[ptr + len] = 0;
        return ptr;
    }
    return 0;
}

// Build extension string from glGetStringi (Core profile returns empty from glGetString)
// Also inject GLES-equivalent extension names for Core profile features so gl4es can find them.
static char _ext_string_buf[16384] = {0};
static const char* _build_extension_string(void) {
    if (_ext_string_buf[0]) return _ext_string_buf;
    // Start with driver's glGetString result if non-empty
    const char* base = (const char*)glGetString(GL_EXTENSIONS);
    int offset = 0;
    if (base && base[0]) {
        int len = strlen(base);
        if (len > 14000) len = 14000;
        memcpy(_ext_string_buf, base, len);
        offset = len;
    } else {
        // Core profile: build from glGetStringi
        GLint num_ext = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &num_ext);
        for (int i = 0; i < num_ext && offset < 14000; i++) {
            const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
            if (ext) {
                int len = strlen(ext);
                if (offset + len + 1 < 16384) {
                    if (offset > 0) _ext_string_buf[offset++] = ' ';
                    memcpy(_ext_string_buf + offset, ext, len);
                    offset += len;
                }
            }
        }
    }
    // Inject GLES extension names that gl4es looks for.
    // On Core 3.3+ these features are core, but gl4es probes by GLES name.
    static const char* gles_compat_exts[] = {
        "GL_EXT_blend_minmax", "GL_EXT_draw_buffers",
        "GL_OES_mapbuffer", "GL_OES_element_index_uint",
        "GL_OES_packed_depth_stencil", "GL_OES_depth24",
        "GL_OES_rgb8_rgba8", "GL_EXT_multi_draw_arrays",
        "GL_EXT_texture_format_BGRA8888", "GL_OES_depth_texture",
        "GL_OES_texture_stencil8", "GL_EXT_texture_rg",
        "GL_OES_texture_float", "GL_OES_texture_half_float",
        "GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float",
        "GL_EXT_frag_depth", "GL_OES_standard_derivatives",
        "GL_OES_get_program_binary", "GL_OES_vertex_array_object",
        "GL_EXT_texture_filter_anisotropic", "GL_OES_texture_npot",
        "GL_EXT_draw_buffers_indexed",
        "GL_OES_vertex_array_object",
        NULL
    };
    for (int i = 0; gles_compat_exts[i]; i++) {
        // Only add if not already present (check with trailing space for exact match)
        char search_buf[128];
        snprintf(search_buf, sizeof(search_buf), "%s ", gles_compat_exts[i]);
        if (!strstr(_ext_string_buf, search_buf)) {
            int len = strlen(gles_compat_exts[i]);
            if (offset + len + 1 < 16384) {
                _ext_string_buf[offset++] = ' ';
                memcpy(_ext_string_buf + offset, gles_compat_exts[i], len);
                offset += len;
            }
        }
    }
    // Trailing space — gl4es searches for "GL_EXT_xxx " (with space)
    if (offset > 0 && _ext_string_buf[offset-1] != ' ') {
        _ext_string_buf[offset++] = ' ';
    }
    _ext_string_buf[offset] = '\0';
    return _ext_string_buf;
}

GL_REG(glGetString, 1, 1, {
    uint32_t name = A_U32(0);
    const char* s = (const char*)glGetString(name);
    // wasmcart ABI = WebGL2 = ES 3.0. Always report ES 3.0 regardless of actual context.
    if (name == 0x1F02) s = "OpenGL ES 3.0 wasmcart";
    // Always build comprehensive extension string — Core profile may return
    // empty or ARB-only names. GLES compat names are always injected.
    if (name == 0x1F03) {
        s = _build_extension_string();
        static int _ext_logged = 0;
        if (!_ext_logged) {
            _ext_logged = 1;
            wc_log("wasmcart: GL_EXTENSIONS has draw_buffers: %s\n",
                strstr(s, "GL_EXT_draw_buffers") ? "YES" : "NO");
            wc_log("wasmcart: GL_EXTENSIONS length: %d\n", (int)strlen(s));
        }
    }
    if (!s) { R_I32(0); } else {
        int idx = (name == 0x1F00) ? 0 : (name == 0x1F01) ? 1 : (name == 0x1F02) ? 2 :
                  (name == 0x1F03) ? 3 : (name == 0x8B8C) ? 4 : 5;
        if (_glstring_cache[idx]) { R_I32(_glstring_cache[idx]); }
        else {
            uint32_t ptr = _gl_alloc_string(_host, s);
            _glstring_cache[idx] = ptr;
            R_I32(ptr);
        }
    }
})

// ─── Viewport / clear ──────────────────────────────────────────────────────

GL_REG(glViewport, 4, 0, glViewport(A_I32(0), A_I32(1), A_I32(2), A_I32(3)))
GL_REG(glScissor, 4, 0, glScissor(A_I32(0), A_I32(1), A_I32(2), A_I32(3)))
GL_REG(glClear, 1, 0, {
    uint32_t mask = A_U32(0);
    if (_cart_blitted_to_redirect && _last_draw_fbo == _redirect_fbo && (mask & GL_COLOR_BUFFER_BIT)) {
        // Suppress color clear on redirect FBO after cart blitted to it
        uint32_t remaining = mask & ~GL_COLOR_BUFFER_BIT;
        if (remaining) glClear(remaining);
    } else {
        glClear(mask);
    }
})
GL_REG(glClearColor, 4, 0, glClearColor(A_F32(0), A_F32(1), A_F32(2), A_F32(3)))
GL_REG(glClearDepthf, 1, 0, glClearDepthf(A_F32(0)))
GL_REG(glClearStencil, 1, 0, glClearStencil(A_I32(0)))

// ─── Blending ──────────────────────────────────────────────────────────────

GL_REG(glBlendFunc, 2, 0, glBlendFunc(A_U32(0), A_U32(1)))
GL_REG(glBlendFuncSeparate, 4, 0, glBlendFuncSeparate(A_U32(0), A_U32(1), A_U32(2), A_U32(3)))
GL_REG(glBlendEquation, 1, 0, glBlendEquation(A_U32(0)))
GL_REG(glBlendEquationSeparate, 2, 0, glBlendEquationSeparate(A_U32(0), A_U32(1)))
GL_REG(glBlendColor, 4, 0, glBlendColor(A_F32(0), A_F32(1), A_F32(2), A_F32(3)))
GL_REG(glColorMask, 4, 0, glColorMask(A_U32(0)!=0, A_U32(1)!=0, A_U32(2)!=0, A_U32(3)!=0))

// ─── Depth / stencil ──────────────────────────────────────────────────────

GL_REG(glDepthFunc, 1, 0, glDepthFunc(A_U32(0)))
GL_REG(glDepthMask, 1, 0, glDepthMask(A_U32(0)!=0))
GL_REG(glDepthRangef, 2, 0, glDepthRangef(A_F32(0), A_F32(1)))
GL_REG(glStencilFunc, 3, 0, glStencilFunc(A_U32(0), A_I32(1), A_U32(2)))
GL_REG(glStencilFuncSeparate, 4, 0, glStencilFuncSeparate(A_U32(0), A_U32(1), A_I32(2), A_U32(3)))
GL_REG(glStencilOp, 3, 0, glStencilOp(A_U32(0), A_U32(1), A_U32(2)))
GL_REG(glStencilOpSeparate, 4, 0, glStencilOpSeparate(A_U32(0), A_U32(1), A_U32(2), A_U32(3)))
GL_REG(glStencilMask, 1, 0, glStencilMask(A_U32(0)))
GL_REG(glStencilMaskSeparate, 2, 0, glStencilMaskSeparate(A_U32(0), A_U32(1)))

// ─── Face culling ─────────────────────────────────────────────────────────

GL_REG(glCullFace, 1, 0, glCullFace(A_U32(0)))
GL_REG(glFrontFace, 1, 0, glFrontFace(A_U32(0)))
GL_REG(glPolygonOffset, 2, 0, glPolygonOffset(A_F32(0), A_F32(1)))
GL_REG(glLineWidth, 1, 0, glLineWidth(A_F32(0)))

// ─── Buffers ──────────────────────────────────────────────────────────────

// glGen*/glDelete*(n, names) all read or write n GLuints at names.
GL_REG(glGenBuffers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glGenBuffers(A_I32(0), (GLuint*)wmem(A_U32(1))); })
GL_REG(glDeleteBuffers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDeleteBuffers(A_I32(0), (const GLuint*)wmem(A_U32(1))); })
GL_REG(glBindBuffer, 2, 0, glBindBuffer(A_U32(0), A_U32(1)))
GL_REG(glBufferData, 4, 0, {
    uint32_t size = A_U32(1);
    uint32_t data = A_U32(2);
    if (data) NEED(data, size);
    glBufferData(A_U32(0), size, wptr(data), A_U32(3));
})
GL_REG(glBufferSubData, 4, 0, {
    GLenum target = A_U32(0);
    GLintptr offset = A_U32(1);
    GLsizeiptr size = A_U32(2);
    if (A_U32(3)) NEED(A_U32(3), A_U32(2));
    const void* data = wptr(A_U32(3));
    // Buffer orphaning: if updating from offset 0, orphan first to avoid
    // GPU sync stalls on mobile (Mali, Adreno). Driver allocates a new
    // buffer instead of waiting for the GPU to finish with the old one.
    if (offset == 0) {
        glBufferData(target, size, NULL, GL_STREAM_DRAW);
    }
    glBufferSubData(target, offset, size, data);
})
GL_REG(glIsBuffer, 1, 1, R_I32(glIsBuffer(A_U32(0))))

// ─── Textures ─────────────────────────────────────────────────────────────

GL_REG(glGenTextures, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glGenTextures(A_I32(0), (GLuint*)wmem(A_U32(1))); })
GL_REG(glDeleteTextures, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDeleteTextures(A_I32(0), (const GLuint*)wmem(A_U32(1))); })
GL_REG(glBindTexture, 2, 0, glBindTexture(A_U32(0), A_U32(1)))
GL_REG(glActiveTexture, 1, 0, glActiveTexture(A_U32(0)))
GL_REG(glTexParameteri, 3, 0, glTexParameteri(A_U32(0), A_U32(1), A_I32(2)))
GL_REG(glTexParameterf, 3, 0, glTexParameterf(A_U32(0), A_U32(1), A_F32(2)))
GL_REG(glGenerateMipmap, 1, 0, glGenerateMipmap(A_U32(0)))
GL_REG(glIsTexture, 1, 1, R_I32(glIsTexture(A_U32(0))))

// Fix unsized/legacy internal formats for ES 3.0/Core 3.3/WebGL2 compatibility
static GLenum _fixInternalFormat(GLenum ifmt, GLenum type) {
    if (type == GL_UNSIGNED_BYTE) {
        switch (ifmt) {
            case 0x1906: return 0x8229; // GL_ALPHA → GL_R8
            case 0x1909: return 0x8229; // GL_LUMINANCE → GL_R8
            case 0x190A: return 0x8227; // GL_LUMINANCE_ALPHA → GL_RG8
            case 0x1907: return 0x8051; // GL_RGB → GL_RGB8
            case 0x1908: return 0x8058; // GL_RGBA → GL_RGBA8
            case 0x1903: return 0x8229; // GL_RED → GL_R8
        }
    } else if (type == GL_FLOAT) {
        switch (ifmt) {
            case 0x1907: return 0x8815; // GL_RGB → GL_RGB32F
            case 0x1908: return 0x8814; // GL_RGBA → GL_RGBA32F
        }
    }
    return ifmt;
}
static GLenum _fixFormat(GLenum fmt) {
    switch (fmt) {
        case 0x1906: return 0x1903; // GL_ALPHA → GL_RED
        case 0x1909: return 0x1903; // GL_LUMINANCE → GL_RED
        case 0x190A: return 0x8227; // GL_LUMINANCE_ALPHA → GL_RG
        default: return fmt;
    }
}
GL_REG(glTexImage2D, 9, 0, {
    GLenum type = A_U32(7);
    GLenum ifmt = _fixInternalFormat(A_I32(2), type);
    GLenum fmt = _fixFormat(A_U32(6));
    const void* pixels;
    if (!_unpackPixels(_import, A_U32(8), false, A_I32(3), A_I32(4), 1, fmt, type, &pixels)) return;
    glTexImage2D(A_U32(0), A_I32(1), ifmt, A_I32(3), A_I32(4), A_I32(5), fmt, type, pixels);
})
GL_REG(glTexSubImage2D, 9, 0, {
    const void* pixels;
    if (!_unpackPixels(_import, A_U32(8), false, A_I32(4), A_I32(5), 1, A_U32(6), A_U32(7), &pixels)) return;
    glTexSubImage2D(A_U32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_U32(6), A_U32(7), pixels);
})
GL_REG(glCompressedTexImage2D, 8, 0, {
    const void* data;
    if (!_unpackCompressed(_import, A_U32(7), A_I32(6), &data)) return;
    glCompressedTexImage2D(A_U32(0), A_I32(1), A_U32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), data);
})
GL_REG(glCompressedTexSubImage2D, 9, 0, {
    const void* data;
    if (!_unpackCompressed(_import, A_U32(8), A_I32(7), &data)) return;
    glCompressedTexSubImage2D(A_U32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_U32(6), A_I32(7), data);
})
GL_REG(glCopyTexSubImage2D, 8, 0, glCopyTexSubImage2D(A_U32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), A_I32(7)))
GL_REG(glTexImage3D, 10, 0, {
    const void* pixels;
    if (!_unpackPixels(_import, A_U32(9), true, A_I32(3), A_I32(4), A_I32(5), A_U32(7), A_U32(8), &pixels)) return;
    glTexImage3D(A_U32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), A_U32(7), A_U32(8), pixels);
})
GL_REG(glTexStorage2D, 5, 0, glTexStorage2D(A_U32(0), A_I32(1), A_U32(2), A_I32(3), A_I32(4)))
GL_REG(glTexStorage3D, 6, 0, glTexStorage3D(A_U32(0), A_I32(1), A_U32(2), A_I32(3), A_I32(4), A_I32(5)))

// ─── Shaders ──────────────────────────────────────────────────────────────

GL_REG(glCreateShader, 1, 1, R_I32(glCreateShader(A_U32(0))))
GL_REG(glDeleteShader, 1, 0, glDeleteShader(A_U32(0)))
GL_REG(glCompileShader, 1, 0, {
    GLuint shader = A_U32(0);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        wc_log("wasmcart: shader %u compile FAILED: %s\n", shader, log);
    }
})
GL_REG(glIsShader, 1, 1, R_I32(glIsShader(A_U32(0))))

// Shader source is passed through unmodified. wasmcart ABI requires ES 3.0
// compatible shaders (#version 100 or #version 300 es). Carts using gl4es
// must configure it for GLES output — the host does not translate shaders.

// REMOVED: _patch_shader_v1_to_v300es — shader patching was interfering with
// gl4es's GLSL version detection. gl4es tests if #version 120 compiles; our
// patcher made it succeed, causing gl4es to generate desktop GL shaders.
// Without the patcher, gl4es correctly detects GLES-only and outputs #version 100.

GL_REG(glShaderSource, 4, 0, {
    uint32_t shader = A_U32(0);
    int32_t count = A_I32(1);
    uint32_t strings_ptr = A_U32(2);
    uint32_t lengths_ptr = A_U32(3);
    const char* sources[16];
    int lens[16];
    std::string copies[16];
    int n = count > 16 ? 16 : count;
    NEED(strings_ptr, _nbytes(n, 4));
    if (lengths_ptr) NEED(lengths_ptr, _nbytes(n, 4));
    for (int i = 0; i < n; i++) {
        uint32_t src;
        int32_t len = -1;
        memcpy(&src, wmem(strings_ptr + 4 * i), 4);
        if (lengths_ptr) memcpy(&len, wmem(lengths_ptr + 4 * i), 4);
        // A negative length, like no lengths array, means NUL-terminated.
        if (len >= 0) {
            NEED(src, (uint32_t)len);
            sources[i] = (const char*)wmem(src);
        } else {
            NEED_STR(str, src);
            copies[i] = std::move(str);
            sources[i] = copies[i].c_str();
        }
        lens[i] = len;
    }
    glShaderSource(shader, n, sources, lengths_ptr ? lens : NULL);
})

GL_REG(glGetShaderiv, 3, 0, {
    _getInto<GLint>(_import, A_U32(2), 1, [&](GLint* out) { glGetShaderiv(A_U32(0), A_U32(1), out); });
})
// The info logs write at most bufSize bytes to infoLog; length is optional.
GL_REG(glGetShaderInfoLog, 4, 0, {
    GLsizei bufSize = A_I32(1);
    if (A_U32(2)) NEED(A_U32(2), 4);
    NEED(A_U32(3), _nbytes(bufSize, 1));
    glGetShaderInfoLog(A_U32(0), bufSize, (GLsizei*)wptr(A_U32(2)), (char*)wmem(A_U32(3)));
})

// ─── Programs ─────────────────────────────────────────────────────────────

GL_REG(glCreateProgram, 0, 1, R_I32(glCreateProgram()))
GL_REG(glDeleteProgram, 1, 0, glDeleteProgram(A_U32(0)))
GL_REG(glAttachShader, 2, 0, glAttachShader(A_U32(0), A_U32(1)))
GL_REG(glDetachShader, 2, 0, glDetachShader(A_U32(0), A_U32(1)))
GL_REG(glLinkProgram, 1, 0, {
    GLuint prog = A_U32(0);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        wc_log("wasmcart: program %u link FAILED: %s\n", prog, log);
    }
})
GL_REG(glUseProgram, 1, 0, glUseProgram(A_U32(0)))
GL_REG(glIsProgram, 1, 1, R_I32(glIsProgram(A_U32(0))))
GL_REG(glValidateProgram, 1, 0, glValidateProgram(A_U32(0)))
GL_REG(glGetProgramiv, 3, 0, {
    GLenum pname = A_U32(1);
    uint64_t n = pname == GL_COMPUTE_WORK_GROUP_SIZE ? 3 : 1;
    _getInto<GLint>(_import, A_U32(2), n, [&](GLint* out) { glGetProgramiv(A_U32(0), pname, out); });
})
GL_REG(glGetProgramInfoLog, 4, 0, {
    GLsizei bufSize = A_I32(1);
    if (A_U32(2)) NEED(A_U32(2), 4);
    NEED(A_U32(3), _nbytes(bufSize, 1));
    glGetProgramInfoLog(A_U32(0), bufSize, (GLsizei*)wptr(A_U32(2)), (char*)wmem(A_U32(3)));
})
GL_REG(glBindAttribLocation, 3, 0, { NEED_STR(name, A_U32(2)); glBindAttribLocation(A_U32(0), A_U32(1), name.c_str()); })
GL_REG(glGetAttribLocation, 2, 1, { NEED_STR(name, A_U32(1)); R_I32(glGetAttribLocation(A_U32(0), name.c_str())); })
GL_REG(glGetUniformLocation, 2, 1, { NEED_STR(name, A_U32(1)); R_I32(glGetUniformLocation(A_U32(0), name.c_str())); })

// length is optional; size and type are one value each; name gets at most
// bufSize bytes.
GL_REG(glGetActiveAttrib, 7, 0, {
    GLsizei bufSize = A_I32(2);
    if (A_U32(3)) NEED(A_U32(3), 4);
    NEED(A_U32(4), 4);
    NEED(A_U32(5), 4);
    NEED(A_U32(6), _nbytes(bufSize, 1));
    glGetActiveAttrib(A_U32(0), A_U32(1), bufSize, (GLsizei*)wptr(A_U32(3)), (GLint*)wmem(A_U32(4)), (GLenum*)wmem(A_U32(5)), (char*)wmem(A_U32(6)));
})
GL_REG(glGetActiveUniform, 7, 0, {
    GLsizei bufSize = A_I32(2);
    if (A_U32(3)) NEED(A_U32(3), 4);
    NEED(A_U32(4), 4);
    NEED(A_U32(5), 4);
    NEED(A_U32(6), _nbytes(bufSize, 1));
    glGetActiveUniform(A_U32(0), A_U32(1), bufSize, (GLsizei*)wptr(A_U32(3)), (GLint*)wmem(A_U32(4)), (GLenum*)wmem(A_U32(5)), (char*)wmem(A_U32(6)));
})

// ─── Uniforms ─────────────────────────────────────────────────────────────

GL_REG(glUniform1i, 2, 0, glUniform1i(A_I32(0), A_I32(1)))
GL_REG(glUniform2i, 3, 0, glUniform2i(A_I32(0), A_I32(1), A_I32(2)))
GL_REG(glUniform3i, 4, 0, glUniform3i(A_I32(0), A_I32(1), A_I32(2), A_I32(3)))
GL_REG(glUniform4i, 5, 0, glUniform4i(A_I32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4)))
GL_REG(glUniform1f, 2, 0, glUniform1f(A_I32(0), A_F32(1)))
GL_REG(glUniform2f, 3, 0, glUniform2f(A_I32(0), A_F32(1), A_F32(2)))
GL_REG(glUniform3f, 4, 0, glUniform3f(A_I32(0), A_F32(1), A_F32(2), A_F32(3)))
GL_REG(glUniform4f, 5, 0, glUniform4f(A_I32(0), A_F32(1), A_F32(2), A_F32(3), A_F32(4)))

// glUniform*v(location, count, value) read count elements of N values each.
#define UNIFORM_V(T, N) NEED(A_U32(2), _nbytes(A_I32(1), (N) * sizeof(T))); const T* _v = (const T*)wmem(A_U32(2))
GL_REG(glUniform1iv, 3, 0, { UNIFORM_V(GLint, 1); glUniform1iv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform2iv, 3, 0, { UNIFORM_V(GLint, 2); glUniform2iv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform3iv, 3, 0, { UNIFORM_V(GLint, 3); glUniform3iv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform4iv, 3, 0, { UNIFORM_V(GLint, 4); glUniform4iv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform1fv, 3, 0, { UNIFORM_V(GLfloat, 1); glUniform1fv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform2fv, 3, 0, { UNIFORM_V(GLfloat, 2); glUniform2fv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform3fv, 3, 0, { UNIFORM_V(GLfloat, 3); glUniform3fv(A_I32(0), A_I32(1), _v); })
GL_REG(glUniform4fv, 3, 0, { UNIFORM_V(GLfloat, 4); glUniform4fv(A_I32(0), A_I32(1), _v); })

// glUniformMatrix*v(location, count, transpose, value): count NxN matrices.
#define UNIFORM_M(N) NEED(A_U32(3), _nbytes(A_I32(1), (N) * (N) * sizeof(GLfloat))); const GLfloat* _v = (const GLfloat*)wmem(A_U32(3))
GL_REG(glUniformMatrix2fv, 4, 0, { UNIFORM_M(2); glUniformMatrix2fv(A_I32(0), A_I32(1), A_U32(2)!=0, _v); })
GL_REG(glUniformMatrix3fv, 4, 0, { UNIFORM_M(3); glUniformMatrix3fv(A_I32(0), A_I32(1), A_U32(2)!=0, _v); })
GL_REG(glUniformMatrix4fv, 4, 0, { UNIFORM_M(4); glUniformMatrix4fv(A_I32(0), A_I32(1), A_U32(2)!=0, _v); })

// ─── Vertex attributes ───────────────────────────────────────────────────

GL_REG(glEnableVertexAttribArray, 1, 0, glEnableVertexAttribArray(A_U32(0)))
GL_REG(glDisableVertexAttribArray, 1, 0, {
    uint32_t idx = A_U32(0);
    _clearClientAttrib(idx);
    glDisableVertexAttribArray(idx);
})
// A pointer of 0 with no buffer bound is passed on as NULL, i.e. no array:
// the state every attribute starts in.
GL_REG(glVertexAttribPointer, 6, 0, {
    uint32_t idx = A_U32(0);
    if (A_U32(5) != 0 && _boundBuffer(GL_ARRAY_BUFFER_BINDING) == 0) {
        _setClientAttrib(idx, A_I32(1), A_U32(2), A_U32(3) != 0, A_I32(4), A_U32(5));
    } else {
        _clearClientAttrib(idx);
        glVertexAttribPointer(idx, A_I32(1), A_U32(2), A_U32(3)!=0, A_I32(4), (const void*)(uintptr_t)A_U32(5));
    }
})
GL_REG(glVertexAttribIPointer, 5, 0, {
    uint32_t idx = A_U32(0);
    if (A_U32(4) != 0 && _boundBuffer(GL_ARRAY_BUFFER_BINDING) == 0) {
        _setClientAttrib(idx, A_I32(1), A_U32(2), GL_FALSE, A_I32(3), A_U32(4));
    } else {
        _clearClientAttrib(idx);
        glVertexAttribIPointer(idx, A_I32(1), A_U32(2), A_I32(3), (const void*)(uintptr_t)A_U32(4));
    }
})
GL_REG(glVertexAttribDivisor, 2, 0, glVertexAttribDivisor(A_U32(0), A_U32(1)))

// ─── VAOs ─────────────────────────────────────────────────────────────────

GL_REG(glGenVertexArrays, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glGenVertexArrays(A_I32(0), (GLuint*)wmem(A_U32(1))); })
GL_REG(glDeleteVertexArrays, 2, 0, {
    GLsizei n = A_I32(0);
    NEED(A_U32(1), _nbytes(n, 4));
    const GLuint* arrays = (const GLuint*)wmem(A_U32(1));
    // Deleting the bound VAO reverts the binding to 0 (GL semantics).
    if (arrays) for (GLsizei i = 0; i < n; i++) if (arrays[i] == _cart_bound_vao) _cart_bound_vao = 0;
    glDeleteVertexArrays(n, arrays);
})
GL_REG(glBindVertexArray, 1, 0, {
    GLuint vao = A_U32(0);
    if (vao == 0 && _cart_vao) vao = _cart_vao;
    _cart_bound_vao = vao;
    glBindVertexArray(vao);
})

// ─── Drawing ──────────────────────────────────────────────────────────────

GL_REG(glDrawArrays, 3, 0, {
    GLenum mode = A_U32(0);
    int first = A_I32(1);
    int count = A_I32(2);
    // A negative first or count is GL's INVALID_VALUE and reads nothing.
    if (_numClientAttribs > 0 && first >= 0 && count > 0) {
        if (!_uploadClientAttribs(_import, first, count, false)) return;
        glDrawArrays(mode, 0, count);
    } else {
        glDrawArrays(mode, first, count);
    }
    // Log first few GL errors to diagnose rendering issues
    {
        static int _gl_err_count = 0;
        if (_gl_err_count < 5) {
            GLenum err = glGetError();
            if (err) {
                _gl_err_count++;
                wc_log("wasmcart: GL error 0x%04x after glDrawArrays(mode=0x%x, first=%d, count=%d) draw#%d\n",
                    err, mode, first, count, _draw_call_count);
            }
        }
    }
    _draw_call_count++;
})
GL_REG(glDrawElements, 4, 0, {
    GLenum mode = A_U32(0);
    int count = A_I32(1);
    GLenum type = A_U32(2);
    uint32_t offsetPtr = A_U32(3);
    // With no element buffer bound the indices are client-side, in cart
    // memory (offset 0 included: that's cart address 0, not a NULL for the
    // driver to follow). They go through _tempEBO, so GL only ever gets an
    // offset.
    bool hasClientIndices = _boundBuffer(GL_ELEMENT_ARRAY_BUFFER_BINDING) == 0;
    if (count <= 0 || !_isIndexType(type)) {
        // Nothing drawn, or GL's INVALID_VALUE/INVALID_ENUM: no index is read.
        glDrawElements(mode, count, type, hasClientIndices ? NULL : (const void*)(uintptr_t)offsetPtr);
        return;
    }
    if (_numClientAttribs > 0 || hasClientIndices) {
        std::vector<uint8_t> indices;
        uint64_t maxVertex = 0;
        uint64_t indexBytes = (uint64_t)count * _bytesForGLType(type);
        if (hasClientIndices) {
            NEED(offsetPtr, indexBytes);
            // Copied first, then scanned and uploaded from the copy, so the
            // indices GL draws are the ones scanned: a threaded cart's other
            // threads can rewrite them in between.
            indices.assign(_host->memory + offsetPtr, _host->memory + offsetPtr + indexBytes);
            // Scan indices to find max vertex: that's how far GL reads into
            // each client array. The restart index isn't a vertex.
            const uint8_t* mem = indices.data();
            bool restart = glIsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX);
            for (int i = 0; i < count; i++) {
                uint32_t v;
                if (type == GL_UNSIGNED_SHORT) { uint16_t v16; memcpy(&v16, mem + 2 * i, 2); v = v16; }
                else if (type == GL_UNSIGNED_INT) memcpy(&v, mem + 4 * i, 4);
                else v = mem[i];
                if (restart && v == (type == GL_UNSIGNED_INT ? 0xFFFFFFFFu :
                                     type == GL_UNSIGNED_SHORT ? 0xFFFFu : 0xFFu)) continue;
                if (v > maxVertex) maxVertex = v;
            }
        } else {
            // The indices are in a buffer, which can't be read without mapping
            // it, so how many vertices they reach is a guess. The client arrays
            // are uploaded only as far as cart memory goes, which bounds what
            // the host reads; an index past what was uploaded reads past the
            // temp VBO on the GPU, as it did before.
            maxVertex = (uint64_t)count * 2;
        }
        if (!_uploadClientAttribs(_import, 0, maxVertex + 1, !hasClientIndices)) return;
        if (hasClientIndices) {
            _uploadClientIndices(indices.data(), indexBytes);
            glDrawElements(mode, count, type, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        } else {
            glDrawElements(mode, count, type, (const void*)(uintptr_t)offsetPtr);
        }
    } else {
        glDrawElements(mode, count, type, (const void*)(uintptr_t)offsetPtr);
    }
})
GL_REG(glDrawArraysInstanced, 4, 0, glDrawArraysInstanced(A_U32(0), A_I32(1), A_I32(2), A_I32(3)))
GL_REG(glDrawElementsInstanced, 5, 0, {
    GLenum mode = A_U32(0);
    int count = A_I32(1);
    GLenum type = A_U32(2);
    uint32_t offsetPtr = A_U32(3);
    GLsizei instances = A_I32(4);
    if (_boundBuffer(GL_ELEMENT_ARRAY_BUFFER_BINDING) != 0) {
        glDrawElementsInstanced(mode, count, type, (const void*)(uintptr_t)offsetPtr, instances);
        return;
    }
    // Client-side indices, as for glDrawElements. (Client-side vertex arrays
    // aren't uploaded for instanced draws.)
    if (count <= 0 || !_isIndexType(type)) {
        glDrawElementsInstanced(mode, count, type, NULL, instances);
        return;
    }
    uint64_t indexBytes = (uint64_t)count * _bytesForGLType(type);
    NEED(offsetPtr, indexBytes);
    _uploadClientIndices(wmem(offsetPtr), indexBytes);
    glDrawElementsInstanced(mode, count, type, 0, instances);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
})
GL_REG(glDrawBuffers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDrawBuffers(A_I32(0), (const GLenum*)wmem(A_U32(1))); })
GL_REG(glReadBuffer, 1, 0, glReadBuffer(A_U32(0)))

// ─── Missing GLES3 functions needed by Godot ──────────────────────────────

GL_REG(glGetStringi, 2, 1, {
    uint32_t name = A_U32(0);
    uint32_t index = A_U32(1);
    const char* s = (const char*)glGetStringi(name, index);
    {
        static int _si_dbg = 0;
        if (_si_dbg < 3 && name == 0x1F03 && s) {
            wc_log( "wasmcart: glGetStringi(GL_EXTENSIONS, %u) = %s\n", index, s);
            _si_dbg++;
        }
    }
    if (!s) { R_I32(0); } else {
        uint32_t ptr = _gl_alloc_string(_host, s);
        R_I32(ptr);
    }
})
GL_REG(glGetInteger64v, 2, 0, {
    GLenum pname = A_U32(0);
    _getInto<GLint64>(_import, A_U32(1), _getCount(pname), [&](GLint64* out) { glGetInteger64v(pname, out); });
})
GL_REG(glBindBufferBase, 3, 0, glBindBufferBase(A_U32(0), A_U32(1), A_U32(2)))
GL_REG(glBindBufferRange, 5, 0, glBindBufferRange(A_U32(0), A_U32(1), A_U32(2), A_I32(3), A_I32(4)))
GL_REG(glGetUniformBlockIndex, 2, 1, { NEED_STR(name, A_U32(1)); R_I32(glGetUniformBlockIndex(A_U32(0), name.c_str())); })
// Uniform-BLOCK introspection. An engine that cannot query
// GL_UNIFORM_BLOCK_DATA_SIZE sizes its UBO from whatever the missing import
// returned, uploads a block of that size, and the shader samples an
// essentially unwritten buffer: every vertex multiplied by a zero
// view-projection matrix collapses to a point. No GL error is raised and every
// draw call succeeds, so the screen simply stays empty. Kept Defold carts from
// ever drawing geometry on the JS host until the same pair was added there.
GL_REG(glGetActiveUniformBlockiv, 4, 0, {
    GLuint prog = A_U32(0), block = A_U32(1);
    GLenum pname = A_U32(2);
    // One value, except the index list, which has one per active uniform.
    GLint n = 1;
    if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
        n = 0;
        glGetActiveUniformBlockiv(prog, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &n);
    }
    _getInto<GLint>(_import, A_U32(3), _nbytes(n, 1), [&](GLint* out) {
        glGetActiveUniformBlockiv(prog, block, pname, out);
    });
})
// count indices in, count values out.
GL_REG(glGetActiveUniformsiv, 5, 0, {
    GLsizei count = A_I32(1);
    NEED(A_U32(2), _nbytes(count, 4));
    NEED(A_U32(4), _nbytes(count, 4));
    glGetActiveUniformsiv(A_U32(0), count, (const GLuint*)wmem(A_U32(2)), A_U32(3), (GLint*)wmem(A_U32(4)));
})
GL_REG(glUniformBlockBinding, 3, 0, glUniformBlockBinding(A_U32(0), A_U32(1), A_U32(2)))
GL_REG(glUniform1ui, 2, 0, glUniform1ui(A_I32(0), A_U32(1)))
GL_REG(glUniform1uiv, 3, 0, { UNIFORM_V(GLuint, 1); glUniform1uiv(A_I32(0), A_I32(1), _v); })
GL_REG(glVertexAttribI4ui, 5, 0, glVertexAttribI4ui(A_U32(0), A_U32(1), A_U32(2), A_U32(3), A_U32(4)))
GL_REG(glCopyBufferSubData, 5, 0, glCopyBufferSubData(A_U32(0), A_U32(1), A_I32(2), A_I32(3), A_I32(4)))
GL_REG(glFramebufferTextureLayer, 5, 0, glFramebufferTextureLayer(A_U32(0), A_U32(1), A_U32(2), A_I32(3), A_I32(4)))
GL_REG(glTexSubImage3D, 11, 0, {
    const void* pixels;
    if (!_unpackPixels(_import, A_U32(10), true, A_I32(5), A_I32(6), A_I32(7), A_U32(8), A_U32(9), &pixels)) return;
    glTexSubImage3D(A_U32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), A_I32(7), A_U32(8), A_U32(9), pixels);
})
GL_REG(glCompressedTexImage3D, 9, 0, {
    const void* data;
    if (!_unpackCompressed(_import, A_U32(8), A_I32(7), &data)) return;
    glCompressedTexImage3D(A_U32(0), A_I32(1), A_U32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), A_I32(7), data);
})
GL_REG(glCompressedTexSubImage3D, 11, 0, {
    const void* data;
    if (!_unpackCompressed(_import, A_U32(10), A_I32(9), &data)) return;
    glCompressedTexSubImage3D(A_U32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), A_I32(7), A_U32(8), A_I32(9), data);
})
GL_REG(glBeginTransformFeedback, 1, 0, glBeginTransformFeedback(A_U32(0)))
GL_REG(glEndTransformFeedback, 0, 0, glEndTransformFeedback())
GL_REG(glTransformFeedbackVaryings, 4, 0, {
    uint32_t prog = A_U32(0);
    int32_t count = A_I32(1);
    uint32_t strings_ptr = A_U32(2);
    const char* names[16];
    std::string copies[16];
    int n = count > 16 ? 16 : count;
    NEED(strings_ptr, _nbytes(n, 4));
    for (int i = 0; i < n; i++) {
        uint32_t name;
        memcpy(&name, wmem(strings_ptr + 4 * i), 4);
        NEED_STR(str, name);
        copies[i] = std::move(str);
        names[i] = copies[i].c_str();
    }
    glTransformFeedbackVaryings(prog, n, names, A_U32(3));
})
// A GLsync is a driver pointer, too wide for wasm32 and not something to
// take back from the cart: the driver would follow whatever value it passed.
// The cart gets a handle into this table instead (index + 1; 0 is no sync).
static std::vector<GLsync> _syncs;

static uint32_t _syncPut(GLsync s) {
    if (!s) return 0;
    for (size_t i = 0; i < _syncs.size(); i++)
        if (!_syncs[i]) { _syncs[i] = s; return (uint32_t)i + 1; }
    _syncs.push_back(s);
    return (uint32_t)_syncs.size();
}

static GLsync _syncGet(uint32_t h) {
    return (h && h <= _syncs.size()) ? _syncs[h - 1] : NULL;
}

// values gets at most bufSize values; length is optional.
GL_REG(glGetSynciv, 5, 0, {
    GLsync sync = _syncGet(A_U32(0));
    GLsizei bufSize = A_I32(2);
    if (A_U32(3)) NEED(A_U32(3), 4);
    NEED(A_U32(4), _nbytes(bufSize, 4));
    if (!sync) return;  // GL's INVALID_VALUE
    glGetSynciv(sync, A_U32(1), bufSize, (GLsizei*)wptr(A_U32(3)), (GLint*)wmem(A_U32(4)));
})
GL_REG(glGenQueries, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glGenQueries(A_I32(0), (GLuint*)wmem(A_U32(1))); })
GL_REG(glDeleteQueries, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDeleteQueries(A_I32(0), (const GLuint*)wmem(A_U32(1))); })

// Four values for GL_COLOR, one for GL_DEPTH. GL reads them from a copy, so
// a buffer value it doesn't expect can't make it read further.
GL_REG(glClearBufferfv, 3, 0, {
    GLenum buffer = A_U32(0);
    uint64_t n = buffer == GL_COLOR ? 4 : 1;
    GLfloat value[4] = {0, 0, 0, 0};
    NEED(A_U32(2), n * sizeof(GLfloat));
    memcpy(value, wmem(A_U32(2)), n * sizeof(GLfloat));
    glClearBufferfv(buffer, A_I32(1), value);
})

// ─── Framebuffers ─────────────────────────────────────────────────────────

static int _cart_uses_fbos = 0;
GL_REG(glGenFramebuffers, 2, 0, {
    NEED(A_U32(1), _nbytes(A_I32(0), 4));
    _cart_uses_fbos = 1;
    glGenFramebuffers(A_I32(0), (GLuint*)wmem(A_U32(1)));
})
GL_REG(glDeleteFramebuffers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDeleteFramebuffers(A_I32(0), (const GLuint*)wmem(A_U32(1))); })
// FBO redirect: capture what the cart renders to FBO 0

static GLuint _redirect_rbo = 0;

// DIRECT PRESENT: while set, the cart's framebuffer 0 is the real default
// framebuffer (the window surface) instead of the redirect FBO, and the
// present blit is skipped: the frame is already where it is shown. The host
// turns it on only when the surface is exactly the cart's render size (no
// scaling or letterbox to do) -- wc_gl_set_direct, between frames.
static int _direct = 0;
// The real "framebuffer 0" in direct mode: 0 for a window surface, or a
// frontend's framebuffer (libretro: RetroArch's hw_render FBO, which can
// change from frame to frame; wc_gl_set_direct_target).
static GLuint _direct_target = 0;

static void _ensure_redirect_fbo(uint32_t w, uint32_t h) {
    if (_redirect_fbo && _redirect_w == w && _redirect_h == h) return;
    // NEVER delete + re-gen these on resize. The cart allocates GL names
    // through the same context; a delete here lets the driver hand the
    // recycled name to the cart's next glGen — or vice versa. Seen for real
    // on Mali-G715: the re-genned redirect texture came back with the SAME
    // name as the cart's live sprite atlas, so every frame rendered INTO
    // the atlas and every sprite sampled the frame instead of its art.
    // Allocate the names once and re-spec their storage in place.
    if (!_redirect_fbo) {
        glGenFramebuffers(1, &_redirect_fbo);
        glGenTextures(1, &_redirect_tex);
        glGenRenderbuffers(1, &_redirect_rbo);
    }
    // Don't leak the resize into cart-visible state: engines cache their
    // texture binding and skip redundant glBindTexture calls.
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, _redirect_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glBindFramebuffer(GL_FRAMEBUFFER, _redirect_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _redirect_tex, 0);
    // Also need depth/stencil for 3D rendering
    glBindRenderbuffer(GL_RENDERBUFFER, _redirect_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _redirect_rbo);
    _redirect_w = w; _redirect_h = h;
    // Create cart VAO to isolate cart's vertex attrib state from host
    if (!_cart_vao) {
        glGenVertexArrays(1, &_cart_vao);
    }
    // Set viewport so cart gets correct dimensions when querying
    glViewport(0, 0, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GL_REG(glBindFramebuffer, 2, 0, {
    uint32_t target = A_U32(0);
    uint32_t fb = A_U32(1);
    GLuint actual = fb;
    if (fb == 0 && _redirect_fbo) {
        actual = _direct ? _direct_target : _redirect_fbo;
    }
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        _last_draw_fbo = actual;
    }
    glBindFramebuffer(target, actual);
})
GL_REG(glCheckFramebufferStatus, 1, 1, R_I32(glCheckFramebufferStatus(A_U32(0))))
GL_REG(glFramebufferTexture2D, 5, 0, glFramebufferTexture2D(A_U32(0), A_U32(1), A_U32(2), A_U32(3), A_I32(4)))
GL_REG(glFramebufferRenderbuffer, 4, 0, glFramebufferRenderbuffer(A_U32(0), A_U32(1), A_U32(2), A_U32(3)))
GL_REG(glBlitFramebuffer, 10, 0, {
    /* the cart blitting its picture into "framebuffer 0": the redirect, or
     * in direct mode the surface itself (its size still decides the mode) */
    if ((_direct ? _last_draw_fbo == _direct_target : _last_draw_fbo == _redirect_fbo) && (A_U32(8) & GL_COLOR_BUFFER_BIT)) {
        _cart_blitted_to_redirect = 1;
        // Track the cart's actual render size from the source rect
        // Use signed math — Ganesh may blit with Y-inverted coords
        int32_t sw = A_I32(2) - A_I32(0);
        int32_t sh = A_I32(3) - A_I32(1);
        if (sw < 0) sw = -sw;
        if (sh < 0) sh = -sh;
        if (sw > 0 && sh > 0) {
            _cart_blit_w = (uint32_t)sw;
            _cart_blit_h = (uint32_t)sh;
        }
    }
    glBlitFramebuffer(A_I32(0), A_I32(1), A_I32(2), A_I32(3), A_I32(4), A_I32(5), A_I32(6), A_I32(7), A_U32(8), A_U32(9));
})
// With a buffer on GL_PIXEL_PACK_BUFFER, pixels is an offset into it.
// Otherwise it is where in cart memory the pixels go (0 included).
GL_REG(glReadPixels, 7, 0, {
    GLsizei w = A_I32(2), h = A_I32(3);
    GLenum format = A_U32(4), type = A_U32(5);
    uint32_t p = A_U32(6);
    void* pixels = (void*)(uintptr_t)p;
    if (!_boundBuffer(GL_PIXEL_PACK_BUFFER_BINDING)) {
        NEED(p, _imageBytes(true, false, w, h, 1, format, type));
        pixels = wmem(p);
    }
    glReadPixels(A_I32(0), A_I32(1), w, h, format, type, pixels);
})

// ─── Renderbuffers ────────────────────────────────────────────────────────

GL_REG(glGenRenderbuffers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glGenRenderbuffers(A_I32(0), (GLuint*)wmem(A_U32(1))); })
GL_REG(glDeleteRenderbuffers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDeleteRenderbuffers(A_I32(0), (const GLuint*)wmem(A_U32(1))); })
GL_REG(glBindRenderbuffer, 2, 0, glBindRenderbuffer(A_U32(0), A_U32(1)))
GL_REG(glRenderbufferStorage, 4, 0, glRenderbufferStorage(A_U32(0), A_U32(1), A_I32(2), A_I32(3)))
GL_REG(glRenderbufferStorageMultisample, 5, 0, glRenderbufferStorageMultisample(A_U32(0), A_I32(1), A_U32(2), A_I32(3), A_I32(4)))

// ─── Samplers ─────────────────────────────────────────────────────────────

GL_REG(glGenSamplers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glGenSamplers(A_I32(0), (GLuint*)wmem(A_U32(1))); })
GL_REG(glDeleteSamplers, 2, 0, { NEED(A_U32(1), _nbytes(A_I32(0), 4)); glDeleteSamplers(A_I32(0), (const GLuint*)wmem(A_U32(1))); })
GL_REG(glBindSampler, 2, 0, glBindSampler(A_U32(0), A_U32(1)))
GL_REG(glSamplerParameteri, 3, 0, glSamplerParameteri(A_U32(0), A_U32(1), A_I32(2)))
GL_REG(glSamplerParameterf, 3, 0, glSamplerParameterf(A_U32(0), A_U32(1), A_F32(2)))

// ─── Sync ─────────────────────────────────────────────────────────────────

GL_REG(glFenceSync, 2, 1, { R_I32(_syncPut(glFenceSync(A_U32(0), A_U32(1)))); })
GL_REG(glDeleteSync, 1, 0, {
    uint32_t h = A_U32(0);
    GLsync s = _syncGet(h);
    if (!s) return;
    glDeleteSync(s);
    _syncs[h - 1] = NULL;
})
GL_REG(glClientWaitSync, 3, 1, {
    GLsync s = _syncGet(A_U32(0));
    if (!s) { R_I32(GL_WAIT_FAILED); return; }  // GL's answer for a bad sync
    R_I32(glClientWaitSync(s, A_U32(1), (GLuint64)A_I64(2)));
})

// ─── Buffer mapping ───────────────────────────────────────────────────────

GL_REG(glMapBufferRange, 4, 1, {
    void* ptr = glMapBufferRange(A_U32(0), A_I32(1), A_I32(2), A_U32(3));
    // Can't return a host pointer to WASM — return 0 (unsupported in WASM context)
    R_I32(0);
})
GL_REG(glUnmapBuffer, 1, 1, R_I32(glUnmapBuffer(A_U32(0))))

// ─── Extra attribs / params ───────────────────────────────────────────────

GL_REG(glVertexAttrib4fv, 2, 0, { NEED(A_U32(1), 16); glVertexAttrib4fv(A_U32(0), (const GLfloat*)wmem(A_U32(1))); })
// One value, or four for a colour or a swizzle. GL reads them from a copy, so
// a pname not listed here can't make it read past the cart's buffer.
GL_REG(glTexParameteriv, 3, 0, {
    GLenum pname = A_U32(1);
    uint64_t n = (pname == GL_TEXTURE_BORDER_COLOR || pname == 0x8E46 /* GL_TEXTURE_SWIZZLE_RGBA */) ? 4 : 1;
    GLint params[4] = {0, 0, 0, 0};
    NEED(A_U32(2), n * sizeof(GLint));
    memcpy(params, wmem(A_U32(2)), n * sizeof(GLint));
    glTexParameteriv(A_U32(0), pname, params);
})

// ─── GLES 3.1+ functions needed by Skia Ganesh ──────────────────────────

GL_REG(glMemoryBarrier, 1, 0, glMemoryBarrier(A_U32(0)))
GL_REG(glTexBuffer, 3, 0, glTexBuffer(A_U32(0), A_U32(1), A_U32(2)))
GL_REG(glTexBufferRange, 5, 0, glTexBufferRange(A_U32(0), A_U32(1), A_U32(2), A_I32(3), A_I32(4)))
GL_REG(glPatchParameteri, 2, 0, glPatchParameteri(A_U32(0), A_I32(1)))
// The indirect draws read their command from GL_DRAW_INDIRECT_BUFFER, and
// glDrawElementsIndirect its indices from the element buffer. ES 3.1 requires
// both and errors without them; a desktop compatibility context would instead
// read client memory at the cart's value, i.e. at a host address. So without
// them the draw is dropped, as ES would.
GL_REG(glDrawArraysIndirect, 2, 0, {
    if (!_boundBuffer(GL_DRAW_INDIRECT_BUFFER_BINDING)) return;
    glDrawArraysIndirect(A_U32(0), (const void*)(uintptr_t)A_U32(1));
})
GL_REG(glDrawElementsIndirect, 3, 0, {
    if (!_boundBuffer(GL_DRAW_INDIRECT_BUFFER_BINDING) ||
        !_boundBuffer(GL_ELEMENT_ARRAY_BUFFER_BINDING)) return;
    glDrawElementsIndirect(A_U32(0), A_U32(1), (const void*)(uintptr_t)A_U32(2));
})
GL_REG(glGetMultisamplefv, 3, 0, {
    _getInto<GLfloat>(_import, A_U32(2), 2, [&](GLfloat* out) { glGetMultisamplefv(A_U32(0), A_U32(1), out); });
})
GL_REG(glGetTexLevelParameteriv, 4, 0, {
    _getInto<GLint>(_import, A_U32(3), 1, [&](GLint* out) { glGetTexLevelParameteriv(A_U32(0), A_I32(1), A_U32(2), out); });
})
GL_REG(glBindFragDataLocation, 3, 0, { /* not in GLES — no-op */ })
GL_REG(glBindFragDataLocationIndexed, 4, 0, { /* not in GLES — no-op */ })
GL_REG(glBlendBarrier, 0, 0, glBlendBarrier())
GL_REG(glBlendBarrierKHR, 0, 0, glBlendBarrier())
GL_REG(glDiscardFramebufferEXT, 3, 0, { NEED(A_U32(2), _nbytes(A_I32(1), 4)); glInvalidateFramebuffer(A_U32(0), A_I32(1), (const GLenum*)wmem(A_U32(2))); })
GL_REG(glInvalidateFramebuffer, 3, 0, { NEED(A_U32(2), _nbytes(A_I32(1), 4)); glInvalidateFramebuffer(A_U32(0), A_I32(1), (const GLenum*)wmem(A_U32(2))); })

// Skia debug/label functions — no-op stubs
GL_REG(glDebugMessageCallback, 2, 0, { /* no-op */ })
GL_REG(glDebugMessageCallbackKHR, 2, 0, { /* no-op */ })
GL_REG(glDebugMessageControl, 6, 0, { /* no-op */ })
GL_REG(glDebugMessageControlKHR, 6, 0, { /* no-op */ })
GL_REG(glDebugMessageInsert, 6, 0, { /* no-op */ })
GL_REG(glDebugMessageInsertKHR, 6, 0, { /* no-op */ })
GL_REG(glGetDebugMessageLog, 8, 1, R_I32(0))
GL_REG(glGetDebugMessageLogKHR, 8, 1, R_I32(0))
GL_REG(glObjectLabel, 4, 0, { /* no-op */ })
GL_REG(glObjectLabelKHR, 4, 0, { /* no-op */ })
GL_REG(glPopDebugGroup, 0, 0, { /* no-op */ })
GL_REG(glPopDebugGroupKHR, 0, 0, { /* no-op */ })
GL_REG(glPushDebugGroup, 4, 0, { /* no-op */ })
GL_REG(glPushDebugGroupKHR, 4, 0, { /* no-op */ })
GL_REG(glWindowRectanglesEXT, 3, 0, { /* no-op — extension not available */ })

// Timer queries
GL_REG(glQueryCounterEXT, 2, 0, { /* no-op */ })
GL_REG(glGetQueryObjecti64v, 3, 0, { int64_t zero = 0; NEED(A_U32(2), 8); memcpy(wmem(A_U32(2)), &zero, 8); })
GL_REG(glGetQueryObjecti64vEXT, 3, 0, { int64_t zero = 0; NEED(A_U32(2), 8); memcpy(wmem(A_U32(2)), &zero, 8); })
GL_REG(glGetQueryObjectui64v, 3, 0, { uint64_t zero = 0; NEED(A_U32(2), 8); memcpy(wmem(A_U32(2)), &zero, 8); })
GL_REG(glGetQueryObjectui64vEXT, 3, 0, { uint64_t zero = 0; NEED(A_U32(2), 8); memcpy(wmem(A_U32(2)), &zero, 8); })

// Multi-draw indirect
GL_REG(glMultiDrawArraysIndirect, 4, 0, { /* no-op — fallback to individual draws */ })
GL_REG(glMultiDrawArraysIndirectEXT, 4, 0, { /* no-op */ })
GL_REG(glMultiDrawElementsIndirect, 5, 0, { /* no-op */ })
GL_REG(glMultiDrawElementsIndirectEXT, 5, 0, { /* no-op */ })

// Instance drawing with base
GL_REG(glDrawArraysInstancedBaseInstance, 5, 0, { /* not in GLES — no-op */ })
GL_REG(glDrawArraysInstancedBaseInstanceEXT, 5, 0, { /* no-op */ })
GL_REG(glDrawElementsInstancedBaseVertexBaseInstance, 7, 0, { /* no-op */ })
GL_REG(glDrawElementsInstancedBaseVertexBaseInstanceEXT, 7, 0, { /* no-op */ })

// Texture clear
GL_REG(glClearTexImage, 5, 0, { /* not in GLES — no-op */ })
GL_REG(glClearTexImageEXT, 5, 0, { /* no-op */ })
GL_REG(glClearTexSubImage, 9, 0, { /* not in GLES — no-op */ })
GL_REG(glClearTexSubImageEXT, 9, 0, { /* no-op */ })

// Texture barrier
GL_REG(glTextureBarrier, 0, 0, { /* no-op */ })
GL_REG(glTextureBarrierNV, 0, 0, { /* no-op */ })

// Map buffer (OES variant)
GL_REG(glMapBufferOES, 2, 1, R_I32(0))

// ─── Registration table ───────────────────────────────────────────────────

typedef void (*v8_gl_callback_t)(const v8::FunctionCallbackInfo<v8::Value>&);

typedef struct {
    const char* name;
    v8_gl_callback_t cb;
    const char* sig; // kept for reference, not used in V8 registration
} gl_import_entry_t;


// Signature format: "iiff>i" means 2 i32 params, 2 f32 params, returns i32
// "iiii>" means 4 i32 params, void return
#define GL_E(name, sig_str) { #name, _cb_##name, sig_str }

static const gl_import_entry_t gl_table[] = {
    // State
    GL_E(glEnable, "i>"), GL_E(glDisable, "i>"), GL_E(glGetError, ">i"),
    GL_E(glFinish, ">"), GL_E(glFlush, ">"), GL_E(glHint, "ii>"),
    GL_E(glPixelStorei, "ii>"), GL_E(glIsEnabled, "i>i"),
    GL_E(glGetIntegerv, "ii>"), GL_E(glGetFloatv, "ii>"),
    GL_E(glGetBooleanv, "ii>"), GL_E(glGetString, "i>i"),
    GL_E(glGetInternalformativ, "iiiii>"),
    // Viewport/clear
    GL_E(glViewport, "iiii>"), GL_E(glScissor, "iiii>"), GL_E(glClear, "i>"),
    GL_E(glClearColor, "ffff>"), GL_E(glClearDepthf, "f>"), GL_E(glClearStencil, "i>"),
    // Blending
    GL_E(glBlendFunc, "ii>"), GL_E(glBlendFuncSeparate, "iiii>"),
    GL_E(glBlendEquation, "i>"), GL_E(glBlendEquationSeparate, "ii>"),
    GL_E(glBlendColor, "ffff>"), GL_E(glColorMask, "iiii>"),
    // Depth/stencil
    GL_E(glDepthFunc, "i>"), GL_E(glDepthMask, "i>"), GL_E(glDepthRangef, "ff>"),
    GL_E(glStencilFunc, "iii>"), GL_E(glStencilFuncSeparate, "iiii>"),
    GL_E(glStencilOp, "iii>"), GL_E(glStencilOpSeparate, "iiii>"),
    GL_E(glStencilMask, "i>"), GL_E(glStencilMaskSeparate, "ii>"),
    // Face culling
    GL_E(glCullFace, "i>"), GL_E(glFrontFace, "i>"),
    GL_E(glPolygonOffset, "ff>"), GL_E(glLineWidth, "f>"),
    // Buffers
    GL_E(glGenBuffers, "ii>"), GL_E(glDeleteBuffers, "ii>"),
    GL_E(glBindBuffer, "ii>"), GL_E(glBufferData, "iiii>"),
    GL_E(glBufferSubData, "iiii>"), GL_E(glIsBuffer, "i>i"),
    // Textures
    GL_E(glGenTextures, "ii>"), GL_E(glDeleteTextures, "ii>"),
    GL_E(glBindTexture, "ii>"), GL_E(glActiveTexture, "i>"),
    GL_E(glTexParameteri, "iii>"), GL_E(glTexParameterf, "iif>"),
    GL_E(glGenerateMipmap, "i>"), GL_E(glIsTexture, "i>i"),
    GL_E(glTexImage2D, "iiiiiiiii>"), GL_E(glTexSubImage2D, "iiiiiiiii>"),
    GL_E(glCompressedTexImage2D, "iiiiiiii>"), GL_E(glCopyTexSubImage2D, "iiiiiiii>"),
    GL_E(glCompressedTexSubImage2D, "iiiiiiiii>"),
    GL_E(glTexImage3D, "iiiiiiiiii>"), GL_E(glTexStorage2D, "iiiii>"), GL_E(glTexStorage3D, "iiiiii>"),
    // Shaders
    GL_E(glCreateShader, "i>i"), GL_E(glDeleteShader, "i>"),
    GL_E(glCompileShader, "i>"), GL_E(glIsShader, "i>i"),
    GL_E(glShaderSource, "iiii>"), GL_E(glGetShaderiv, "iii>"),
    GL_E(glGetShaderInfoLog, "iiii>"),
    // Programs
    GL_E(glCreateProgram, ">i"), GL_E(glDeleteProgram, "i>"),
    GL_E(glAttachShader, "ii>"), GL_E(glDetachShader, "ii>"),
    GL_E(glLinkProgram, "i>"), GL_E(glUseProgram, "i>"),
    GL_E(glIsProgram, "i>i"), GL_E(glValidateProgram, "i>"),
    GL_E(glGetProgramiv, "iii>"), GL_E(glGetProgramInfoLog, "iiii>"),
    GL_E(glBindAttribLocation, "iii>"), GL_E(glGetAttribLocation, "ii>i"),
    GL_E(glGetUniformLocation, "ii>i"),
    GL_E(glGetActiveAttrib, "iiiiiii>"), GL_E(glGetActiveUniform, "iiiiiii>"),
    // Uniforms
    GL_E(glUniform1i, "ii>"), GL_E(glUniform2i, "iii>"),
    GL_E(glUniform3i, "iiii>"), GL_E(glUniform4i, "iiiii>"),
    GL_E(glUniform1f, "if>"), GL_E(glUniform2f, "iff>"),
    GL_E(glUniform3f, "ifff>"), GL_E(glUniform4f, "iffff>"),
    GL_E(glUniform1iv, "iii>"), GL_E(glUniform2iv, "iii>"),
    GL_E(glUniform3iv, "iii>"), GL_E(glUniform4iv, "iii>"),
    GL_E(glUniform1fv, "iii>"), GL_E(glUniform2fv, "iii>"),
    GL_E(glUniform3fv, "iii>"), GL_E(glUniform4fv, "iii>"),
    GL_E(glUniformMatrix2fv, "iiii>"), GL_E(glUniformMatrix3fv, "iiii>"),
    GL_E(glUniformMatrix4fv, "iiii>"),
    // Vertex attributes
    GL_E(glEnableVertexAttribArray, "i>"), GL_E(glDisableVertexAttribArray, "i>"),
    GL_E(glVertexAttribPointer, "iiiiii>"), GL_E(glVertexAttribIPointer, "iiiii>"),
    GL_E(glVertexAttribDivisor, "ii>"),
    // VAOs
    GL_E(glGenVertexArrays, "ii>"), GL_E(glDeleteVertexArrays, "ii>"),
    GL_E(glBindVertexArray, "i>"),
    // Drawing
    GL_E(glDrawArrays, "iii>"), GL_E(glDrawElements, "iiii>"),
    GL_E(glDrawArraysInstanced, "iiii>"), GL_E(glDrawElementsInstanced, "iiiii>"),
    GL_E(glDrawBuffers, "ii>"), GL_E(glReadBuffer, "i>"),
    GL_E(glClearBufferfv, "iii>"),
    // GLES3 functions needed by Godot
    GL_E(glGetStringi, "ii>i"), GL_E(glGetInteger64v, "ii>"),
    GL_E(glBindBufferBase, "iii>"), GL_E(glBindBufferRange, "iiiii>"),
    GL_E(glGetUniformBlockIndex, "ii>i"), GL_E(glUniformBlockBinding, "iii>"),
    GL_E(glGetActiveUniformBlockiv, "iiii>"), GL_E(glGetActiveUniformsiv, "iiiii>"),
    GL_E(glUniform1ui, "ii>"), GL_E(glUniform1uiv, "iii>"),
    GL_E(glVertexAttribI4ui, "iiiii>"),
    GL_E(glCopyBufferSubData, "iiiii>"),
    GL_E(glFramebufferTextureLayer, "iiiii>"),
    GL_E(glTexSubImage3D, "iiiiiiiiiii>"),
    GL_E(glCompressedTexImage3D, "iiiiiiiii>"),
    GL_E(glCompressedTexSubImage3D, "iiiiiiiiiii>"),
    GL_E(glBeginTransformFeedback, "i>"), GL_E(glEndTransformFeedback, ">"),
    GL_E(glTransformFeedbackVaryings, "iiii>"),
    GL_E(glGetSynciv, "iiiii>"),
    GL_E(glGenQueries, "ii>"), GL_E(glDeleteQueries, "ii>"),
    // Framebuffers
    GL_E(glGenFramebuffers, "ii>"), GL_E(glDeleteFramebuffers, "ii>"),
    GL_E(glBindFramebuffer, "ii>"), GL_E(glCheckFramebufferStatus, "i>i"),
    GL_E(glFramebufferTexture2D, "iiiii>"), GL_E(glFramebufferRenderbuffer, "iiii>"),
    GL_E(glBlitFramebuffer, "iiiiiiiiii>"), GL_E(glReadPixels, "iiiiiii>"),
    // Renderbuffers
    GL_E(glGenRenderbuffers, "ii>"), GL_E(glDeleteRenderbuffers, "ii>"),
    GL_E(glBindRenderbuffer, "ii>"), GL_E(glRenderbufferStorage, "iiii>"),
    GL_E(glRenderbufferStorageMultisample, "iiiii>"),
    // Samplers
    GL_E(glGenSamplers, "ii>"), GL_E(glDeleteSamplers, "ii>"),
    GL_E(glBindSampler, "ii>"), GL_E(glSamplerParameteri, "iii>"),
    GL_E(glSamplerParameterf, "iif>"),
    // Sync
    GL_E(glFenceSync, "ii>i"), GL_E(glDeleteSync, "i>"),
    GL_E(glClientWaitSync, "iil>i"),  // i32, i32, i64 timeout, result i32
    // Buffer mapping
    GL_E(glMapBufferRange, "iiii>i"), GL_E(glUnmapBuffer, "i>i"),
    // Extra attribs/params
    GL_E(glVertexAttrib4fv, "ii>"), GL_E(glTexParameteriv, "iii>"),
    // GLES 3.1+ / Skia Ganesh
    GL_E(glMemoryBarrier, "i>"), GL_E(glTexBuffer, "iii>"), GL_E(glTexBufferRange, "iiiii>"),
    GL_E(glPatchParameteri, "ii>"),
    GL_E(glDrawArraysIndirect, "ii>"), GL_E(glDrawElementsIndirect, "iii>"),
    GL_E(glGetMultisamplefv, "iii>"), GL_E(glGetTexLevelParameteriv, "iiii>"),
    GL_E(glBindFragDataLocation, "iii>"), GL_E(glBindFragDataLocationIndexed, "iiii>"),
    GL_E(glBlendBarrier, ">"), GL_E(glBlendBarrierKHR, ">"),
    GL_E(glDiscardFramebufferEXT, "iii>"), GL_E(glInvalidateFramebuffer, "iii>"),
    // Debug
    GL_E(glDebugMessageCallback, "ii>"), GL_E(glDebugMessageCallbackKHR, "ii>"),
    GL_E(glDebugMessageControl, "iiiiii>"), GL_E(glDebugMessageControlKHR, "iiiiii>"),
    GL_E(glDebugMessageInsert, "iiiiii>"), GL_E(glDebugMessageInsertKHR, "iiiiii>"),
    GL_E(glGetDebugMessageLog, "iiiiiiii>i"), GL_E(glGetDebugMessageLogKHR, "iiiiiiii>i"),
    GL_E(glObjectLabel, "iiii>"), GL_E(glObjectLabelKHR, "iiii>"),
    GL_E(glPopDebugGroup, ">"), GL_E(glPopDebugGroupKHR, ">"),
    GL_E(glPushDebugGroup, "iiii>"), GL_E(glPushDebugGroupKHR, "iiii>"),
    GL_E(glWindowRectanglesEXT, "iii>"),
    // Timer queries
    GL_E(glQueryCounterEXT, "ii>"),
    GL_E(glGetQueryObjecti64v, "iii>"), GL_E(glGetQueryObjecti64vEXT, "iii>"),
    GL_E(glGetQueryObjectui64v, "iii>"), GL_E(glGetQueryObjectui64vEXT, "iii>"),
    // Multi-draw indirect
    GL_E(glMultiDrawArraysIndirect, "iiii>"), GL_E(glMultiDrawArraysIndirectEXT, "iiii>"),
    GL_E(glMultiDrawElementsIndirect, "iiiii>"), GL_E(glMultiDrawElementsIndirectEXT, "iiiii>"),
    // Instance base
    GL_E(glDrawArraysInstancedBaseInstance, "iiiii>"), GL_E(glDrawArraysInstancedBaseInstanceEXT, "iiiii>"),
    GL_E(glDrawElementsInstancedBaseVertexBaseInstance, "iiiiiii>"), GL_E(glDrawElementsInstancedBaseVertexBaseInstanceEXT, "iiiiiii>"),
    // Texture clear/barrier
    GL_E(glClearTexImage, "iiiii>"), GL_E(glClearTexImageEXT, "iiiii>"),
    GL_E(glClearTexSubImage, "iiiiiiiii>"), GL_E(glClearTexSubImageEXT, "iiiiiiiii>"),
    GL_E(glTextureBarrier, ">"), GL_E(glTextureBarrierNV, ">"),
    // Map buffer OES
    GL_E(glMapBufferOES, "ii>i"),
    // End
    { NULL, NULL, NULL }
};

// ─── Registration ─────────────────────────────────────────────────────────

extern "C" int wc_gl_has_redirect(void) { return _redirect_fbo != 0; }

// Blit redirect FBO to a specific target FBO (for libretro — target is RetroArch's FBO)
// flip_y: when true, flips the image vertically during blit (needed for RetroArch GLES contexts)
extern "C" void wc_gl_blit_to_fbo(uint32_t target_fbo, uint32_t cart_w, uint32_t cart_h, uint32_t dst_w, uint32_t dst_h, int flip_y) {
    if (!_redirect_fbo) return;
    if (_direct && !flip_y && target_fbo == _direct_target) {
        /* the cart drew into target_fbo itself: nothing to copy */
        _cart_blitted_to_redirect = 0;
        _draw_call_count = 0;
        return;
    }
    uint32_t src_w = _cart_blit_w ? _cart_blit_w : cart_w;
    uint32_t src_h = _cart_blit_h ? _cart_blit_h : cart_h;

    /* Like wc_gl_blit_to_screen, this must be invisible to the cart: the
     * frontend saves the cart's GL state AFTER this runs (libretro), and an
     * engine that caches its clear colour or scissor enable (three.c does)
     * would otherwise clear with our black on every later frame. */
    GLfloat cart_clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cart_clear);
    GLboolean cart_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint cart_vp[4];
    glGetIntegerv(GL_VIEWPORT, cart_vp);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _redirect_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target_fbo);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, src_w, src_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (flip_y) {
        // Flip vertically: src bottom-left origin → dst top-left origin
        glBlitFramebuffer(0, 0, src_w, src_h, 0, src_h, src_w, 0,
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        glBlitFramebuffer(0, 0, src_w, src_h, 0, 0, src_w, src_h,
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

    glClearColor(cart_clear[0], cart_clear[1], cart_clear[2], cart_clear[3]);
    if (cart_scissor) glEnable(GL_SCISSOR_TEST);

    // Restore redirect FBO for next frame, and the CART's viewport (not the
    // redirect's size: an engine caching its viewport would keep drawing
    // across the whole redirect when that is larger than the cart)
    glBindFramebuffer(GL_FRAMEBUFFER, _redirect_fbo);
    glViewport(cart_vp[0], cart_vp[1], cart_vp[2], cart_vp[3]);
    _cart_blitted_to_redirect = 0;
    _last_draw_fbo = _redirect_fbo;
    _draw_call_count = 0;
}

extern "C" void wc_gl_rebind_redirect(void) {
    if (!_redirect_fbo) return;
    glBindFramebuffer(GL_FRAMEBUFFER, _direct ? _direct_target : _redirect_fbo);
    glViewport(0, 0, _redirect_w, _redirect_h);
    // Put back the VAO the cart last bound (its element buffer lives there).
    // Carts that never bind a VAO (gl4es) get _cart_vao: Core 3.3 requires a
    // non-zero VAO for draw calls, else GL_INVALID_OPERATION.
    GLuint vao = _cart_bound_vao ? _cart_bound_vao : _cart_vao;
    if (vao) glBindVertexArray(vao);
    _last_draw_fbo = _direct ? _direct_target : _redirect_fbo;
    _cart_blitted_to_redirect = 0;
    _draw_call_count = 0;
}

/* The framebuffer direct mode draws into (0 = the window surface). Set
 * before wc_gl_set_direct, and again whenever the frontend's changes. */
extern "C" void wc_gl_set_direct_target(uint32_t fbo) {
    _direct_target = fbo;
    if (_direct) {
        glBindFramebuffer(GL_FRAMEBUFFER, _direct_target);
        _last_draw_fbo = _direct_target;
    }
}

/* Switch direct present on or off, between frames. Leaves the cart's
 * "framebuffer 0" bound (the surface or the redirect), so a cart that never
 * rebinds still draws into the right one. */
extern "C" void wc_gl_set_direct(int on) {
    on = on ? 1 : 0;
    if (on == _direct || !_redirect_fbo) return;
    _direct = on;
    glBindFramebuffer(GL_FRAMEBUFFER, _direct ? _direct_target : _redirect_fbo);
    _last_draw_fbo = _direct ? _direct_target : _redirect_fbo;
    _cart_blitted_to_redirect = 0;
}
extern "C" int wc_gl_is_direct(void) { return _direct; }

/* Read the frame the cart just drew, w x h RGBA bottom-up, from where it drew
 * it: the surface in direct mode (before the swap, while it is defined), else
 * the redirect FBO. For tests (--shot). Leaves the cart's binding in place. */
extern "C" int wc_gl_read_frame(uint8_t* out, uint32_t w, uint32_t h) {
    if (!_redirect_fbo && !_direct) return 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _direct ? _direct_target : _redirect_fbo);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindFramebuffer(GL_FRAMEBUFFER, _direct ? _direct_target : _redirect_fbo);
    return 1;
}

extern "C" void wc_gl_get_blit_size(uint32_t* w, uint32_t* h) {
    *w = _cart_blit_w;
    *h = _cart_blit_h;
}

extern "C" int wc_gl_get_redirect_fbo(void) { return _redirect_fbo; }

// Set redirect to an external FBO (e.g. RetroArch's hw_render FBO).
// Cart's glBindFramebuffer(0) will bind this FBO directly — no intermediate blit needed.
extern "C" void wc_gl_set_redirect_fbo(uint32_t fbo, uint32_t width, uint32_t height) {
    _redirect_fbo = fbo;
    _redirect_w = width;
    _redirect_h = height;
    _last_draw_fbo = fbo;
}

// Resolve all GL entry points from the host's loader, once, from whichever
// site first has a loader available. The host (libretro core or native) sets
// host->gl_loader; for the core that is RetroArch's get_proc_address (not
// available until context_reset), while the standalone hosts set it before
// wc_host_load_file. Every _cb_gl* shim jumps through these pointers with no
// null check, so they MUST be resolved before any cart code can call GL — a
// cart that touches GL during _initialize (love.load building canvases)
// otherwise jumps to 0 and takes the process down.
static int s_gl_procs_loaded = 0;
static void _load_gl_procs_once(void) {
    if (s_gl_procs_loaded || !_host || !_host->gl_loader) return;
    wc_gl_procs_load((void*)_host->gl_loader);
    s_gl_procs_loaded = 1;
    if (!p_glGenFramebuffers)
        wc_log("wasmcart: WARNING host gl_loader returned no glGenFramebuffers\n");
}

extern "C" void wc_gl_setup_redirect(uint32_t width, uint32_t height) {
    _load_gl_procs_once();
    _ensure_redirect_fbo(width, height);
}

extern "C" void wc_gl_blit_to_screen(uint32_t cart_w, uint32_t cart_h, uint32_t win_w, uint32_t win_h) {
    if (!_redirect_fbo) return;
    if (_direct) {
        /* the cart drew straight onto the surface: nothing to copy */
        _cart_blitted_to_redirect = 0;
        _draw_call_count = 0;
        return;
    }
    // Use the cart's actual blit size if available (e.g. Godot renders 640x360 into 640x480 FBO)
    uint32_t src_w = _cart_blit_w ? _cart_blit_w : cart_w;
    uint32_t src_h = _cart_blit_h ? _cart_blit_h : cart_h;

    // Calculate letterboxed destination rect (preserve aspect ratio)
    float src_aspect = (float)src_w / (float)src_h;
    float win_aspect = (float)win_w / (float)win_h;
    int dst_x, dst_y, dst_w, dst_h;
    if (win_aspect > src_aspect) {
        // Window is wider — pillarbox (bars on sides)
        dst_h = win_h;
        dst_w = (int)(win_h * src_aspect);
        dst_x = (win_w - dst_w) / 2;
        dst_y = 0;
    } else {
        // Window is taller — letterbox (bars on top/bottom)
        dst_w = win_w;
        dst_h = (int)(win_w / src_aspect);
        dst_x = 0;
        dst_y = (win_h - dst_h) / 2;
    }

    // Use RAW GL calls — not our intercepted versions
    GLboolean cart_scissor = glIsEnabled(GL_SCISSOR_TEST); /* restored below, like the clear colour */
    GLint cart_vp[4];
    glGetIntegerv(GL_VIEWPORT, cart_vp);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, _redirect_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);  // real FBO 0 = EGL surface
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, win_w, win_h);
    // The letterbox clear must not leak into cart-visible GL state: engines
    // cache glClearColor across frames (wasmcart-lua r2d only re-sets it when
    // the cart's background CHANGES), so leaving black here turns every
    // subsequent cart clear black. Save and restore around our clear.
    GLfloat cart_clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cart_clear);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, src_w, src_h,
        dst_x, dst_y, dst_x + dst_w, dst_y + dst_h,
        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glClearColor(cart_clear[0], cart_clear[1], cart_clear[2], cart_clear[3]);
    if (cart_scissor) glEnable(GL_SCISSOR_TEST);

    // Restore redirect FBO + the cart's own viewport for next frame
    glBindFramebuffer(GL_FRAMEBUFFER, _redirect_fbo);
    glViewport(cart_vp[0], cart_vp[1], cart_vp[2], cart_vp[3]);
    _cart_blitted_to_redirect = 0;
    _last_draw_fbo = _redirect_fbo;
    _draw_call_count = 0;
}

// ─── Upload 2D framebuffer to redirect FBO (unified GL display path) ────────

static void _init_fb_upload(void) {
    if (_fb_upload_program) return;

    const char* vs_src =
        "#version 300 es\n"
        "out vec2 vUV;\n"
        "void main() {\n"
        "  float x = float((gl_VertexID & 1) << 2) - 1.0;\n"
        "  float y = float((gl_VertexID & 2) << 1) - 1.0;\n"
        "  vUV = vec2((x + 1.0) * 0.5, 1.0 - (y + 1.0) * 0.5);\n"
        "  gl_Position = vec4(x, y, 0.0, 1.0);\n"
        "}\n";
    const char* fs_src =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  fragColor = texture(uTex, vUV);\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);

    _fb_upload_program = glCreateProgram();
    glAttachShader(_fb_upload_program, vs);
    glAttachShader(_fb_upload_program, fs);
    glLinkProgram(_fb_upload_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenVertexArrays(1, &_fb_upload_vao);
    glGenTextures(1, &_fb_upload_tex);
    glBindTexture(GL_TEXTURE_2D, _fb_upload_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

extern "C" void wc_gl_upload_framebuffer(const uint8_t* pixels, uint32_t w, uint32_t h) {
    if (!_redirect_fbo || !pixels || w == 0 || h == 0) return;
    _init_fb_upload();
    if (!_fb_upload_program) return;

    // Upload pixels as texture (ARGB8888 = BGRA byte order on little-endian)
    glBindTexture(GL_TEXTURE_2D, _fb_upload_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Draw to redirect FBO at cart's framebuffer size
    glBindFramebuffer(GL_FRAMEBUFFER, _redirect_fbo);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glUseProgram(_fb_upload_program);
    glBindVertexArray(_fb_upload_vao);
    glActiveTexture(GL_TEXTURE0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Track blit size so downstream blit uses cart's actual resolution
    _cart_blit_w = w;
    _cart_blit_h = h;
    _cart_blitted_to_redirect = 1;
    glViewport(0, 0, _redirect_w, _redirect_h);
}

extern "C" void wc_gl_imports_init(wc_host_t* host) {
    _host = host;
    wc_log( "wasmcart: GL imports registered (%s)\n",
        (const char*)glGetString(GL_RENDERER));
}

// Called from cart_host.cpp to populate the GL import objects for V8
extern "C" void wc_gl_build_v8_imports(v8::Isolate* isolate, v8::Local<v8::Context> context,
    v8::Local<v8::Object> gl_obj, v8::Local<v8::Object> env_obj, wc_host_t* host) {
    _host = host;

    int idx = 0;
    for (const gl_import_entry_t* e = gl_table; e->name; e++, idx++) {
        auto fn = v8::Function::New(context, e->cb).ToLocalChecked();
        auto name = v8::String::NewFromUtf8(isolate, e->name).ToLocalChecked();
        gl_obj->Set(context, name, fn).Check();
        env_obj->Set(context, name, fn).Check();
    }

    // Resolve procs NOW if the host already provided a loader (standalone
    // hosts do, before wc_host_load_file) so the cart can never call an
    // unresolved GL shim during _initialize. For the libretro core the loader
    // doesn't exist until context_reset, so this no-ops and the
    // wc_gl_setup_redirect call on context_reset resolves them instead —
    // safe there because the frontend never runs the cart before that.
    _load_gl_procs_once();
    const char* renderer = p_glGetString ? (const char*)glGetString(GL_RENDERER) : "(GL not ready)";
    wc_log( "wasmcart: GL imports registered (%d functions, %s)\n", idx, renderer);
}
