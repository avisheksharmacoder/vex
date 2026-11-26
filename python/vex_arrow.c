#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vx_result.h"
#include "vex_arrow.h"

/* ============================================================================
 * 1. Standalone Arrow C Data & Stream Interface ABI Definitions
 * ============================================================================ */
#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE           2
#define ARROW_FLAG_MAP_KEYS_SORTED    4

struct ArrowSchema {
    const char          *format;
    const char          *name;
    const char          *metadata;
    int64_t              flags;
    int64_t              n_children;
    struct ArrowSchema **children;
    struct ArrowSchema  *dictionary;
    void               (*release)(struct ArrowSchema *);
    void                *private_data;
};

struct ArrowArray {
    int64_t             length;
    int64_t             null_count;
    int64_t             offset;
    int64_t             n_buffers;
    int64_t             n_children;
    const void        **buffers;
    struct ArrowArray **children;
    struct ArrowArray  *dictionary;
    void              (*release)(struct ArrowArray *);
    void               *private_data;
};
#endif /* ARROW_C_DATA_INTERFACE */

#ifndef ARROW_C_STREAM_INTERFACE
#define ARROW_C_STREAM_INTERFACE
struct ArrowArrayStream {
    int         (*get_schema)(struct ArrowArrayStream *, struct ArrowSchema *out);
    int         (*get_next)(struct ArrowArrayStream *, struct ArrowArray *out);
    const char *(*get_last_error)(struct ArrowArrayStream *);
    void        (*release)(struct ArrowArrayStream *);
    void         *private_data;
};
#endif /* ARROW_C_STREAM_INTERFACE */

static const int64_t  k_empty_variadic_sizes[1] = { 0 };
static const void    *k_struct_null_buf[1]      = { NULL };

static inline uint32_t vx_arrow_popcnt64(uint64_t x) {
#if defined(_MSC_VER)
    return (uint32_t)__popcnt64(x);
#else
    return (uint32_t)__builtin_popcountll(x);
#endif
}

static inline const char *vx_type_to_arrow_fmt(vx_type_t t) {
    switch (t) {
    case VX_TYPE_BOOL: return "b";  /* 1-bit LSB-packed boolean */
    case VX_TYPE_I64:  return "l";  /* int64 */
    case VX_TYPE_U64:  return "L";  /* uint64 */
    case VX_TYPE_F64:  return "g";  /* float64 */
    case VX_TYPE_STR:  return "vu"; /* Arrow UTF-8 StringView (16B inline/variadic) */
    }
    return NULL;
}

static int64_t vx_count_nulls(const uint64_t *validity, uint32_t nrows) {
    if (!validity || nrows == 0) return 0;
    uint32_t full = nrows >> 6;
    uint32_t rem  = nrows & 63u;
    uint64_t val  = 0;
    for (uint32_t w = 0; w < full; ++w) {
        val += vx_arrow_popcnt64(validity[w]);
    }
    if (rem > 0) {
        val += vx_arrow_popcnt64(validity[full] & ((1ULL << rem) - 1ULL));
    }
    return (int64_t)(nrows - val);
}

static char *vx_strdup_c(const char *s) {
    const char *src = s ? s : "";
    size_t      len = strlen(src);
    char       *dst = (char *)malloc(len + 1u);
    if (dst) memcpy(dst, src, len + 1u);
    return dst;
}

/* ============================================================================
 * 2. ArrowSchema Builders & Release Callbacks (Zero Python/GIL Dependency)
 * ============================================================================ */
static void vx_arrow_schema_release(struct ArrowSchema *schema) {
    if (!schema || !schema->release) return;
    if (schema->children) {
        for (int64_t i = 0; i < schema->n_children; ++i) {
            struct ArrowSchema *ch = schema->children[i];
            if (ch) {
                if (ch->release) ch->release(ch);
                free(ch);
            }
        }
        free(schema->children);
    }
    if (schema->dictionary) {
        if (schema->dictionary->release) schema->dictionary->release(schema->dictionary);
        free(schema->dictionary);
    }
    free((void *)schema->name); /* owned heap copy from vx_strdup_c */
    schema->children     = NULL;
    schema->dictionary   = NULL;
    schema->name         = NULL;
    schema->private_data = NULL;
    schema->release      = NULL;
}

static int vx_arrow_init_col_schema(struct ArrowSchema *out, const char *name, vx_type_t type) {
    const char *fmt = vx_type_to_arrow_fmt(type);
    if (!fmt) return EINVAL;

    memset(out, 0, sizeof(*out));
    out->format = fmt; /* static literal */
    out->name   = vx_strdup_c(name);
    if (!out->name) return ENOMEM;

    out->flags   = ARROW_FLAG_NULLABLE;
    out->release = vx_arrow_schema_release;
    return 0;
}

static int vx_arrow_init_result_schema(const vx_result_t *res, struct ArrowSchema *out) {
    memset(out, 0, sizeof(*out));
    out->format     = "+s"; /* RecordBatch struct */
    out->name       = vx_strdup_c("");
    out->flags      = 0;
    out->n_children = (int64_t)res->ncols;
    if (!out->name) return ENOMEM;

    if (res->ncols > 0) {
        out->children = (struct ArrowSchema **)calloc(res->ncols, sizeof(*out->children));
        if (!out->children) {
            free((void *)out->name);
            memset(out, 0, sizeof(*out));
            return ENOMEM;
        }
        for (uint32_t c = 0; c < res->ncols; ++c) {
            struct ArrowSchema *ch = (struct ArrowSchema *)calloc(1u, sizeof(*ch));
            if (!ch) goto fail;
            int rc = vx_arrow_init_col_schema(ch, res->col_names[c], res->col_types[c]);
            if (rc != 0) { free(ch); goto fail; }
            out->children[c] = ch;
        }
    }

    out->release = vx_arrow_schema_release;
    return 0;

fail:
    out->release = vx_arrow_schema_release;
    out->release(out);
    return ENOMEM;
}

/* ============================================================================
 * 3. Zero-Copy ArrowArray Builders (Numeric, Validity, StringView, Bool)
 * ============================================================================ */
typedef struct vx_arrow_arr_priv {
    vx_owner_t  *owner;     /* pins backing chunk / mmap segment / arena */
    vx_owner_t  *aux_owner; /* optional secondary pin (e.g., &res->hdr for stitched strpool) */
    const void **buffers;   /* heap buffer pointer table (NULL for static struct buffer) */
    void        *owned_buf; /* non-NULL only for bit-packed VX_TYPE_BOOL */
} vx_arrow_arr_priv_t;

static void vx_arrow_array_release(struct ArrowArray *arr) {
    if (!arr || !arr->release) return;

    if (arr->children) {
        for (int64_t i = 0; i < arr->n_children; ++i) {
            struct ArrowArray *ch = arr->children[i];
            if (ch) {
                if (ch->release) ch->release(ch);
                free(ch);
            }
        }
        free(arr->children);
    }
    if (arr->dictionary) {
        if (arr->dictionary->release) arr->dictionary->release(arr->dictionary);
        free(arr->dictionary);
    }

    vx_arrow_arr_priv_t *priv = (vx_arrow_arr_priv_t *)arr->private_data;
    if (priv) {
        free(priv->buffers);
        free(priv->owned_buf);
        vx_owner_release(priv->owner);
        vx_owner_release(priv->aux_owner);
        free(priv);
    }

    arr->buffers      = NULL;
    arr->children     = NULL;
    arr->dictionary   = NULL;
    arr->private_data = NULL;
    arr->release      = NULL;
}

static int vx_arrow_export_vec(const vx_vec_t *vec, uint32_t nrows,
                               vx_owner_t *aux_owner, struct ArrowArray *out) {
    if (!vec || !out || vec->kind != VX_VEC_FLAT) return EINVAL;
    memset(out, 0, sizeof(*out));

    vx_arrow_arr_priv_t *priv = (vx_arrow_arr_priv_t *)calloc(1u, sizeof(*priv));
    if (!priv) return ENOMEM;

    const uint64_t *val   = (vec->has_nulls && vec->validity) ? vec->validity : NULL;
    const int64_t   nulls = vx_count_nulls(val, nrows);
    const void     *val_p = (nulls > 0) ? (const void *)val : NULL;

    if (vec->type == VX_TYPE_STR) {
        /* Arrow StringView ("vu"): buffers = [validity, views, data_0..data_{k-1}, sizes] */
        const uint32_t nbufs = (vec->strpool) ? vec->strpool->nbufs : 0u;
        const size_t   nb    = (size_t)nbufs + 3u;
        priv->buffers = (const void **)calloc(nb, sizeof(const void *));
        if (!priv->buffers) { free(priv); return ENOMEM; }

        priv->buffers[0] = val_p;
        priv->buffers[1] = (nrows > 0) ? vec->data : NULL;
        for (uint32_t b = 0; b < nbufs; ++b) {
            priv->buffers[2u + b] = vec->strpool->ptrs[b];
        }
        priv->buffers[2u + nbufs] = (nbufs > 0)
            ? (const void *)vec->strpool->lens
            : (const void *)k_empty_variadic_sizes;

        out->n_buffers = (int64_t)nb;
        out->buffers   = priv->buffers;
    } else if (vec->type == VX_TYPE_BOOL) {
        /* Arrow boolean ("b") requires LSB bit-packed 1-bit values */
        priv->buffers = (const void **)calloc(2u, sizeof(const void *));
        if (!priv->buffers) { free(priv); return ENOMEM; }

        const size_t nbytes = ((size_t)nrows + 7u) >> 3;
        uint8_t     *packed = (uint8_t *)calloc(nbytes ? nbytes : 1u, 1u);
        if (!packed) { free(priv->buffers); free(priv); return ENOMEM; }

        const uint8_t *src = (const uint8_t *)vec->data;
        for (uint32_t i = 0; i < nrows; ++i) {
            if (src[i]) packed[i >> 3] |= (uint8_t)(1u << (i & 7u));
        }

        priv->owned_buf  = packed;
        priv->buffers[0] = val_p;
        priv->buffers[1] = packed;
        out->n_buffers   = 2;
        out->buffers     = priv->buffers;
    } else {
        /* Strictly zero-copy numeric export (i64 / u64 / f64) */
        priv->buffers = (const void **)calloc(2u, sizeof(const void *));
        if (!priv->buffers) { free(priv); return ENOMEM; }

        priv->buffers[0] = val_p;
        priv->buffers[1] = (nrows > 0) ? vec->data : NULL;
        out->n_buffers   = 2;
        out->buffers     = priv->buffers;
    }

    priv->owner       = vx_owner_retain(vec->owner);
    priv->aux_owner   = vx_owner_retain(aux_owner);
    out->length       = (int64_t)nrows;
    out->null_count   = nulls;
    out->offset       = 0;
    out->private_data = priv;
    out->release      = vx_arrow_array_release;
    return 0;
}

static int vx_arrow_export_batch(const vx_result_t *res, uint32_t batch_idx,
                                 struct ArrowArray *out) {
    const vx_res_batch_t *rb = &res->batches[batch_idx];
    memset(out, 0, sizeof(*out));

    out->length     = (int64_t)rb->nrows;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->buffers    = k_struct_null_buf; /* struct array itself has no nulls */
    out->n_children = (int64_t)res->ncols;

    if (res->ncols > 0) {
        out->children = (struct ArrowArray **)calloc(res->ncols, sizeof(*out->children));
        if (!out->children) return ENOMEM;

        for (uint32_t c = 0; c < res->ncols; ++c) {
            struct ArrowArray *ch = (struct ArrowArray *)calloc(1u, sizeof(*ch));
            if (!ch) goto fail;
            int rc = vx_arrow_export_vec(&rb->cols[c], rb->nrows, NULL, ch);
            if (rc != 0) {
                free(ch);
                out->release = vx_arrow_array_release;
                out->release(out);
                return rc;
            }
            out->children[c] = ch;
        }
    }

    out->release = vx_arrow_array_release;
    return 0;

fail:
    out->release = vx_arrow_array_release;
    out->release(out);
    return ENOMEM;
}

/* ============================================================================
 * 4. ArrowArrayStream Iterator over vx_result_t->batches[0..nbatches-1]
 * ============================================================================ */
typedef struct vx_arrow_stream_priv {
    vx_result_t *res;        /* retained vx_owner_t (decoupled from Python Result lifetime) */
    uint32_t     next_batch;
    char         last_err[128];
} vx_arrow_stream_priv_t;

static int vx_stream_get_schema(struct ArrowArrayStream *stream, struct ArrowSchema *out) {
    if (!stream || !stream->private_data || !out) return EINVAL;
    vx_arrow_stream_priv_t *priv = (vx_arrow_stream_priv_t *)stream->private_data;
    int rc = vx_arrow_init_result_schema(priv->res, out);
    if (rc != 0) snprintf(priv->last_err, sizeof(priv->last_err), "failed to build ArrowSchema");
    return rc;
}

static int vx_stream_get_next(struct ArrowArrayStream *stream, struct ArrowArray *out) {
    if (!stream || !stream->private_data || !out) return EINVAL;
    vx_arrow_stream_priv_t *priv = (vx_arrow_stream_priv_t *)stream->private_data;

    if (priv->next_batch >= priv->res->nbatches) {
        /* Arrow EOF convention: return 0 with out->release = NULL */
        memset(out, 0, sizeof(*out));
        out->release = NULL;
        return 0;
    }

    int rc = vx_arrow_export_batch(priv->res, priv->next_batch, out);
    if (rc != 0) {
        snprintf(priv->last_err, sizeof(priv->last_err),
                 "failed to export batch %u (errno=%d)", priv->next_batch, rc);
        memset(out, 0, sizeof(*out));
        out->release = NULL;
        return rc;
    }

    priv->next_batch++;
    return 0;
}

static const char *vx_stream_get_last_error(struct ArrowArrayStream *stream) {
    if (!stream || !stream->private_data) return NULL;
    vx_arrow_stream_priv_t *priv = (vx_arrow_stream_priv_t *)stream->private_data;
    return priv->last_err[0] ? priv->last_err : NULL;
}

static void vx_stream_release(struct ArrowArrayStream *stream) {
    if (!stream || !stream->release) return;
    vx_arrow_stream_priv_t *priv = (vx_arrow_stream_priv_t *)stream->private_data;
    if (priv) {
        if (priv->res) vx_owner_release(&priv->res->hdr);
        free(priv);
    }
    stream->private_data = NULL;
    stream->release      = NULL;
}

/* ============================================================================
 * 5. Arrow PyCapsule Protocol Exports (Called from python/vex_py.c)
 * ============================================================================ */
static void vx_capsule_schema_destructor(PyObject *cap) {
    struct ArrowSchema *s = (struct ArrowSchema *)PyCapsule_GetPointer(cap, "arrow_schema");
    if (!s) { PyErr_Clear(); return; }
    if (s->release) s->release(s);
    free(s);
}

static void vx_capsule_array_destructor(PyObject *cap) {
    struct ArrowArray *a = (struct ArrowArray *)PyCapsule_GetPointer(cap, "arrow_array");
    if (!a) { PyErr_Clear(); return; }
    if (a->release) a->release(a);
    free(a);
}

static void vx_capsule_stream_destructor(PyObject *cap) {
    struct ArrowArrayStream *st =
        (struct ArrowArrayStream *)PyCapsule_GetPointer(cap, "arrow_array_stream");
    if (!st) { PyErr_Clear(); return; }
    if (st->release) st->release(st);
    free(st);
}

/* Result.__arrow_c_schema__(self) -> PyCapsule("arrow_schema") */
PyObject *vex_result_arrow_c_schema(vx_result_t *res) {
    if (!res) { PyErr_SetString(PyExc_ValueError, "null result"); return NULL; }
    struct ArrowSchema *s = (struct ArrowSchema *)calloc(1u, sizeof(*s));
    if (!s) return PyErr_NoMemory();

    int rc = vx_arrow_init_result_schema(res, s);
    if (rc != 0) {
        free(s);
        return (rc == ENOMEM) ? PyErr_NoMemory()
                              : PyErr_Format(PyExc_RuntimeError, "ArrowSchema error (%d)", rc);
    }

    PyObject *cap = PyCapsule_New(s, "arrow_schema", vx_capsule_schema_destructor);
    if (!cap) {
        if (s->release) s->release(s);
        free(s);
        return NULL;
    }
    return cap;
}

/* Result.__arrow_c_stream__(self, requested_schema=None) -> PyCapsule("arrow_array_stream") */
PyObject *vex_result_arrow_c_stream(vx_result_t *res, PyObject *requested_schema) {
    if (!res) { PyErr_SetString(PyExc_ValueError, "null result"); return NULL; }
    if (requested_schema && requested_schema != Py_None) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "Vex __arrow_c_stream__ does not support requested_schema casting");
        return NULL;
    }

    struct ArrowArrayStream *stream = (struct ArrowArrayStream *)calloc(1u, sizeof(*stream));
    vx_arrow_stream_priv_t  *priv   = (vx_arrow_stream_priv_t *)calloc(1u, sizeof(*priv));
    if (!stream || !priv) {
        free(stream);
        free(priv);
        return PyErr_NoMemory();
    }

    priv->res            = (vx_result_t *)vx_owner_retain(&res->hdr);
    priv->next_batch     = 0u;
    stream->get_schema   = vx_stream_get_schema;
    stream->get_next     = vx_stream_get_next;
    stream->get_last_error = vx_stream_get_last_error;
    stream->release      = vx_stream_release;
    stream->private_data = priv;

    PyObject *cap = PyCapsule_New(stream, "arrow_array_stream", vx_capsule_stream_destructor);
    if (!cap) {
        stream->release(stream);
        free(stream);
        return NULL;
    }
    return cap;
}

/* ColumnView.__arrow_c_array__(self, requested_schema=None) -> (schema_cap, array_cap) */
PyObject *vex_colview_arrow_c_array(const vx_vec_t *vec, const char *col_name,
                                    vx_owner_t *aux_owner, PyObject *requested_schema) {
    if (!vec) { PyErr_SetString(PyExc_ValueError, "null column view"); return NULL; }
    if (requested_schema && requested_schema != Py_None) {
        PyErr_SetString(PyExc_NotImplementedError,
                        "Vex __arrow_c_array__ does not support requested_schema casting");
        return NULL;
    }

    struct ArrowSchema *s = (struct ArrowSchema *)calloc(1u, sizeof(*s));
    struct ArrowArray  *a = (struct ArrowArray *)calloc(1u, sizeof(*a));
    if (!s || !a) {
        free(s);
        free(a);
        return PyErr_NoMemory();
    }

    int rc = vx_arrow_init_col_schema(s, col_name, vec->type);
    if (rc == 0) rc = vx_arrow_export_vec(vec, vec->phys_len, aux_owner, a);
    if (rc != 0) {
        if (s->release) s->release(s);
        if (a->release) a->release(a);
        free(s);
        free(a);
        return (rc == ENOMEM) ? PyErr_NoMemory()
                              : PyErr_Format(PyExc_RuntimeError, "ArrowArray export error (%d)", rc);
    }

    PyObject *sc = PyCapsule_New(s, "arrow_schema", vx_capsule_schema_destructor);
    if (!sc) {
        if (s->release) s->release(s);
        if (a->release) a->release(a);
        free(s);
        free(a);
        return NULL;
    }

    PyObject *ac = PyCapsule_New(a, "arrow_array", vx_capsule_array_destructor);
    if (!ac) {
        Py_DECREF(sc);
        if (a->release) a->release(a);
        free(a);
        return NULL;
    }

    PyObject *tup = PyTuple_Pack(2, sc, ac);
    Py_DECREF(sc);
    Py_DECREF(ac);
    return tup;
}

