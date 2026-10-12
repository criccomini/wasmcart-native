/* No GL library/context needed: test the actual loader and dispatch bridge
 * against a driver with independent completion, error and disjoint state. */
#include "gl_timer.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int mode, elapsed_bits, timestamp_bits, disjoint, disjoint_reads;
static int begins, ends, deletes, result_reads, fail_counter, fail_bits;
static const char* missing;
static GLenum errors[16];
static unsigned error_count;
typedef struct { int exists, ready; GLuint64 result; } query_t;
static query_t queries[64];
static GLuint next_id, active;

static void driver_error(GLenum value) { errors[error_count++] = value; }
static GLenum GL_APIENTRY fake_error(void) {
    if (!error_count) return GL_NO_ERROR;
    GLenum error = errors[0];
    memmove(errors, errors + 1, --error_count * sizeof *errors);
    return error;
}
static const GLubyte* GL_APIENTRY fake_string(GLenum pname) {
    if (pname == GL_VERSION) return (const GLubyte*)(mode == 1 ? "OpenGL ES 3.0 mock" :
        mode == 2 ? "4.6 mock" : mode == 3 ? "2.1 mock" : "OpenGL ES 3.0 unsupported");
    if (pname == GL_EXTENSIONS) return (const GLubyte*)(mode == 1 ? "GL_EXT_disjoint_timer_query" :
        mode == 2 ? "GL_ARB_timer_query" : mode == 3 ? "GL_EXT_timer_query" : "GL_EXT_disjoint_timer_query_extra");
    return (const GLubyte*)"mock";
}
static const GLubyte* GL_APIENTRY fake_string_i(GLenum pname, GLuint i) {
    (void)pname;
    return i == 0 ? fake_string(GL_EXTENSIONS) : NULL;
}
static void GL_APIENTRY fake_integer(GLenum pname, GLint* value) {
    if (pname == GL_NUM_EXTENSIONS) *value = 1;
    else if (pname == GL_GPU_DISJOINT_EXT) {
        ++disjoint_reads; *value = disjoint; disjoint = 0;
    } else driver_error(GL_INVALID_ENUM);
}
static void GL_APIENTRY fake_integer64(GLenum pname, GLint64* value) {
    if (pname == GL_TIMESTAMP_EXT) *value = 0x12345678abcdef0LL;
    else driver_error(GL_INVALID_ENUM);
}
static void GL_APIENTRY fake_gen(GLsizei n, GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) { ids[i] = next_id++; queries[ids[i]].exists = 1; }
}
static void GL_APIENTRY fake_delete(GLsizei n, const GLuint* ids) {
    deletes += n;
    for (GLsizei i = 0; i < n; ++i) queries[ids[i]].exists = 0;
}
static GLboolean GL_APIENTRY fake_is_query(GLuint id) {
    return id < 64 && queries[id].exists ? GL_TRUE : GL_FALSE;
}
static void GL_APIENTRY fake_begin(GLenum target, GLuint id) {
    if (target != GL_TIME_ELAPSED_EXT) { driver_error(GL_INVALID_ENUM); return; }
    if (active || !fake_is_query(id)) { driver_error(GL_INVALID_OPERATION); return; }
    ++begins; active = id; queries[id].ready = 0;
}
static void GL_APIENTRY fake_end(GLenum target) {
    if (target != GL_TIME_ELAPSED_EXT) { driver_error(GL_INVALID_ENUM); return; }
    if (!active) { driver_error(GL_INVALID_OPERATION); return; }
    ++ends; active = 0;
}
static void GL_APIENTRY fake_query_iv(GLenum target, GLenum pname, GLint* value) {
    if (fail_bits) { driver_error(GL_INVALID_OPERATION); return; }
    if (pname == GL_QUERY_COUNTER_BITS_EXT) *value = target == GL_TIMESTAMP_EXT ? timestamp_bits : elapsed_bits;
    else if (pname == GL_CURRENT_QUERY_EXT) *value = (GLint)active;
    else driver_error(GL_INVALID_ENUM);
}
static void GL_APIENTRY fake_counter(GLuint id, GLenum target) {
    if (fail_counter || !fake_is_query(id) || active == id) { driver_error(GL_INVALID_OPERATION); return; }
    if (target != GL_TIMESTAMP_EXT) { driver_error(GL_INVALID_ENUM); return; }
    queries[id].ready = 0;
    queries[id].result = 0xfedcba9876543210ULL;
}
static int query_valid(GLuint id, GLenum pname) {
    if (!fake_is_query(id) || active == id) { driver_error(GL_INVALID_OPERATION); return 0; }
    if (pname != GL_QUERY_RESULT_AVAILABLE_EXT && pname != GL_QUERY_RESULT_EXT) { driver_error(GL_INVALID_ENUM); return 0; }
    return 1;
}
static void GL_APIENTRY fake_object_uiv(GLuint id, GLenum pname, GLuint* value) {
    if (!query_valid(id, pname)) return;
    if (pname == GL_QUERY_RESULT_AVAILABLE_EXT) *value = queries[id].ready;
    else { ++result_reads; *value = (GLuint)queries[id].result; }
}
static void GL_APIENTRY fake_object_iv(GLuint id, GLenum pname, GLint* value) {
    if (!query_valid(id, pname)) return;
    if (pname == GL_QUERY_RESULT_AVAILABLE_EXT) *value = queries[id].ready;
    else { ++result_reads; *value = (GLint)queries[id].result; }
}
static void GL_APIENTRY fake_object_ui64v(GLuint id, GLenum pname, GLuint64* value) {
    if (!query_valid(id, pname)) return;
    if (pname == GL_QUERY_RESULT_AVAILABLE_EXT) *value = queries[id].ready;
    else { ++result_reads; *value = queries[id].result; }
}
static void GL_APIENTRY fake_object_i64v(GLuint id, GLenum pname, GLint64* value) {
    if (!query_valid(id, pname)) return;
    if (pname == GL_QUERY_RESULT_AVAILABLE_EXT) *value = queries[id].ready;
    else { ++result_reads; *value = queries[id].result > INT64_MAX ? INT64_MAX : (GLint64)queries[id].result; }
}
static void* load(const char* name) {
    if (missing && !strcmp(missing, name)) return NULL;
#define PROC(name_, proc_) if (!strcmp(name, name_)) return (void*)(proc_)
    PROC("glGetString", fake_string); PROC("glGetStringi", fake_string_i);
    PROC("glGetIntegerv", fake_integer); PROC("glGetInteger64v", fake_integer64);
    PROC("glGetError", fake_error);
    PROC("glGenQueries", fake_gen); PROC("glGenQueriesEXT", fake_gen);
    PROC("glDeleteQueries", fake_delete); PROC("glDeleteQueriesEXT", fake_delete);
    PROC("glIsQuery", fake_is_query); PROC("glIsQueryEXT", fake_is_query);
    PROC("glBeginQuery", fake_begin); PROC("glBeginQueryEXT", fake_begin);
    PROC("glEndQuery", fake_end); PROC("glEndQueryEXT", fake_end);
    PROC("glGetQueryiv", fake_query_iv); PROC("glGetQueryivEXT", fake_query_iv);
    PROC("glQueryCounter", fake_counter); PROC("glQueryCounterEXT", fake_counter);
    PROC("glGetQueryObjectiv", fake_object_iv); PROC("glGetQueryObjectivEXT", fake_object_iv);
    PROC("glGetQueryObjectuiv", fake_object_uiv); PROC("glGetQueryObjectuivEXT", fake_object_uiv);
    PROC("glGetQueryObjecti64v", fake_object_i64v); PROC("glGetQueryObjecti64vEXT", fake_object_i64v);
    PROC("glGetQueryObjectui64v", fake_object_ui64v); PROC("glGetQueryObjectui64vEXT", fake_object_ui64v);
#undef PROC
    return NULL;
}
static void reset(int api) {
    mode = api; elapsed_bits = timestamp_bits = 64;
    disjoint = disjoint_reads = begins = ends = deletes = result_reads = fail_counter = fail_bits = 0;
    missing = NULL; error_count = 0; next_id = 1; active = 0;
    memset(queries, 0, sizeof queries);
    wc_gl_timer_load((void*)load);
}
static void unsupported(void) {
    reset(0); /* Exported symbols alone do not establish timer support. */
    assert(!wc_gl_timer_caps().elapsed_bits && !wc_gl_timer_caps().timestamp_bits);
    assert(!wc_gl_timer_extension_visible("GL_EXT_disjoint_timer_query"));
    GLuint64 value = 0xdeadbeefcafef00dULL;
    assert(!wc_gl_timer_get_query_object_ui64v(1, GL_QUERY_RESULT_EXT, &value));
    assert(value == 0xdeadbeefcafef00dULL);
    assert(wc_gl_timer_get_error() == GL_INVALID_OPERATION);
    assert(!wc_gl_timer_query_counter(1, GL_TIMESTAMP_EXT));
    assert(wc_gl_timer_get_error() == GL_INVALID_OPERATION);
    assert(!wc_gl_timer_query_counter(1, GL_TIME_ELAPSED_EXT));
    assert(wc_gl_timer_get_error() == GL_INVALID_ENUM);
    assert(!wc_gl_timer_begin_query(GL_TIME_ELAPSED_EXT, 1));
    assert(wc_gl_timer_get_error() == GL_INVALID_ENUM);
    GLint flag = 17;
    assert(!wc_gl_timer_get_disjoint(&flag) && flag == 17);
    assert(wc_gl_timer_get_error() == GL_INVALID_ENUM);
    assert(wc_gl_timer_get_error() == GL_NO_ERROR);
    wc_gl_timer_load(NULL);
    GLuint id = 99;
    assert(!wc_gl_timer_gen_queries(1, &id) && id == 99);
    assert(wc_gl_timer_get_error() == GL_INVALID_OPERATION);
    assert(!wc_gl_timer_gen_queries(-1, &id));
    assert(wc_gl_timer_get_error() == GL_INVALID_VALUE);
}
static void capabilities(void) {
    static const char* required[] = {"glGenQueriesEXT", "glDeleteQueriesEXT", "glIsQueryEXT", "glBeginQueryEXT",
        "glEndQueryEXT", "glGetQueryivEXT", "glGetQueryObjectivEXT", "glGetQueryObjectuivEXT",
        "glQueryCounterEXT", "glGetQueryObjecti64vEXT", "glGetQueryObjectui64vEXT", "glGetInteger64v", "glGetIntegerv", "glGetError"};
    for (unsigned i = 0; i < sizeof required / sizeof *required; ++i) {
        reset(1); missing = required[i]; wc_gl_timer_load((void*)load);
        assert(!wc_gl_timer_caps().elapsed_bits && !wc_gl_timer_caps().timestamp_bits);
        assert(!wc_gl_timer_extension_visible("GL_EXT_disjoint_timer_query"));
    }
    reset(1); missing = "glGetInteger64vEXT"; timestamp_bits = 0; wc_gl_timer_load((void*)load);
    assert(wc_gl_timer_caps().elapsed_bits == 64 && !wc_gl_timer_caps().timestamp_bits);
    assert(wc_gl_timer_caps().disjoint && wc_gl_timer_extension_visible("GL_EXT_disjoint_timer_query"));
    assert(!disjoint_reads); /* Discovery cannot clear the accumulated flag. */
    elapsed_bits = 0; wc_gl_timer_load((void*)load);
    assert(!wc_gl_timer_caps().elapsed_bits && wc_gl_timer_caps().disjoint);
    elapsed_bits = 29; timestamp_bits = 65; wc_gl_timer_load((void*)load);
    assert(!wc_gl_timer_caps().elapsed_bits && !wc_gl_timer_caps().timestamp_bits);
    reset(1); fail_bits = 1; wc_gl_timer_load((void*)load);
    assert(!wc_gl_timer_caps().elapsed_bits && !wc_gl_timer_extension_visible("GL_EXT_disjoint_timer_query"));
    assert(wc_gl_timer_get_error() == GL_NO_ERROR); /* Own discovery errors isolated. */
    assert(!wc_gl_timer_begin_query(GL_TIME_ELAPSED_EXT, 1) && !begins);
    assert(wc_gl_timer_get_error() == GL_INVALID_ENUM);
    GLuint64 untouched = 55;
    assert(!wc_gl_timer_get_query_object_ui64v(1, GL_QUERY_RESULT_EXT, &untouched) && untouched == 55);
    assert(wc_gl_timer_get_error() == GL_INVALID_OPERATION);
    reset(2);
    assert(wc_gl_timer_caps().elapsed_bits == 64 && wc_gl_timer_caps().timestamp_bits == 64);
    assert(!wc_gl_timer_caps().disjoint && !wc_gl_timer_extension_visible("GL_EXT_disjoint_timer_query"));
    reset(2); missing = "glQueryCounter"; wc_gl_timer_load((void*)load);
    assert(!wc_gl_timer_caps().timestamp_bits && !wc_gl_timer_caps().elapsed_bits);
    reset(3);
    assert(wc_gl_timer_caps().elapsed_bits == 64 && !wc_gl_timer_caps().timestamp_bits && !wc_gl_timer_caps().disjoint);
    assert(wc_gl_timer_extension_visible("GL_EXT_timer_query") && !wc_gl_timer_extension_visible("GL_ARB_timer_query"));
    assert(!wc_gl_timer_query_counter(1, GL_TIMESTAMP_EXT));
    assert(wc_gl_timer_get_error() == GL_INVALID_OPERATION);
}
static void asynchronous_results(void) {
    reset(1); GLuint id;
    assert(wc_gl_timer_checked_gen_queries(1, &id));
    assert(wc_gl_timer_checked_query_counter(id, GL_TIMESTAMP_EXT));
    GLuint available = 1;
    assert(wc_gl_timer_checked_get_query_object_uiv(id, GL_QUERY_RESULT_AVAILABLE_EXT, &available));
    assert(!available && !result_reads);
    GLuint64 value = 99;
    assert(wc_gl_timer_get_query_object_ui64v(id, GL_QUERY_RESULT_AVAILABLE_EXT, &value));
    assert(!value && !result_reads);
    queries[id].ready = 1;
    assert(wc_gl_timer_checked_get_query_object_uiv(id, GL_QUERY_RESULT_AVAILABLE_EXT, &available) && available);
    assert(wc_gl_timer_checked_get_query_object_ui64v(id, GL_QUERY_RESULT_EXT, &value));
    assert(value == 0xfedcba9876543210ULL && result_reads == 1);
    GLint64 signed_value = 0;
    assert(wc_gl_timer_get_query_object_i64v(id, GL_QUERY_RESULT_EXT, &signed_value));
    assert(signed_value == INT64_MAX);
    assert(wc_gl_timer_checked_delete_queries(1, &id) && !wc_gl_timer_is_query(id) && deletes == 1);
    value = 77;
    assert(!wc_gl_timer_checked_get_query_object_ui64v(id, GL_QUERY_RESULT_EXT, &value) && value == 77);
    assert(wc_gl_timer_get_error() == GL_NO_ERROR);
}
static void errors_and_cleanup(void) {
    reset(1); GLuint ids[2];
    assert(wc_gl_timer_checked_gen_queries(2, ids));
    queries[ids[0]].ready = 1; queries[ids[0]].result = 1234;
    driver_error(GL_INVALID_ENUM); driver_error(GL_INVALID_VALUE);
    fail_counter = 1;
    assert(!wc_gl_timer_checked_query_counter(ids[0], GL_TIMESTAMP_EXT));
    assert(queries[ids[0]].ready && queries[ids[0]].result == 1234); /* Old result still exists; failure prevents acceptance. */
    assert(wc_gl_timer_get_error() == GL_INVALID_ENUM);
    assert(wc_gl_timer_get_error() == GL_INVALID_VALUE);
    assert(wc_gl_timer_get_error() == GL_NO_ERROR);
    fail_counter = 0;
    assert(wc_gl_timer_checked_begin_query(GL_TIME_ELAPSED_EXT, ids[0]));
    assert(!wc_gl_timer_checked_begin_query(GL_TIME_ELAPSED_EXT, ids[1]));
    assert(active == ids[0] && begins == 1);
    GLint current = 0;
    assert(wc_gl_timer_checked_get_query_iv(GL_TIME_ELAPSED_EXT, GL_CURRENT_QUERY_EXT, &current) && current == (GLint)ids[0]);
    assert(wc_gl_timer_checked_end_query(GL_TIME_ELAPSED_EXT) && !active && ends == 1);
    assert(wc_gl_timer_checked_delete_queries(2, ids) && deletes == 2);
    assert(!wc_gl_timer_is_query(ids[0]) && !wc_gl_timer_is_query(ids[1]));
}
static void independent_disjoint_views(void) {
    reset(1); GLint guest = 0, profiler = 0;
    disjoint = 1;
    assert(wc_gl_timer_get_disjoint(&guest) && guest);
    assert(wc_gl_timer_get_disjoint(&guest) && !guest);
    assert(wc_gl_timer_checked_get_disjoint(&profiler) && profiler);
    assert(wc_gl_timer_checked_get_disjoint(&profiler) && !profiler);
    disjoint = 1;
    assert(wc_gl_timer_checked_get_disjoint(&profiler) && profiler);
    assert(wc_gl_timer_checked_get_disjoint(&profiler) && !profiler);
    GLint64 guest64 = 0;
    assert(wc_gl_timer_get_integer64v(GL_GPU_DISJOINT_EXT, &guest64) && guest64 == 1);
    assert(wc_gl_timer_get_disjoint(&guest) && !guest);
}
int main(void) {
    unsupported(); capabilities(); asynchronous_results(); errors_and_cleanup(); independent_disjoint_views();
    puts("PASS: timer-query capabilities, pending/64-bit results, errors, cleanup and independent disjoint views");
    return 0;
}
