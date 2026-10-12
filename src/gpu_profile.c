#include "gpu_profile.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

static unsigned pending_count(const wc_gpu_profile_t* p) {
    unsigned n = 0;
    for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i)
        if (p->slots[i].pending) ++n;
    return n;
}

static void sample(wc_gpu_profile_t* p, uint32_t frame, uint64_t cpu_ns,
                   uint32_t readback_frame, uint64_t gpu_ns, const char* reason) {
    if (!p->output) return;
    fprintf(p->output, "{\"event\":\"gpu_profile_sample\",\"frame\":%" PRIu32
            ",\"readback_frame\":%" PRIu32 ",\"cpu_cart_frame_wall_ns\":%" PRIu64
            ",\"gpu_cart_frame_elapsed_ns\":", frame, readback_frame, cpu_ns);
    if (reason) fprintf(p->output, "null,\"invalid_gpu\":true,\"reason\":\"%s\"", reason);
    else fprintf(p->output, "%" PRIu64 ",\"invalid_gpu\":false", gpu_ns);
    fprintf(p->output, ",\"stats\":{\"accepted\":%" PRIu64 ",\"pending\":%u"
            ",\"dropped_saturated\":%" PRIu64 ",\"invalid\":%" PRIu64
            ",\"disjoint_events\":%" PRIu64 ",\"disjoint_samples\":%" PRIu64 "}}\n",
            p->accepted, pending_count(p), p->saturated, p->invalid,
            p->disjoint_events, p->disjoint_samples);
}

static void discard_pending(wc_gpu_profile_t* p, uint32_t frame, const char* reason) {
    for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i) {
        wc_gpu_profile_slot_t* s = &p->slots[i];
        if (!s->pending) continue;
        s->pending = false;
        sample(p, s->frame, s->cpu_ns, frame, 0, reason);
    }
}

static void delete_queries(wc_gpu_profile_t* p) {
    GLuint ids[2 * WC_GPU_PROFILE_CAPACITY];
    unsigned n = 0;
    for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i) {
        if (p->slots[i].start) ids[n++] = p->slots[i].start;
        if (p->slots[i].end) ids[n++] = p->slots[i].end;
        p->slots[i].start = p->slots[i].end = 0;
    }
    if (n && p->ops.delete_queries && !p->ops.delete_queries((GLsizei)n, ids))
        ++p->procedure_failed;
}

void wc_gpu_profile_init(wc_gpu_profile_t* p, FILE* output,
                         const wc_gpu_profile_ops_t* ops, wc_gl_timer_caps_t caps) {
    memset(p, 0, sizeof(*p));
    p->active = -1;
    p->output = output;
    p->caps = caps;
    if (ops) p->ops = *ops;
    p->unsupported_reason = "timestamp_query_unsupported";
    if (caps.timestamp_bits >= 30 && caps.timestamp_bits < 64)
        p->unsupported_reason = "timestamp_counter_too_narrow";
    /* No elapsed fallback: a guest could begin its own elapsed query during
     * this frame, even when GL_CURRENT_QUERY was zero at the start. Timestamps
     * have no active target and cannot corrupt that guest query state. */
    /* Narrow counters can complete one or more full wraps while pending and
     * still yield end > start. Restrict host sampling to 64 bits; retain the
     * actual narrower capability for guest APIs and in the output metadata. */
    p->supported = output && caps.timestamp_bits == 64 && ops &&
        ops->gen_queries && ops->delete_queries && ops->query_counter &&
        ops->get_query_object_uiv && ops->get_query_object_ui64v &&
        (!caps.disjoint || ops->get_disjoint);
    if (p->supported) {
        GLuint ids[2 * WC_GPU_PROFILE_CAPACITY] = {0};
        int generated = p->ops.gen_queries(2 * WC_GPU_PROFILE_CAPACITY, ids);
        for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i) {
            p->slots[i].start = ids[2 * i];
            p->slots[i].end = ids[2 * i + 1];
            if (!ids[2 * i] || !ids[2 * i + 1]) generated = 0;
        }
        if (!generated) {
            ++p->procedure_failed;
            delete_queries(p);
            p->supported = false;
            p->unsupported_reason = "query_allocation_failed";
        }
    }
    if (output) {
        fprintf(output, "{\"event\":\"gpu_profile_capabilities\",\"schema\":1,"
                "\"gpu_supported\":%s,\"method\":\"%s\",\"timestamp_bits\":%d,"
                "\"elapsed_bits\":%d,\"disjoint_supported\":%s,\"ring_capacity\":%d,"
                "\"max_elapsed_ns\":%" PRIu64 ",\"frame_origin\":0,"
                "\"cpu_clock\":\"SDL performance counter\","
                "\"scope\":\"wc_host_run_frame: host async pump, input delivery, wc_render, memory refresh; excludes frontend blit and present\","
                "\"gpu_semantics\":\"timestamp completion span, includes command starvation and driver scheduling; not dedicated GPU busy time\","
                "\"elapsed_fallback\":false,\"required_timestamp_bits\":64,\"unsupported_reason\":",
                p->supported ? "true" : "false", p->supported ? "timestamp" : "unsupported",
                caps.timestamp_bits, caps.elapsed_bits, caps.disjoint ? "true" : "false",
                WC_GPU_PROFILE_CAPACITY, WC_GPU_PROFILE_MAX_ELAPSED_NS);
        if (p->supported) fputs("null}\n", output);
        else fprintf(output, "\"%s\"}\n", p->unsupported_reason);
        fflush(output);
    }
}

void wc_gpu_profile_poll(wc_gpu_profile_t* p, uint32_t frame) {
    if (!p->output || !p->supported) return;
    if (p->caps.disjoint) {
        GLint disjoint = 0;
        if (!p->ops.get_disjoint(&disjoint)) {
            ++p->procedure_failed;
            p->invalid += pending_count(p);
            discard_pending(p, frame, "disjoint_read_failed");
            return;
        }
        if (disjoint) {
            ++p->disjoint_events;
            p->disjoint_samples += pending_count(p);
            discard_pending(p, frame, "gpu_disjoint");
            return;
        }
    }
    for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i) {
        wc_gpu_profile_slot_t* s = &p->slots[i];
        if (!s->pending) continue;
        GLuint start_available = 0, end_available = 0;
        int ok = p->ops.get_query_object_uiv(s->start, GL_QUERY_RESULT_AVAILABLE, &start_available);
        ok = p->ops.get_query_object_uiv(s->end, GL_QUERY_RESULT_AVAILABLE, &end_available) && ok;
        if (ok && (!start_available || !end_available)) continue;
        GLuint64 start = 0, end = 0;
        if (ok) {
            ok = p->ops.get_query_object_ui64v(s->start, GL_QUERY_RESULT, &start);
            ok = p->ops.get_query_object_ui64v(s->end, GL_QUERY_RESULT, &end) && ok;
        }
        /* Disjoint can be raised during readback too. Reject every still
         * outstanding sample before publishing anything from this poll. */
        if (p->caps.disjoint) {
            GLint disjoint = 0;
            int checked = p->ops.get_disjoint(&disjoint);
            if (!checked || disjoint) {
                if (!checked) { ++p->procedure_failed; p->invalid += pending_count(p); }
                else { ++p->disjoint_events; p->disjoint_samples += pending_count(p); }
                discard_pending(p, frame, checked ? "gpu_disjoint" : "disjoint_read_failed");
                return;
            }
        }
        const char* reason = NULL;
        uint64_t limit = p->caps.timestamp_bits == 64 ? UINT64_MAX :
            (UINT64_C(1) << p->caps.timestamp_bits) - 1;
        if (!ok) { reason = "query_read_failed"; ++p->procedure_failed; }
        else if (!start || !end) reason = "zero_timestamp";
        else if (start > limit || end > limit) reason = "timestamp_counter_overflow";
        else if (start == limit || end == limit) reason = "saturated_counter";
        else if (end <= start) reason = "reversed_or_zero_elapsed";
        else if (end - start > WC_GPU_PROFILE_MAX_ELAPSED_NS) reason = "elapsed_above_validation_bound";
        s->pending = false;
        if (reason) ++p->invalid;
        else ++p->accepted;
        sample(p, s->frame, s->cpu_ns, frame, reason ? 0 : end - start, reason);
    }
}

void wc_gpu_profile_begin(wc_gpu_profile_t* p, uint32_t frame) {
    if (!p->output || p->suspended) return;
    wc_gpu_profile_poll(p, frame);
    p->current_frame = frame;
    p->frame_open = true;
    p->active = -1;
    p->skipped_reason = p->supported ? "ring_saturated" : p->unsupported_reason;
    ++p->frames_seen;
    if (!p->supported) return;
    for (unsigned i = 0; i < WC_GPU_PROFILE_CAPACITY; ++i) {
        wc_gpu_profile_slot_t* s = &p->slots[i];
        if (s->pending) continue;
        s->frame = frame;
        if (p->ops.query_counter(s->start, GL_TIMESTAMP_EXT)) p->active = (int)i;
        else { ++p->procedure_failed; p->skipped_reason = "query_start_failed"; }
        return;
    }
    ++p->saturated;
}

void wc_gpu_profile_end(wc_gpu_profile_t* p, double cpu_ns, bool render_ok) {
    if (!p->output || !p->frame_open) return;
    p->frame_open = false;
    bool cpu_ok = isfinite(cpu_ns) && cpu_ns >= 1.0 && cpu_ns < 18446744073709551616.0;
    uint64_t cpu = cpu_ok ? (uint64_t)cpu_ns : 0;
    const char* reason = p->skipped_reason;
    if (p->active >= 0) {
        wc_gpu_profile_slot_t* s = &p->slots[p->active];
        s->cpu_ns = cpu;
        if (!p->ops.query_counter(s->end, GL_TIMESTAMP_EXT)) {
            ++p->procedure_failed;
            reason = "query_end_failed";
        } else if (render_ok && cpu_ok) {
            s->pending = true;
            p->active = -1;
            return;
        }
    }
    p->active = -1;
    if (!render_ok) { ++p->render_failed; reason = "render_failed"; }
    else if (!cpu_ok) { ++p->invalid; reason = "invalid_cpu_clock"; }
    else if (!p->supported) ++p->cpu_only;
    else if (reason && strcmp(reason, "ring_saturated")) ++p->invalid;
    sample(p, p->current_frame, cpu, p->current_frame, 0, reason);
}

void wc_gpu_profile_suspend(wc_gpu_profile_t* p, uint32_t frame) {
    if (!p->output || p->suspended) return;
    if (p->frame_open) wc_gpu_profile_end(p, 0, false);
    wc_gpu_profile_poll(p, frame);
    p->lifecycle_dropped += pending_count(p);
    discard_pending(p, frame, "suspended");
    p->suspended = true;
}

void wc_gpu_profile_resume(wc_gpu_profile_t* p) {
    p->suspended = false;
}

void wc_gpu_profile_destroy(wc_gpu_profile_t* p, uint32_t frame) {
    if (!p->output) return;
    if (p->frame_open) wc_gpu_profile_end(p, 0, false);
    wc_gpu_profile_poll(p, frame);
    p->pending_at_shutdown = pending_count(p);
    discard_pending(p, frame, "shutdown_pending");
    delete_queries(p);
    fprintf(p->output, "{\"event\":\"gpu_profile_summary\",\"frames_seen\":%" PRIu64
            ",\"samples_accepted\":%" PRIu64 ",\"pending\":%" PRIu64
            ",\"dropped_saturated\":%" PRIu64 ",\"invalid\":%" PRIu64
            ",\"render_failed\":%" PRIu64 ",\"procedure_failed\":%" PRIu64
            ",\"cpu_only\":%" PRIu64 ",\"disjoint_events\":%" PRIu64
            ",\"disjoint_samples\":%" PRIu64 ",\"lifecycle_dropped\":%" PRIu64 "}\n",
            p->frames_seen, p->accepted, p->pending_at_shutdown, p->saturated,
            p->invalid, p->render_failed, p->procedure_failed, p->cpu_only,
            p->disjoint_events, p->disjoint_samples, p->lifecycle_dropped);
    fflush(p->output);
    p->output = NULL;
}
