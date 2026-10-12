/* Exercise the bridge against Mesa's real software GL driver. This establishes
 * GL procedure/result semantics, not hardware GPU performance evidence. */
#define _POSIX_C_SOURCE 200809L
#include "gl_timer.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void* load_proc(const char* name) {
    void* proc = (void*)eglGetProcAddress(name);
    return proc ? proc : dlsym(RTLD_DEFAULT, name);
}
static uint64_t monotonic_ns(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value)) return 0;
    return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}
static int wait_available(GLuint id, unsigned* polls, GLuint* first, uint64_t* longest_poll_ns) {
    /* A finite test deadline with sleeps. Production profiling never waits. */
    uint64_t deadline = monotonic_ns() + 5000000000ULL;
    struct timespec pause = {0, 1000000};
    *polls = 0;
    do {
        GLuint ready = 0;
        uint64_t started = monotonic_ns();
        int valid = wc_gl_timer_checked_get_query_object_uiv(id, GL_QUERY_RESULT_AVAILABLE_EXT, &ready);
        uint64_t duration = monotonic_ns() - started;
        if (duration > *longest_poll_ns) *longest_poll_ns = duration;
        if (!(*polls)++) *first = ready;
        if (!valid) return 0;
        if (ready) return 1;
        nanosleep(&pause, NULL);
    } while (monotonic_ns() < deadline);
    return 0;
}
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (EGL error 0x%x)\n", message, eglGetError()); goto cleanup; } \
} while (0)

int main(int argc, char** argv) {
    int desktop = argc == 2 && !strcmp(argv[1], "--desktop");
    int result = 1, owns_queries = 0;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint ids[3] = {0, 0, 0};
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(display != EGL_NO_DISPLAY, "surfaceless EGL display");
    EGLint major = 0, minor = 0;
    CHECK(eglInitialize(display, &major, &minor), "initialize EGL");
    CHECK(eglBindAPI(desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API), "bind GL API");
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, /* No window requirement. */
        EGL_RENDERABLE_TYPE, desktop ? EGL_OPENGL_BIT : EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint config_count = 0;
    CHECK(eglChooseConfig(display, config_attributes, &config, 1, &config_count) && config_count == 1,
          "choose pbuffer-only configuration");
    const EGLint es_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const EGLint desktop_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_NONE
    };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, desktop ? desktop_attributes : es_attributes);
    CHECK(context != EGL_NO_CONTEXT, "create GL context");
    const EGLint surface_attributes[] = {EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
    surface = eglCreatePbufferSurface(display, config, surface_attributes);
    CHECK(surface != EGL_NO_SURFACE, "create 8x8 pbuffer");
    CHECK(eglMakeCurrent(display, surface, surface, context), "make GL context current");
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    CHECK(renderer && strstr(renderer, "llvmpipe"), "software llvmpipe renderer");
    printf("driver: %s; %s; EGL %d.%d\n", renderer, (const char*)glGetString(GL_VERSION), major, minor);
    wc_gl_timer_load((void*)load_proc);
    wc_gl_timer_caps_t caps = wc_gl_timer_caps();
    printf("timer capabilities: elapsed_bits=%d timestamp_bits=%d disjoint=%d\n",
           caps.elapsed_bits, caps.timestamp_bits, caps.disjoint);
    CHECK(caps.timestamp_bits == 64 && caps.elapsed_bits > 0, "real driver supports useful timer queries");
    CHECK(caps.disjoint == !desktop, "GLES disjoint and desktop timer semantics");
    CHECK(wc_gl_timer_extension_visible(desktop ? "GL_ARB_timer_query" : "GL_EXT_disjoint_timer_query"),
          "complete actual procedure family is visible");
    GLint bits = 0;
    CHECK(wc_gl_timer_checked_get_query_iv(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits) && bits == caps.timestamp_bits,
          "driver counter width matches capability");
    if (caps.disjoint) {
        GLint flag = -1;
        CHECK(wc_gl_timer_checked_get_disjoint(&flag) && flag == 0, "initial host disjoint check");
        CHECK(wc_gl_timer_get_disjoint(&flag) && flag == 0, "initial guest disjoint check");
    }
    CHECK(wc_gl_timer_checked_gen_queries(3, ids), "generate real query objects");
    owns_queries = 1;
    /* Host-owned validation must retain an unread real guest GL error. */
    glEnable(0xdeadbeefU);
    CHECK(wc_gl_timer_checked_query_counter(ids[0], GL_TIMESTAMP_EXT), "start timestamp preserves guest GL error");
    CHECK(wc_gl_timer_get_error() == GL_INVALID_ENUM && wc_gl_timer_get_error() == GL_NO_ERROR,
          "guest retrieves its original driver error exactly once");
    glViewport(0, 0, 8, 8);
    for (unsigned i = 0; i < 4096; ++i) {
        glClearColor((GLfloat)(i & 255) / 255.0f, 0.25f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    CHECK(wc_gl_timer_checked_query_counter(ids[1], GL_TIMESTAMP_EXT), "end timestamp");
    CHECK(wc_gl_timer_checked_begin_query(GL_TIME_ELAPSED_EXT, ids[2]), "begin elapsed query");
    for (unsigned i = 0; i < 128; ++i) glClear(GL_COLOR_BUFFER_BIT);
    CHECK(wc_gl_timer_checked_end_query(GL_TIME_ELAPSED_EXT), "end elapsed query");
    glFlush(); /* Submit commands; never glFinish or retrieve an unready result. */
    unsigned polls[3];
    GLuint first[3];
    uint64_t longest_poll_ns = 0;
    for (unsigned i = 0; i < 3; ++i)
        CHECK(wait_available(ids[i], &polls[i], &first[i], &longest_poll_ns), "queries complete within test deadline");
    if (caps.disjoint) {
        GLint flag = -1;
        CHECK(wc_gl_timer_checked_get_disjoint(&flag) && flag == 0, "ready results are not disjoint");
        CHECK(wc_gl_timer_get_disjoint(&flag) && flag == 0, "guest flag is preserved independently");
        CHECK(wc_gl_timer_checked_get_disjoint(&flag) && flag == 0, "second host disjoint read stays clear");
    }
    GLuint64 timestamps[2] = {0, 0}, elapsed = 0;
    for (unsigned i = 0; i < 2; ++i)
        CHECK(wc_gl_timer_checked_get_query_object_ui64v(ids[i], GL_QUERY_RESULT_EXT, timestamps + i),
              "read available unsigned 64-bit timestamp");
    CHECK(wc_gl_timer_checked_get_query_object_ui64v(ids[2], GL_QUERY_RESULT_EXT, &elapsed), "read available elapsed result");
    CHECK(timestamps[0] > UINT32_MAX && timestamps[1] > timestamps[0] && elapsed > 0,
          "real timestamps retain high bits and advance across submitted workload");
    GLint64 signed_timestamp = 0;
    CHECK(wc_gl_timer_get_query_object_i64v(ids[0], GL_QUERY_RESULT_EXT, &signed_timestamp) &&
          (GLuint64)signed_timestamp == timestamps[0], "signed result uses real 64-bit driver readback");
    printf("query results: start=%" PRIu64 " end=%" PRIu64 " interval_ns=%" PRIu64 " elapsed_ns=%" PRIu64 "\n",
           (uint64_t)timestamps[0], (uint64_t)timestamps[1], (uint64_t)(timestamps[1] - timestamps[0]), (uint64_t)elapsed);
    printf("availability: first=%u,%u,%u polls=%u,%u,%u longest_poll_ns=%" PRIu64 "\n",
           first[0], first[1], first[2], polls[0], polls[1], polls[2], longest_poll_ns);
    CHECK(!wc_gl_timer_checked_query_counter(0, GL_TIMESTAMP_EXT), "invalid native query ID is rejected");
    CHECK(wc_gl_timer_get_error() == GL_NO_ERROR, "profiler error does not leak to guest");
    CHECK(!wc_gl_timer_query_counter(ids[0], GL_TIME_ELAPSED_EXT), "invalid timestamp target is rejected");
    CHECK(wc_gl_timer_get_error() == GL_INVALID_ENUM && wc_gl_timer_get_error() == GL_NO_ERROR,
          "unsupported guest call produces a GL error without fake success");
    CHECK(wc_gl_timer_checked_delete_queries(3, ids), "delete real query objects");
    owns_queries = 0;
    for (unsigned i = 0; i < 3; ++i) CHECK(!wc_gl_timer_is_query(ids[i]), "query deletion removes driver objects");
    CHECK(wc_gl_timer_get_error() == GL_NO_ERROR, "no remaining guest GL errors");
    puts("PASS: real EGL timer-query capabilities, asynchronous 64-bit readback, errors and cleanup");
    result = 0;
cleanup:
    if (owns_queries) wc_gl_timer_checked_delete_queries(3, ids);
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglTerminate(display);
    }
    return result;
}
