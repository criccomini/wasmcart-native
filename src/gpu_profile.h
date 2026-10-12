/* Opt-in asynchronous timestamps around wc_host_run_frame, never presentation.
 * GL operations are injected so query/lifecycle behavior is testable without a
 * context. The caller must finish a begun frame even when the cart traps. */
#ifndef WC_GPU_PROFILE_H
#define WC_GPU_PROFILE_H
#include "gl_timer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { WC_GPU_PROFILE_CAPACITY = 8 };
#define WC_GPU_PROFILE_MAX_ELAPSED_NS UINT64_C(10000000000)

typedef struct wc_gpu_profile_ops {
    int (*gen_queries)(GLsizei n, GLuint* ids);
    int (*delete_queries)(GLsizei n, const GLuint* ids);
    int (*query_counter)(GLuint id, GLenum target);
    int (*get_query_object_uiv)(GLuint id, GLenum pname, GLuint* value);
    int (*get_query_object_ui64v)(GLuint id, GLenum pname, GLuint64* value);
    int (*get_disjoint)(GLint* value);
} wc_gpu_profile_ops_t;

typedef struct wc_gpu_profile_slot {
    GLuint start, end;
    uint32_t frame;
    uint64_t cpu_ns;
    bool pending;
} wc_gpu_profile_slot_t;

typedef struct wc_gpu_profile {
    FILE* output; /* caller owns the stream */
    wc_gpu_profile_ops_t ops;
    wc_gl_timer_caps_t caps;
    wc_gpu_profile_slot_t slots[WC_GPU_PROFILE_CAPACITY];
    int active;
    uint32_t current_frame;
    bool frame_open, supported, suspended;
    const char* unsupported_reason;
    const char* skipped_reason;
    uint64_t frames_seen, accepted, saturated, invalid, render_failed;
    uint64_t disjoint_events, disjoint_samples, lifecycle_dropped;
    uint64_t procedure_failed, cpu_only, pending_at_shutdown;
} wc_gpu_profile_t;

void wc_gpu_profile_init(wc_gpu_profile_t* p, FILE* output,
                         const wc_gpu_profile_ops_t* ops,
                         wc_gl_timer_caps_t caps);
/* Each poll performs bounded availability probes. It never waits or requests a
 * result for an unavailable query, including during pause and shutdown. */
void wc_gpu_profile_poll(wc_gpu_profile_t* p, uint32_t readback_frame);
void wc_gpu_profile_begin(wc_gpu_profile_t* p, uint32_t frame);
void wc_gpu_profile_end(wc_gpu_profile_t* p, double cpu_ns, bool render_ok);
void wc_gpu_profile_suspend(wc_gpu_profile_t* p, uint32_t frame);
void wc_gpu_profile_resume(wc_gpu_profile_t* p);
/* Call with the original context current, before destroying it. Pending samples
 * are reported and deleted; shutdown does not drain by waiting on the GPU. */
void wc_gpu_profile_destroy(wc_gpu_profile_t* p, uint32_t frame);
#endif
