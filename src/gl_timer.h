/* Native query bridge. Capabilities come from the current context's version,
 * advertised extensions, loaded procedures and actual counter bit counts. */
#ifndef WC_GL_TIMER_H
#define WC_GL_TIMER_H
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct wc_gl_timer_caps {
    GLint elapsed_bits;
    GLint timestamp_bits;
    int disjoint; /* GL_GPU_DISJOINT_EXT exists; desktop timers have no such state. */
} wc_gl_timer_caps_t;

/* Call after making a context current. Reloading clears stale context state. */
void wc_gl_timer_load(void* get_proc);
wc_gl_timer_caps_t wc_gl_timer_caps(void);
int wc_gl_timer_extension_visible(const char* extension);

/* Return 0 for an unsupported procedure/target, recording a GL error without
 * writing any output. Return 1 means the driver was called; normal GL errors
 * (invalid IDs, active queries, etc.) are still validated by the driver. */
int wc_gl_timer_gen_queries(GLsizei n, GLuint* ids);
int wc_gl_timer_delete_queries(GLsizei n, const GLuint* ids);
GLboolean wc_gl_timer_is_query(GLuint id);
int wc_gl_timer_begin_query(GLenum target, GLuint id);
int wc_gl_timer_end_query(GLenum target);
int wc_gl_timer_query_counter(GLuint id, GLenum target);
int wc_gl_timer_get_query_iv(GLenum target, GLenum pname, GLint* value);
int wc_gl_timer_get_query_object_iv(GLuint id, GLenum pname, GLint* value);
int wc_gl_timer_get_query_object_uiv(GLuint id, GLenum pname, GLuint* value);
int wc_gl_timer_get_query_object_i64v(GLuint id, GLenum pname, GLint64* value);
int wc_gl_timer_get_query_object_ui64v(GLuint id, GLenum pname, GLuint64* value);
int wc_gl_timer_get_integer64v(GLenum pname, GLint64* value);
/* Accumulated guest view; the checked profiler view below resets separately. */
int wc_gl_timer_get_disjoint(GLint* value);
/* Shared with the WASM glGetError import; consumes a shim error first. */
GLenum wc_gl_timer_get_error(void);

/* Profiler-owned calls validate driver errors, unlike the guest GL imports.
 * They retain already-pending guest errors for glGetError and return 0 on any
 * error from this call. On failure, output values and old query results must
 * never be accepted as a new sample. Use only on the context's GL thread. */
int wc_gl_timer_checked_gen_queries(GLsizei n, GLuint* ids);
int wc_gl_timer_checked_delete_queries(GLsizei n, const GLuint* ids);
int wc_gl_timer_checked_begin_query(GLenum target, GLuint id);
int wc_gl_timer_checked_end_query(GLenum target);
int wc_gl_timer_checked_query_counter(GLuint id, GLenum target);
int wc_gl_timer_checked_get_query_iv(GLenum target, GLenum pname, GLint* value);
int wc_gl_timer_checked_get_query_object_uiv(GLuint id, GLenum pname, GLuint* value);
int wc_gl_timer_checked_get_query_object_ui64v(GLuint id, GLenum pname, GLuint64* value);
int wc_gl_timer_checked_get_disjoint(GLint* value);

#ifdef __cplusplus
}
#endif
#endif
