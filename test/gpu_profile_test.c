#include "gpu_profile.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* All mock results start unavailable. A result read before both query
 * endpoints are available is a hard test failure, rather than a fake wait. */
static struct {
    unsigned gen, deleted, counters, results, availability, disjoint_checks;
    GLuint available[32];
    GLuint64 value[32], clock;
    int disjoint, raise_disjoint_after_reads;
    int fail_gen, fail_counter, fail_available, fail_result, fail_disjoint, fail_delete;
} mock;

static int gen(GLsizei n, GLuint* ids) {
    ++mock.gen;
    for (int i = 0; i < n; ++i) ids[i] = (GLuint)i + 1;
    return !mock.fail_gen;
}
static int del(GLsizei n, const GLuint* ids) {
    for (int i = 0; i < n; ++i) assert(ids[i] > 0 && ids[i] <= 16);
    mock.deleted += (unsigned)n;
    return !mock.fail_delete;
}
static int counter(GLuint id, GLenum target) {
    assert(target == GL_TIMESTAMP_EXT && id > 0 && id <= 16);
    ++mock.counters;
    if (mock.fail_counter) return 0;
    mock.value[id] = (mock.clock += 1000);
    mock.available[id] = 0;
    return 1;
}
static int available(GLuint id, GLenum pname, GLuint* out) {
    assert(pname == GL_QUERY_RESULT_AVAILABLE && id > 0 && id <= 16);
    ++mock.availability;
    if (mock.fail_available) return 0;
    *out = mock.available[id];
    return 1;
}
static int result(GLuint id, GLenum pname, GLuint64* out) {
    assert(pname == GL_QUERY_RESULT && mock.available[id]);
    ++mock.results;
    if (mock.raise_disjoint_after_reads && mock.results == (unsigned)mock.raise_disjoint_after_reads)
        mock.disjoint = 1;
    if (mock.fail_result) return 0;
    *out = mock.value[id];
    return 1;
}
static int disjoint(GLint* out) {
    ++mock.disjoint_checks;
    if (mock.fail_disjoint) return 0;
    *out = mock.disjoint;
    mock.disjoint = 0; /* EXT read clears the flag */
    return 1;
}
static wc_gpu_profile_ops_t ops = {gen, del, counter, available, result, disjoint};
static wc_gl_timer_caps_t caps = {64, 64, 1};

static FILE* start(wc_gpu_profile_t* p, wc_gl_timer_caps_t c) {
    memset(&mock, 0, sizeof(mock));
    FILE* output = tmpfile();
    assert(output);
    wc_gpu_profile_init(p, output, &ops, c);
    return output;
}
static void frame(wc_gpu_profile_t* p, uint32_t f) {
    wc_gpu_profile_begin(p, f);
    wc_gpu_profile_end(p, 12345, true);
}
static void ready(wc_gpu_profile_slot_t* s) {
    mock.available[s->start] = mock.available[s->end] = 1;
}
static void contains(FILE* f, const char* text) {
    char buf[16384];
    fflush(f);
    rewind(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    assert(strstr(buf, text));
    assert(fseek(f, 0, SEEK_END) == 0);
}

static void pending_and_original_frame(void) {
    wc_gpu_profile_t p;
    FILE* f = start(&p, caps);
    frame(&p, 37);
    wc_gpu_profile_poll(&p, 38);
    assert(p.accepted == 0 && mock.results == 0 && p.slots[0].pending);
    mock.available[p.slots[0].start] = 1;
    wc_gpu_profile_poll(&p, 39);
    assert(p.accepted == 0 && mock.results == 0);
    ready(&p.slots[0]);
    wc_gpu_profile_poll(&p, 40);
    assert(p.accepted == 1 && mock.results == 2 && !p.slots[0].pending);
    contains(f, "\"frame\":37,\"readback_frame\":40,\"cpu_cart_frame_wall_ns\":12345,\"gpu_cart_frame_elapsed_ns\":1000");
    wc_gpu_profile_destroy(&p, 40);
    assert(mock.deleted == 16 && p.pending_at_shutdown == 0);
    wc_gpu_profile_destroy(&p, 40);
    assert(mock.deleted == 16);
    fclose(f);
}

static void unsupported_and_disabled(void) {
    wc_gpu_profile_t p;
    wc_gl_timer_caps_t c = {64, 0, 1}; /* elapsed alone is unsafe for guests */
    FILE* f = start(&p, c);
    frame(&p, 0);
    assert(!p.supported && mock.gen == 0 && mock.counters == 0 && p.cpu_only == 1);
    contains(f, "\"gpu_cart_frame_elapsed_ns\":null,\"invalid_gpu\":true,\"reason\":\"timestamp_query_unsupported\"");
    wc_gpu_profile_destroy(&p, 1);
    assert(mock.deleted == 0);
    fclose(f);
    memset(&mock, 0, sizeof(mock));
    wc_gpu_profile_init(&p, NULL, &ops, caps);
    frame(&p, 0);
    wc_gpu_profile_poll(&p, 1);
    wc_gpu_profile_destroy(&p, 1);
    assert(mock.gen == 0 && mock.counters == 0 && mock.results == 0);
    c.timestamp_bits = 29;
    f = start(&p, c);
    assert(!p.supported && !mock.gen);
    wc_gpu_profile_destroy(&p, 1);
    fclose(f);
    for (int bits = 30; bits < 64; ++bits) {
        c.timestamp_bits = bits;
        f = start(&p, c);
        assert(!p.supported && !mock.gen);
        contains(f, "timestamp_counter_too_narrow");
        wc_gpu_profile_destroy(&p, 1);
        fclose(f);
    }
    memset(&mock, 0, sizeof(mock));
    f = tmpfile();
    assert(f);
    wc_gpu_profile_ops_t incomplete = ops;
    incomplete.query_counter = NULL;
    wc_gpu_profile_init(&p, f, &incomplete, caps);
    assert(!p.supported && !mock.gen);
    wc_gpu_profile_destroy(&p, 0);
    fclose(f);
}

static void saturation_and_reuse(void) {
    wc_gpu_profile_t p;
    FILE* f = start(&p, caps);
    for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i) frame(&p, i);
    frame(&p, WC_GPU_PROFILE_CAPACITY);
    assert(p.saturated == 1 && mock.counters == 16 && mock.results == 0);
    contains(f, "\"reason\":\"ring_saturated\"");
    ready(&p.slots[0]);
    frame(&p, 9);
    assert(p.accepted == 1 && p.slots[0].frame == 9 && p.slots[0].pending);
    assert(!mock.available[p.slots[0].start] && !mock.available[p.slots[0].end]);
    wc_gpu_profile_destroy(&p, 10);
    assert(p.pending_at_shutdown == 8 && mock.deleted == 16 && mock.results == 2);
    contains(f, "\"reason\":\"shutdown_pending\"");
    fclose(f);
}

static void disjoint_before_and_during_read(void) {
    for (unsigned during = 0; during < 2; ++during) {
        wc_gpu_profile_t p;
        FILE* f = start(&p, caps);
        frame(&p, 0);
        frame(&p, 1);
        ready(&p.slots[0]);
        ready(&p.slots[1]);
        if (during) mock.raise_disjoint_after_reads = 2;
        else mock.disjoint = 1;
        wc_gpu_profile_poll(&p, 2);
        assert(!p.accepted && p.disjoint_events == 1 && p.disjoint_samples == 2);
        assert(!p.slots[0].pending && !p.slots[1].pending);
        assert(mock.results == (during ? 2u : 0u));
        contains(f, "\"reason\":\"gpu_disjoint\"");
        wc_gpu_profile_destroy(&p, 3);
        assert(mock.deleted == 16);
        fclose(f);
    }
    wc_gpu_profile_t p;
    wc_gl_timer_caps_t c = caps;
    c.disjoint = 0; /* Desktop timer queries have no EXT disjoint flag. */
    FILE* f = start(&p, c);
    frame(&p, 0);
    ready(&p.slots[0]);
    wc_gpu_profile_poll(&p, 1);
    assert(p.accepted == 1 && !mock.disjoint_checks);
    wc_gpu_profile_destroy(&p, 1);
    fclose(f);
}

static void invalid_values(void) {
    const GLuint64 values[][2] = {
        {0, 1000}, {1000, 0}, {1000, 1000}, {1000, 999},
        {1, WC_GPU_PROFILE_MAX_ELAPSED_NS + 2},
        {UINT64_MAX - 1, UINT64_MAX}
    };
    const char* reasons[] = {"zero_timestamp", "zero_timestamp", "reversed_or_zero_elapsed",
        "reversed_or_zero_elapsed", "elapsed_above_validation_bound", "saturated_counter"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        wc_gpu_profile_t p;
        wc_gl_timer_caps_t c = caps;
        FILE* f = start(&p, c);
        frame(&p, 0);
        mock.value[p.slots[0].start] = values[i][0];
        mock.value[p.slots[0].end] = values[i][1];
        ready(&p.slots[0]);
        wc_gpu_profile_poll(&p, 1);
        assert(!p.accepted && p.invalid == 1);
        contains(f, reasons[i]);
        wc_gpu_profile_destroy(&p, 2);
        fclose(f);
    }
    const double cpu[] = {0, 0.1, -1, NAN, INFINITY, 18446744073709551616.0};
    for (unsigned i = 0; i < sizeof(cpu) / sizeof(cpu[0]); ++i) {
        wc_gpu_profile_t p;
        FILE* f = start(&p, caps);
        wc_gpu_profile_begin(&p, 0);
        wc_gpu_profile_end(&p, cpu[i], true);
        assert(p.invalid == 1 && !p.slots[0].pending && mock.counters == 2);
        contains(f, "invalid_cpu_clock");
        wc_gpu_profile_destroy(&p, 1);
        fclose(f);
    }
}

static void failed_procedures(void) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        wc_gpu_profile_t p;
        FILE* f = start(&p, caps);
        if (mode == 0) mock.fail_counter = 1;
        wc_gpu_profile_begin(&p, 0);
        if (mode == 1) mock.fail_counter = 1;
        wc_gpu_profile_end(&p, 100, true);
        if (mode == 2) mock.fail_available = 1;
        if (mode == 3) { ready(&p.slots[0]); mock.fail_result = 1; }
        if (mode == 4) mock.fail_disjoint = 1;
        wc_gpu_profile_poll(&p, 1);
        assert(p.procedure_failed >= 1 && !p.accepted && p.invalid == 1);
        if (mode == 2) assert(!mock.results);
        wc_gpu_profile_destroy(&p, 2);
        assert(mock.deleted == 16);
        fclose(f);
    }
    wc_gpu_profile_t p;
    memset(&mock, 0, sizeof(mock));
    mock.fail_gen = 1;
    FILE* f = tmpfile();
    assert(f);
    wc_gpu_profile_init(&p, f, &ops, caps);
    assert(!p.supported && mock.deleted == 16);
    frame(&p, 0);
    contains(f, "query_allocation_failed");
    wc_gpu_profile_destroy(&p, 1);
    assert(mock.deleted == 16);
    fclose(f);
    f = start(&p, caps);
    frame(&p, 0);
    ready(&p.slots[0]);
    wc_gpu_profile_poll(&p, 1);
    assert(p.accepted == 1 && mock.results == 2);
    /* A checked QueryCounter failure may leave the old generation ready.
     * The failed replacement must never enter the pending ring. */
    mock.fail_counter = 1;
    frame(&p, 1);
    wc_gpu_profile_poll(&p, 2);
    assert(p.accepted == 1 && p.invalid == 1 && mock.results == 2);
    mock.fail_delete = 1;
    wc_gpu_profile_destroy(&p, 2);
    assert(mock.deleted == 16 && p.procedure_failed == 2);
    fclose(f);
}

static void lifecycle_and_render_failure(void) {
    wc_gpu_profile_t p;
    FILE* f = start(&p, caps);
    wc_gpu_profile_begin(&p, 0);
    wc_gpu_profile_end(&p, 100, false);
    assert(mock.counters == 2 && p.render_failed == 1 && !p.slots[0].pending);
    frame(&p, 1);
    wc_gpu_profile_suspend(&p, 2);
    assert(p.lifecycle_dropped == 1 && p.suspended && !p.slots[0].pending);
    frame(&p, 2);
    assert(p.frames_seen == 2);
    wc_gpu_profile_resume(&p);
    frame(&p, 2);
    assert(p.frames_seen == 3 && p.slots[0].pending);
    wc_gpu_profile_destroy(&p, 3);
    assert(p.pending_at_shutdown == 1 && mock.deleted == 16 && !mock.results);
    contains(f, "render_failed");
    contains(f, "suspended");
    contains(f, "shutdown_pending");
    fclose(f);
    f = start(&p, caps);
    wc_gpu_profile_begin(&p, 0);
    wc_gpu_profile_destroy(&p, 0); /* emergency cleanup of an unfinished frame */
    assert(mock.counters == 2 && p.render_failed == 1 && mock.deleted == 16);
    fclose(f);
}

int main(void) {
    pending_and_original_frame();
    unsupported_and_disabled();
    saturation_and_reuse();
    disjoint_before_and_during_read();
    invalid_values();
    failed_procedures();
    lifecycle_and_render_failure();
    puts("gpu_profile_test: passed");
    return 0;
}
