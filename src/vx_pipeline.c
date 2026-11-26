#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "vx_op.h"

/* Pool dispatch contract (implemented in vx_pool.c):
 * Attempts pthread_mutex_trylock on the worker pool.
 * - If acquired: runs fn(arg, 0..nworkers-1) in parallel (caller runs worker 0), joins, returns true.
 * - If busy (another GIL-free reader holds the pool): returns false immediately without blocking. */
typedef void (*vx_pool_fn)(void *arg, uint32_t worker_id, uint32_t nworkers);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
bool vx_pool_try_run(vx_pool_t *pool, vx_pool_fn fn, void *arg) {
    (void)pool; (void)fn; (void)arg;
    return false;
}

#define VX_SCRATCH_INIT_CAP (64u * 1024u)
#define VX_SCRATCH_MAX_CAP  (4u * 1024u * 1024u)

typedef struct vx_pipe_job {
    const vx_pipeline_t *pipe;
    const vx_exec_ctx_t *root_ctx;

    void                *src_g;
    void               **op_g;
    void                *sink_g;

    _Atomic bool         local_cancel;
    _Atomic bool        *cancelled;    /* points to root_ctx->cancelled or &local_cancel */
    _Atomic int32_t      first_err;    /* VX_OK or first failing vx_status_t */
    _Atomic bool         err_claimed;  /* single-writer guard for root_ctx->err */
} vx_pipe_job_t;

static void vx_job_record_error(vx_pipe_job_t *job, vx_status_t st, const char *msg) {
    if (st == VX_OK) return;
    atomic_store_explicit(job->cancelled, true, memory_order_release);

    bool expected = false;
    if (atomic_compare_exchange_strong_explicit(&job->err_claimed, &expected, true,
                                                memory_order_acq_rel, memory_order_relaxed)) {
        atomic_store_explicit(&job->first_err, (int32_t)st, memory_order_release);
        if (job->root_ctx->err) {
            job->root_ctx->err->code = st;
            if (msg && job->root_ctx->err->msg[0] == '\0') {
                strncpy(job->root_ctx->err->msg, msg, sizeof(job->root_ctx->err->msg) - 1u);
                job->root_ctx->err->msg[sizeof(job->root_ctx->err->msg) - 1u] = '\0';
            }
        }
    }
}

static inline vx_status_t vx_res_to_status(vx_op_res_t r, const vx_exec_ctx_t *ctx) {
    if (r == VX_OP_OOM) return VX_ERR_OOM;
    if (ctx->err && ctx->err->code != VX_OK) return ctx->err->code;
    return VX_ERR_INVALID;
}

/* Recursively pushes bufs[op_idx] through ops[op_idx..n_ops-1] -> sink.
 * Invariant: any operator returning VX_OP_HAVE_MORE_OUTPUT stores its resume cursor in
 * lstate (never in ctx->scratch), allowing op_mark to reclaim all downstream scratch
 * allocations after each emitted output batch. */
static vx_op_res_t vx_push_recursive(const vx_pipeline_t *pipe, vx_exec_ctx_t *ctx,
                                     void *const *op_g, void *const *op_l,
                                     void *sink_g, void *sink_l,
                                     vx_batch_t *bufs, uint32_t op_idx) {
    if (bufs[op_idx].sel.count == 0) {
        return VX_OP_NEED_MORE_INPUT;
    }

    /* Base case: feed terminal sink */
    if (op_idx == pipe->n_ops) {
        return pipe->sink.vtbl->sink(&pipe->sink, ctx, sink_g, sink_l, &bufs[op_idx]);
    }

    const vx_op_t *op  = &pipe->ops[op_idx];
    vx_batch_t    *in  = &bufs[op_idx];
    vx_batch_t    *out = &bufs[op_idx + 1u];

    for (;;) {
        if (atomic_load_explicit(ctx->cancelled, memory_order_relaxed)) {
            return VX_OP_FINISHED;
        }

        vx_arena_mark_t op_mark = vx_arena_mark(ctx->scratch);
        vx_batch_reset(out);

        vx_op_res_t r = op->vtbl->execute(op, ctx, op_g[op_idx], op_l[op_idx], in, out);
        if (r < 0) {
            vx_batch_reset(out);
            vx_arena_rewind(ctx->scratch, op_mark);
            return r;
        }

        /* Flush produced rows even when r == VX_OP_FINISHED (e.g., Slice final partial batch) */
        vx_op_res_t dr = VX_OP_NEED_MORE_INPUT;
        if (out->sel.count > 0) {
            dr = vx_push_recursive(pipe, ctx, op_g, op_l, sink_g, sink_l, bufs, op_idx + 1u);
        }

        /* Release batch owners BEFORE rewinding scratch so scratch->hdr.refcnt stays balanced */
        vx_batch_reset(out);
        vx_arena_rewind(ctx->scratch, op_mark);

        if (dr == VX_OP_FINISHED || dr < 0) {
            return dr;
        }
        if (r == VX_OP_HAVE_MORE_OUTPUT) {
            continue; /* resume operator with the exact same `in` batch */
        }
        return r; /* VX_OP_NEED_MORE_INPUT or VX_OP_FINISHED */
    }
}

static void vx_pipeline_worker(void *arg, uint32_t worker_id, uint32_t nworkers) {
    vx_pipe_job_t       *job  = (vx_pipe_job_t *)arg;
    const vx_pipeline_t *pipe = job->pipe;
    const uint32_t       nops = pipe->n_ops;

    vx_arena_t *scratch = vx_arena_create(VX_SCRATCH_INIT_CAP, VX_SCRATCH_MAX_CAP, 0);
    if (!scratch) {
        vx_job_record_error(job, VX_ERR_OOM, "out of memory allocating worker scratch");
        return;
    }

    vx_err_t local_err;
    memset(&local_err, 0, sizeof(local_err));

    vx_exec_ctx_t ctx = {
        .worker_id      = worker_id,
        .nworkers       = nworkers,
        .scratch        = scratch,
        .out_arena      = job->root_ctx->out_arena,
        .simd           = job->root_ctx->simd,
        .work_mem_bytes = job->root_ctx->work_mem_bytes,
        .cancelled      = job->cancelled,
        .err            = &local_err
    };

    void       *src_l       = NULL;
    void      **op_l        = NULL;
    void       *sink_l      = NULL;
    vx_batch_t *bufs        = NULL;
    uint32_t    op_l_init   = 0;
    uint32_t    bufs_init   = 0;
    bool        src_l_ready = false;
    bool        sink_ready  = false;
    vx_status_t st          = VX_OK;

    bufs = (vx_batch_t *)calloc((size_t)nops + 1u, sizeof(*bufs));
    if (nops > 0) op_l = (void **)calloc(nops, sizeof(*op_l));
    if (!bufs || (nops > 0 && !op_l)) {
        st = VX_ERR_OOM;
        goto cleanup;
    }

    /* Initialize stage batches: bufs[0] = source out, bufs[k+1] = ops[k] out */
    st = vx_batch_init(&bufs[0], pipe->source.out_ncols, pipe->source.out_types);
    if (st != VX_OK) goto cleanup;
    bufs_init = 1u;

    for (uint32_t k = 0; k < nops; ++k) {
        st = vx_batch_init(&bufs[k + 1u], pipe->ops[k].out_ncols, pipe->ops[k].out_types);
        if (st != VX_OK) goto cleanup;
        bufs_init = k + 2u;
    }

    /* Initialize thread-local operator states */
    st = pipe->source.vtbl->init_local(&pipe->source, &ctx, job->src_g, &src_l);
    if (st != VX_OK) goto cleanup;
    src_l_ready = true;

    for (uint32_t k = 0; k < nops; ++k) {
        st = pipe->ops[k].vtbl->init_local(&pipe->ops[k], &ctx, job->op_g[k], &op_l[k]);
        if (st != VX_OK) goto cleanup;
        op_l_init = k + 1u;
    }

    st = pipe->sink.vtbl->init_local(&pipe->sink, &ctx, job->sink_g, &sink_l);
    if (st != VX_OK) goto cleanup;
    sink_ready = true;

    /* Main morsel pull-and-push loop */
    vx_arena_mark_t morsel_mark = vx_arena_mark(scratch);
    while (!atomic_load_explicit(ctx.cancelled, memory_order_relaxed)) {
        vx_batch_reset(&bufs[0]);
        vx_op_res_t sr = pipe->source.vtbl->next_batch(&pipe->source, &ctx,
                                                       job->src_g, src_l, &bufs[0]);
        if (sr == VX_OP_FINISHED) {
            break;
        }
        if (sr < 0) {
            st = vx_res_to_status(sr, &ctx);
            break;
        }

        vx_op_res_t pr = vx_push_recursive(pipe, &ctx, job->op_g, op_l,
                                           job->sink_g, sink_l, bufs, 0u);
        vx_batch_reset(&bufs[0]);
        vx_arena_rewind(scratch, morsel_mark);

        if (pr == VX_OP_FINISHED) {
            /* Early termination (e.g. Slice [:N] satisfied): wake & stop sibling workers */
            atomic_store_explicit(ctx.cancelled, true, memory_order_release);
            break;
        }
        if (pr < 0) {
            st = vx_res_to_status(pr, &ctx);
            break;
        }
    }

    /* Merge worker-local sink state if pipeline did not fail */
    if (st == VX_OK && atomic_load_explicit(&job->first_err, memory_order_acquire) == VX_OK) {
        st = pipe->sink.vtbl->combine(&pipe->sink, &ctx, job->sink_g, sink_l);
    }

cleanup:
    if (st != VX_OK) {
        vx_job_record_error(job, st, local_err.msg[0] ? local_err.msg : NULL);
    }
    if (bufs) {
        for (uint32_t b = 0; b < bufs_init; ++b) vx_batch_destroy(&bufs[b]);
        free(bufs);
    }
    if (sink_ready) pipe->sink.vtbl->destroy_local(sink_l);
    for (uint32_t k = op_l_init; k > 0; --k) {
        pipe->ops[k - 1u].vtbl->destroy_local(op_l[k - 1u]);
    }
    free(op_l);
    if (src_l_ready) pipe->source.vtbl->destroy_local(src_l);
    vx_owner_release(&scratch->hdr);
}

vx_status_t vx_pipeline_execute(const vx_pipeline_t *pipe, vx_pool_t *pool,
                                vx_exec_ctx_t *root_ctx, void **out_sink_gstate) {
    if (!pipe || !root_ctx || !out_sink_gstate) return VX_ERR_INVALID;
    *out_sink_gstate = NULL;

    vx_pipe_job_t job;
    memset(&job, 0, sizeof(job));
    job.pipe     = pipe;
    job.root_ctx = root_ctx;
    atomic_init(&job.local_cancel, false);
    job.cancelled = root_ctx->cancelled ? root_ctx->cancelled : &job.local_cancel;
    atomic_init(&job.first_err, (int32_t)VX_OK);
    atomic_init(&job.err_claimed, false);

    uint32_t    op_g_init  = 0;
    bool        src_ready  = false;
    bool        sink_ready = false;
    vx_status_t st         = VX_OK;

    if (pipe->n_ops > 0) {
        job.op_g = (void **)calloc(pipe->n_ops, sizeof(*job.op_g));
        if (!job.op_g) return VX_ERR_OOM;
    }

    st = pipe->source.vtbl->init_global(&pipe->source, root_ctx, &job.src_g);
    if (st != VX_OK) goto fail_init;
    src_ready = true;

    for (uint32_t k = 0; k < pipe->n_ops; ++k) {
        st = pipe->ops[k].vtbl->init_global(&pipe->ops[k], root_ctx, &job.op_g[k]);
        if (st != VX_OK) goto fail_init;
        op_g_init = k + 1u;
    }

    st = pipe->sink.vtbl->init_global(&pipe->sink, root_ctx, &job.sink_g);
    if (st != VX_OK) goto fail_init;
    sink_ready = true;

    /* Trylock parallel dispatch; fall back to inline caller execution if pool is contended */
    if (!pool || !vx_pool_try_run(pool, vx_pipeline_worker, &job)) {
        vx_pipeline_worker(&job, 0u, 1u);
    }

    st = (vx_status_t)atomic_load_explicit(&job.first_err, memory_order_acquire);
    if (st == VX_OK) {
        st = pipe->sink.vtbl->finalize(&pipe->sink, root_ctx, job.sink_g);
    }

fail_init:
    for (uint32_t k = op_g_init; k > 0; --k) {
        pipe->ops[k - 1u].vtbl->destroy_global(job.op_g[k - 1u]);
    }
    free(job.op_g);
    if (src_ready) pipe->source.vtbl->destroy_global(job.src_g);

    if (st != VX_OK) {
        if (sink_ready) pipe->sink.vtbl->destroy_global(job.sink_g);
        return st;
    }

    *out_sink_gstate = job.sink_g;
    return VX_OK;
}

