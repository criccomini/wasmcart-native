#include "gl_timer.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef void* (*loader_t)(const char*);
static PFNGLGENQUERIESPROC gen;
static PFNGLDELETEQUERIESPROC del;
static PFNGLISQUERYPROC is_query;
static PFNGLBEGINQUERYPROC begin, timer_begin;
static PFNGLENDQUERYPROC end, timer_end;
static PFNGLGETQUERYIVPROC query_iv, timer_query_iv;
static PFNGLGETQUERYOBJECTUIVPROC object_uiv;
static PFNGLGETQUERYOBJECTIVEXTPROC object_iv;
static PFNGLQUERYCOUNTEREXTPROC counter;
static PFNGLGETQUERYOBJECTI64VEXTPROC object_i64v;
static PFNGLGETQUERYOBJECTUI64VEXTPROC object_ui64v;
static PFNGLGETINTEGER64VPROC integer64v;
static PFNGLGETINTEGERVPROC integer_v;
static PFNGLGETERRORPROC get_error;
static wc_gl_timer_caps_t caps;
/* GL exposes one flag per error kind, in implementation-defined order. Keep
 * those flags while isolating profiler calls from unread guest GL errors. */
static GLenum shim_errors[16];
static unsigned shim_count;
static int ext_disjoint, arb_timer, ext_timer;
static GLint guest_disjoint, profiler_disjoint;

static int fail(GLenum error) {
    for (unsigned i = 0; i < shim_count; ++i) if (shim_errors[i] == error) return 0;
    if (shim_count < sizeof shim_errors / sizeof *shim_errors) shim_errors[shim_count++] = error;
    return 0;
}

static int has_word(const char* words, const char* word) {
    size_t n = strlen(word);
    if (!words) return 0;
    for (const char* p = words; (p = strstr(p, word)); p += n)
        if ((p == words || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
    return 0;
}

static int has_extension(PFNGLGETSTRINGPROC strings, PFNGLGETSTRINGIPROC strings_i,
                         int indexed, const char* name) {
    if (indexed && strings_i && integer_v) {
        GLint n = 0;
        integer_v(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i) {
            const char* ext = (const char*)strings_i(GL_EXTENSIONS, (GLuint)i);
            if (ext && !strcmp(ext, name)) return 1;
        }
        return 0;
    }
    return strings && has_word((const char*)strings(GL_EXTENSIONS), name);
}

void wc_gl_timer_load(void* get_proc) {
    loader_t load = (loader_t)get_proc;
    memset(&caps, 0, sizeof caps);
    ext_disjoint = arb_timer = ext_timer = 0;
    shim_count = 0;
    guest_disjoint = profiler_disjoint = 0;
    gen = NULL; del = NULL; is_query = NULL; begin = timer_begin = NULL;
    end = timer_end = NULL; query_iv = timer_query_iv = NULL;
    object_uiv = NULL; object_iv = NULL; counter = NULL;
    object_i64v = NULL; object_ui64v = NULL; integer64v = NULL;
    integer_v = NULL; get_error = NULL;
    if (!load) return;
#define LOAD(type, name) ((type)load(name))
    PFNGLGETSTRINGPROC strings = LOAD(PFNGLGETSTRINGPROC, "glGetString");
    PFNGLGETSTRINGIPROC strings_i = LOAD(PFNGLGETSTRINGIPROC, "glGetStringi");
    integer_v = LOAD(PFNGLGETINTEGERVPROC, "glGetIntegerv");
    get_error = LOAD(PFNGLGETERRORPROC, "glGetError");
    integer64v = LOAD(PFNGLGETINTEGER64VPROC, "glGetInteger64v");
    gen = LOAD(PFNGLGENQUERIESPROC, "glGenQueries");
    del = LOAD(PFNGLDELETEQUERIESPROC, "glDeleteQueries");
    is_query = LOAD(PFNGLISQUERYPROC, "glIsQuery");
    begin = LOAD(PFNGLBEGINQUERYPROC, "glBeginQuery");
    end = LOAD(PFNGLENDQUERYPROC, "glEndQuery");
    query_iv = LOAD(PFNGLGETQUERYIVPROC, "glGetQueryiv");
    object_uiv = LOAD(PFNGLGETQUERYOBJECTUIVPROC, "glGetQueryObjectuiv");
    object_iv = LOAD(PFNGLGETQUERYOBJECTIVEXTPROC, "glGetQueryObjectiv");
    const char* version = strings ? (const char*)strings(GL_VERSION) : NULL;
    int es = version && !strncmp(version, "OpenGL ES", 9);
    int major = 0, minor = 0;
    if (version) {
        const char* p = version;
        while (*p && (*p < '0' || *p > '9')) ++p;
        sscanf(p, "%d.%d", &major, &minor);
    }
    int indexed = major >= 3;
    int native_disjoint = es && has_extension(strings, strings_i, indexed, "GL_EXT_disjoint_timer_query");
    int native_arb = !es && (major > 3 || (major == 3 && minor >= 3) ||
        has_extension(strings, strings_i, indexed, "GL_ARB_timer_query"));
    int native_ext = !es && has_extension(strings, strings_i, indexed, "GL_EXT_timer_query");
    if (native_disjoint) {
        /* GLES extension entry points are required even where ES3 also exposes
         * core query aliases. A loader may return non-null unsupported core
         * stubs, so choose the advertised EXT family explicitly. */
        PFNGLGENQUERIESEXTPROC eg = LOAD(PFNGLGENQUERIESEXTPROC, "glGenQueriesEXT");
        PFNGLDELETEQUERIESEXTPROC ed = LOAD(PFNGLDELETEQUERIESEXTPROC, "glDeleteQueriesEXT");
        PFNGLISQUERYEXTPROC ei = LOAD(PFNGLISQUERYEXTPROC, "glIsQueryEXT");
        timer_begin = LOAD(PFNGLBEGINQUERYPROC, "glBeginQueryEXT");
        timer_end = LOAD(PFNGLENDQUERYPROC, "glEndQueryEXT");
        timer_query_iv = LOAD(PFNGLGETQUERYIVPROC, "glGetQueryivEXT");
        PFNGLGETQUERYOBJECTIVEXTPROC eo = LOAD(PFNGLGETQUERYOBJECTIVEXTPROC, "glGetQueryObjectivEXT");
        PFNGLGETQUERYOBJECTUIVEXTPROC eu = LOAD(PFNGLGETQUERYOBJECTUIVEXTPROC, "glGetQueryObjectuivEXT");
        counter = LOAD(PFNGLQUERYCOUNTEREXTPROC, "glQueryCounterEXT");
        object_i64v = LOAD(PFNGLGETQUERYOBJECTI64VEXTPROC, "glGetQueryObjecti64vEXT");
        object_ui64v = LOAD(PFNGLGETQUERYOBJECTUI64VEXTPROC, "glGetQueryObjectui64vEXT");
        if (major < 3) integer64v = LOAD(PFNGLGETINTEGER64VPROC, "glGetInteger64vEXT");
        ext_disjoint = eg && ed && ei && timer_begin && timer_end && timer_query_iv &&
            eo && eu && counter && object_i64v && object_ui64v && integer_v && integer64v && get_error;
        if (ext_disjoint) {
            gen = (PFNGLGENQUERIESPROC)eg; del = (PFNGLDELETEQUERIESPROC)ed;
            is_query = (PFNGLISQUERYPROC)ei; object_iv = eo;
            object_uiv = (PFNGLGETQUERYOBJECTUIVPROC)eu;
            caps.disjoint = 1;
        }
    } else if (native_arb || native_ext) {
        timer_begin = begin; timer_end = end; timer_query_iv = query_iv;
        if (native_arb) {
            counter = LOAD(PFNGLQUERYCOUNTEREXTPROC, "glQueryCounter");
            object_i64v = LOAD(PFNGLGETQUERYOBJECTI64VEXTPROC, "glGetQueryObjecti64v");
            object_ui64v = LOAD(PFNGLGETQUERYOBJECTUI64VEXTPROC, "glGetQueryObjectui64v");
        } else {
            object_i64v = LOAD(PFNGLGETQUERYOBJECTI64VEXTPROC, "glGetQueryObjecti64vEXT");
            object_ui64v = LOAD(PFNGLGETQUERYOBJECTUI64VEXTPROC, "glGetQueryObjectui64vEXT");
        }
        int complete = gen && del && is_query && timer_begin && timer_end && timer_query_iv &&
            object_iv && object_uiv && object_i64v && object_ui64v && get_error;
        arb_timer = native_arb && complete && counter && integer64v;
        ext_timer = native_ext && complete;
    }
    if (ext_disjoint || arb_timer || ext_timer) {
        int valid = wc_gl_timer_checked_get_query_iv(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &caps.elapsed_bits);
        if (ext_disjoint || arb_timer)
            valid &= wc_gl_timer_checked_get_query_iv(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &caps.timestamp_bits);
        if (!valid) {
            memset(&caps, 0, sizeof caps);
            ext_disjoint = arb_timer = ext_timer = 0;
        }
        /* Zero counters are specified, but provide no useful timing. Reject
         * corrupt counts too; the extension requires at least 30 bits. */
        if (caps.elapsed_bits < 30 || caps.elapsed_bits > 64) caps.elapsed_bits = 0;
        if (caps.timestamp_bits < 30 || caps.timestamp_bits > 64) caps.timestamp_bits = 0;
    }
    if (!(ext_disjoint || arb_timer || ext_timer)) {
        timer_begin = NULL; timer_end = NULL; timer_query_iv = NULL;
        counter = NULL; object_i64v = NULL; object_ui64v = NULL;
    }
#undef LOAD
}

wc_gl_timer_caps_t wc_gl_timer_caps(void) { return caps; }

int wc_gl_timer_extension_visible(const char* name) {
    if (!strcmp(name, "GL_EXT_disjoint_timer_query")) return ext_disjoint;
    if (!strcmp(name, "GL_ARB_timer_query")) return arb_timer;
    if (!strcmp(name, "GL_EXT_timer_query")) return ext_timer;
    return 1;
}

int wc_gl_timer_gen_queries(GLsizei n, GLuint* ids) {
    if (n < 0) return fail(GL_INVALID_VALUE);
    if (!gen) return fail(GL_INVALID_OPERATION);
    gen(n, ids); return 1;
}
int wc_gl_timer_delete_queries(GLsizei n, const GLuint* ids) {
    if (n < 0) return fail(GL_INVALID_VALUE);
    if (!del) return fail(GL_INVALID_OPERATION);
    del(n, ids); return 1;
}
GLboolean wc_gl_timer_is_query(GLuint id) {
    if (!is_query) { fail(GL_INVALID_OPERATION); return GL_FALSE; }
    return is_query(id);
}
int wc_gl_timer_begin_query(GLenum target, GLuint id) {
    PFNGLBEGINQUERYPROC proc = target == GL_TIME_ELAPSED_EXT ? timer_begin : begin;
    if (target == GL_TIMESTAMP_EXT || (target == GL_TIME_ELAPSED_EXT && !proc)) return fail(GL_INVALID_ENUM);
    if (!proc) return fail(GL_INVALID_OPERATION);
    proc(target, id); return 1;
}
int wc_gl_timer_end_query(GLenum target) {
    PFNGLENDQUERYPROC proc = target == GL_TIME_ELAPSED_EXT ? timer_end : end;
    if (target == GL_TIMESTAMP_EXT || (target == GL_TIME_ELAPSED_EXT && !proc)) return fail(GL_INVALID_ENUM);
    if (!proc) return fail(GL_INVALID_OPERATION);
    proc(target); return 1;
}
int wc_gl_timer_query_counter(GLuint id, GLenum target) {
    if (target != GL_TIMESTAMP_EXT) return fail(GL_INVALID_ENUM);
    if (!counter || !(ext_disjoint || arb_timer)) return fail(GL_INVALID_OPERATION);
    counter(id, target); return 1;
}
int wc_gl_timer_get_query_iv(GLenum target, GLenum pname, GLint* value) {
    PFNGLGETQUERYIVPROC proc = (target == GL_TIME_ELAPSED_EXT || target == GL_TIMESTAMP_EXT) ? timer_query_iv : query_iv;
    if (target == GL_TIMESTAMP_EXT && !(ext_disjoint || arb_timer)) return fail(GL_INVALID_ENUM);
    if (target == GL_TIME_ELAPSED_EXT && !proc) return fail(GL_INVALID_ENUM);
    if (!proc) return fail(GL_INVALID_OPERATION);
    proc(target, pname, value); return 1;
}
int wc_gl_timer_get_query_object_iv(GLuint id, GLenum pname, GLint* value) {
    if (!object_iv) return fail(GL_INVALID_OPERATION);
    object_iv(id, pname, value); return 1;
}
int wc_gl_timer_get_query_object_uiv(GLuint id, GLenum pname, GLuint* value) {
    if (!object_uiv) return fail(GL_INVALID_OPERATION);
    object_uiv(id, pname, value); return 1;
}
int wc_gl_timer_get_query_object_i64v(GLuint id, GLenum pname, GLint64* value) {
    if (!object_i64v) return fail(GL_INVALID_OPERATION);
    object_i64v(id, pname, value); return 1;
}
int wc_gl_timer_get_query_object_ui64v(GLuint id, GLenum pname, GLuint64* value) {
    if (!object_ui64v) return fail(GL_INVALID_OPERATION);
    object_ui64v(id, pname, value); return 1;
}
int wc_gl_timer_get_integer64v(GLenum pname, GLint64* value) {
    if (pname == GL_GPU_DISJOINT_EXT) {
        GLint occurred;
        if (!wc_gl_timer_get_disjoint(&occurred)) return 0;
        *value = occurred;
        return 1;
    }
    if (pname == GL_TIMESTAMP_EXT && !(ext_disjoint || arb_timer)) return fail(GL_INVALID_ENUM);
    if (!integer64v) return fail(GL_INVALID_OPERATION);
    integer64v(pname, value); return 1;
}
static int read_disjoint(GLint* value, int profiler) {
    if (!caps.disjoint || !integer_v) return fail(GL_INVALID_ENUM);
    GLint occurred = 0;
    integer_v(GL_GPU_DISJOINT_EXT, &occurred);
    if (occurred) guest_disjoint = profiler_disjoint = 1;
    /* The physical flag resets on read. Give the cart and optional host
     * profiler independent accumulated views so neither can erase evidence
     * before the other reader has rejected its outstanding samples. */
    if (profiler) { *value = profiler_disjoint; profiler_disjoint = 0; }
    else { *value = guest_disjoint; guest_disjoint = 0; }
    return 1;
}
int wc_gl_timer_get_disjoint(GLint* value) {
    return read_disjoint(value, 0);
}
GLenum wc_gl_timer_get_error(void) {
    if (shim_count) {
        GLenum error = shim_errors[0];
        memmove(shim_errors, shim_errors + 1, --shim_count * sizeof *shim_errors);
        return error;
    }
    return get_error ? get_error() : GL_NO_ERROR;
}

static unsigned checked_enter(void) {
    /* A valid GL context has a bounded number of error flags. The bound also
     * prevents a faulty loader from keeping an error-drain loop alive. */
    if (get_error) for (unsigned i = 0; i < 16; ++i) {
        GLenum error = get_error();
        if (!error) break;
        fail(error);
    }
    return shim_count;
}

static int checked_leave(unsigned saved_count, int called) {
    int valid = called;
    if (get_error) for (unsigned i = 0; i < 16; ++i) {
        GLenum error = get_error();
        if (!error) break;
        valid = 0;
    }
    /* Remove only shim errors generated by the profiler's operation. */
    shim_count = saved_count;
    return valid;
}

#define CHECKED(name, parameters, arguments) \
    int wc_gl_timer_checked_##name parameters { \
        unsigned saved = checked_enter(); \
        int called = wc_gl_timer_##name arguments; \
        return checked_leave(saved, called); \
    }
CHECKED(gen_queries, (GLsizei n, GLuint* ids), (n, ids))
CHECKED(delete_queries, (GLsizei n, const GLuint* ids), (n, ids))
CHECKED(begin_query, (GLenum target, GLuint id), (target, id))
CHECKED(end_query, (GLenum target), (target))
CHECKED(query_counter, (GLuint id, GLenum target), (id, target))
CHECKED(get_query_iv, (GLenum target, GLenum pname, GLint* value), (target, pname, value))
CHECKED(get_query_object_uiv, (GLuint id, GLenum pname, GLuint* value), (id, pname, value))
CHECKED(get_query_object_ui64v, (GLuint id, GLenum pname, GLuint64* value), (id, pname, value))
#undef CHECKED

int wc_gl_timer_checked_get_disjoint(GLint* value) {
    unsigned saved = checked_enter();
    GLint occurred = 0;
    int called = read_disjoint(&occurred, 1);
    int valid = checked_leave(saved, called);
    if (valid) *value = occurred;
    else if (occurred) profiler_disjoint = 1;
    return valid;
}
