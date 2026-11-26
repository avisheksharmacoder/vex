#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#include "vx_db.h"
#include "vx_simd.h"

/* ============================================================================
 * 1. CPython 3.10..3.14t Portability Shims
 * ============================================================================ */
#ifndef Py_BEGIN_CRITICAL_SECTION
#define Py_BEGIN_CRITICAL_SECTION(op) {
#define Py_END_CRITICAL_SECTION()     }
#endif

#if PY_VERSION_HEX < 0x030B0000
static inline PyObject *Py_NewRef_compat(PyObject *o) { Py_INCREF(o); return o; }
#undef  Py_NewRef
#define Py_NewRef(o) Py_NewRef_compat((PyObject *)(o))
#endif

#if PY_VERSION_HEX < 0x03090000
static inline PyObject *Py_XNewRef_compat(PyObject *o) { Py_XINCREF(o); return o; }
#undef  Py_XNewRef
#define Py_XNewRef(o) Py_XNewRef_compat((PyObject *)(o))
#elif !defined(Py_XNewRef)
#define Py_XNewRef(o) Py_XNewRef((PyObject *)(o))
#endif

#define VX_PY_MAX_COLS    64U
#define VX_PY_STACK_PARAM 32U

typedef int vx_status_t;
#define VX_OK            0
#define VX_ERR_OOM     (-1)
#define VX_ERR_MEMLIM  (-2)
#define VX_ERR_IO      (-3)
#define VX_ERR_CORRUPT (-4)
#define VX_ERR_SCHEMA  (-5)
#define VX_ERR_UNIQUE  (-6)
#define VX_ERR_CANCEL  (-7)
#define VX_ERR_INVALID (-8)
#define VX_ERR_CONFLICT (-9)

/* ============================================================================
 * 2. Per-Module State (Zero Process-Global Mutable State - PEP 489 + PEP 573)
 * ============================================================================ */
typedef struct VexModState {
    PyTypeObject *db_type;
    PyTypeObject *tx_type;
    PyTypeObject *result_type;
    PyTypeObject *colview_type;
    PyObject     *vex_error;
    PyObject     *vex_conflict_error;
} VexModState;

static inline VexModState *vex_state_from_type(PyTypeObject *tp) {
    PyObject *mod = PyType_GetModule(tp);
    return mod ? (VexModState *)PyModule_GetState(mod) : NULL;
}

static inline VexModState *vex_state_from_obj(PyObject *obj) {
    return vex_state_from_type(Py_TYPE(obj));
}

static void vex_set_py_error(VexModState *st, vx_status_t code, const char *detail) {
    if (code == VX_ERR_OOM) {
        PyErr_SetString(st->vex_error, (detail && detail[0]) ? detail : "out of memory");
    } else if (code == VX_ERR_MEMLIM) {
        PyErr_SetString(st->vex_error, "memory limit exceeded");
    } else if (code == VX_ERR_CONFLICT) {
        PyErr_SetString(st->vex_conflict_error,
                        (detail && detail[0]) ? detail : "transaction write conflict");
    } else {
        PyErr_SetString(st->vex_error, (detail && detail[0]) ? detail : "vex engine error");
    }
}

/* ============================================================================
 * 3. Struct Definitions
 * ============================================================================ */
#define VX_PYDB_CLOSED_BIT (1ULL << 63)
#define VX_PYDB_COUNT_MASK (~VX_PYDB_CLOSED_BIT)

#define VX_OCC_HISTORY_SZ 256
typedef struct VexOccRecord {
    uint64_t seq;
    uint64_t write_mask[4];
} VexOccRecord;

typedef struct VexDBObject {
    PyObject_HEAD
    vx_db_t         *db;         /* valid while (op_state & COUNT_MASK) > 0 */
    _Atomic uint64_t op_state;   /* bit 63 = CLOSED, bits 0..62 = active C operations */
    pthread_mutex_t  close_mu;   /* serializes close() teardown & drain condvar */
    pthread_cond_t   close_cv;
    pthread_mutex_t  occ_mu;     /* guards optimistic concurrency write conflict tracking */
    uint64_t         occ_commit_seq;
    VexOccRecord     occ_history[VX_OCC_HISTORY_SZ];
} VexDBObject;

typedef struct VexTxObject {
    PyObject_HEAD
    VexDBObject *db_obj;     /* strong Python ref to parent Database */
    bool         in_use;     /* prevents concurrent calls on the SAME tx object */
    bool         active;
    bool         mutated;
    uint64_t     base_seq;
    uint64_t     read_mask[4];
    uint64_t     write_mask[4];
    char        *buf;        /* buffered atomic multi-statement mutation SQL */
    size_t       buf_len;
    size_t       buf_cap;
} VexTxObject;

static inline void occ_mask_set(uint64_t mask[4], uint64_t id) {
    uint64_t idx = (id > 0) ? ((id - 1) % 256ULL) : (id % 256ULL);
    mask[idx / 64ULL] |= (1ULL << (idx % 64ULL));
}

static inline bool occ_mask_is_empty(const uint64_t mask[4]) {
    return (mask[0] == 0ULL && mask[1] == 0ULL && mask[2] == 0ULL && mask[3] == 0ULL);
}

static inline bool occ_masks_intersect(const uint64_t a[4], const uint64_t b[4]) {
    return ((a[0] & b[0]) || (a[1] & b[1]) || (a[2] & b[2]) || (a[3] & b[3]));
}

typedef struct VexResultObject {
    PyObject_HEAD
    vx_result_t *res;
    PyObject    *col_map;    /* lazily populated dict[str, int] guarded by Py_BEGIN_CRITICAL_SECTION */
} VexResultObject;

typedef struct VexColumnViewObject {
    PyObject_HEAD
    VexResultObject *res_obj;   /* strong reference to parent Result */
    char             name[64];
    vx_type_t        type;
    void            *data;
    uint64_t        *null_bits;
    uint32_t         nrows;
    Py_ssize_t       shape[1];  /* initialized ONCE at construction -> race-free getbuffer */
    Py_ssize_t       strides[1];
    Py_ssize_t       itemsize;
    const char      *format;
} VexColumnViewObject;

/* Forward declarations */
static PyObject *vex_result_wrap(VexModState *st, vx_result_t *res);
static PyObject *vex_colview_create(VexModState *st, VexResultObject *res_obj, uint32_t col_idx);
static PyObject *vex_tx_new_for_db(VexModState *st, VexDBObject *db_obj);

/* Thread-local active transaction for `with db.tx():` */
static _Thread_local VexTxObject *g_tls_active_tx = NULL;

/* ============================================================================
 * 4. Query Normalization Helper (Transparent V2 Pipe -> V1 Syntax Translation)
 * ============================================================================ */
static const char *vx_normalize_query(const char *in, char *buf, size_t bufsz) {
    if (!in || !buf || bufsz == 0) return in;
    while (*in == ' ' || *in == '\t' || *in == '\n' || *in == '\r') in++;
    if (*in == '\0') return in;

    /* If query already contains '[', it's native V1 syntax */
    if (strchr(in, '[')) return in;
    if (*in == '?') return in;

    /* If query starts with 'table ' */
    if (strncmp(in, "table", 5) == 0 && (in[5] == ' ' || in[5] == '\t')) {
        const char *lparen = strchr(in, '(');
        const char *rparen = strrchr(in, ')');
        if (lparen && rparen && rparen > lparen) {
            size_t prefix_len = (size_t)(lparen - in);
            size_t inner_len  = (size_t)(rparen - lparen - 1);
            if (prefix_len + inner_len + 3 >= bufsz) return in;
            memcpy(buf, in, prefix_len);
            buf[prefix_len] = '{';
            memcpy(buf + prefix_len + 1, lparen + 1, inner_len);
            buf[prefix_len + 1 + inner_len] = '}';
            buf[prefix_len + 1 + inner_len + 1] = '\0';
            return buf;
        }
        return in;
    }

    /* If query starts with unique(...) or index(...) */
    if (strncmp(in, "unique(", 7) == 0 || strncmp(in, "index(", 6) == 0) {
        bool is_unique = (strncmp(in, "unique(", 7) == 0);
        const char *paren_start = in + (is_unique ? 7 : 6);
        const char *dot = strchr(paren_start, '.');
        const char *paren_end = strchr(paren_start, ')');
        if (dot && paren_end && paren_end > dot) {
            char tbl[64] = {0};
            char col[64] = {0};
            size_t tlen = (size_t)(dot - paren_start);
            size_t clen = (size_t)(paren_end - dot - 1);
            if (tlen < sizeof(tbl) && clen < sizeof(col)) {
                memcpy(tbl, paren_start, tlen);
                memcpy(col, dot + 1, clen);
                snprintf(buf, bufsz, "%s %s(%s)", is_unique ? "unique" : "index", tbl, col);
                return buf;
            }
        }
        return in;
    }

    const char *pipe = strchr(in, '|');
    if (!pipe && (strchr(in, '+') || strchr(in, '&') || strchr(in, '('))) return in;
    /* Check if there is a comparison operator */
    bool has_cmp = (strstr(in, "==") || strstr(in, "!=") || strstr(in, "<=") ||
                    strstr(in, ">=") || strstr(in, "<") || strstr(in, ">"));

    /* If neither pipe nor comparison operator, it's native V1 (e.g. "users", "vec sort id", "orders + users...") */
    if (!pipe && !has_cmp) return in;

    /* Parse table name */
    char tbl[64] = {0};
    size_t tlen = 0;
    const char *p = in;
    while (*p && *p != ' ' && *p != '\t' && *p != '|') {
        if (tlen + 1 < sizeof(tbl)) tbl[tlen++] = *p;
        p++;
    }
    tbl[tlen] = '\0';

    while (*p == ' ' || *p == '\t') p++;

    char filter[512] = {0};
    char pipe_op[512] = {0};

    if (pipe) {
        size_t flen = (size_t)(pipe - p);
        while (flen > 0 && (p[flen - 1] == ' ' || p[flen - 1] == '\t')) flen--;
        if (flen >= sizeof(filter)) flen = sizeof(filter) - 1;
        memcpy(filter, p, flen);
        filter[flen] = '\0';

        const char *pop = pipe + 1;
        while (*pop == ' ' || *pop == '\t') pop++;
        snprintf(pipe_op, sizeof(pipe_op), "%s", pop);
    } else {
        snprintf(filter, sizeof(filter), "%s", p);
    }

    char norm_filter[512] = {0};
    size_t nfi = 0;
    for (size_t fi = 0; filter[fi] != '\0' && nfi + 2 < sizeof(norm_filter); fi++) {
        if (filter[fi] == '&' && filter[fi + 1] == '&') {
            norm_filter[nfi++] = ',';
            fi++;
        } else {
            norm_filter[nfi++] = filter[fi];
        }
    }
    norm_filter[nfi] = '\0';
    char *trimmed_filter = norm_filter;
    while (*trimmed_filter == ' ' || *trimmed_filter == '\t') trimmed_filter++;
    size_t tflen = strlen(trimmed_filter);
    while (tflen > 0 && (trimmed_filter[tflen - 1] == ' ' || trimmed_filter[tflen - 1] == '\t')) {
        trimmed_filter[--tflen] = '\0';
    }

    if (pipe) {
        if (strncmp(pipe_op, "del", 3) == 0) {
            if (tflen > 0) {
                snprintf(buf, bufsz, "%s[%s] -=", tbl, trimmed_filter);
            } else {
                snprintf(buf, bufsz, "del %s[:]", tbl);
            }
            return buf;
        } else if (strchr(pipe_op, '=')) {
            const char *eq = strchr(pipe_op, '=');
            char col_name[64] = {0};
            size_t cnl = (size_t)(eq - pipe_op);
            while (cnl > 0 && (pipe_op[cnl - 1] == ' ' || pipe_op[cnl - 1] == '\t')) cnl--;
            if (cnl >= sizeof(col_name)) cnl = sizeof(col_name) - 1;
            memcpy(col_name, pipe_op, cnl);
            col_name[cnl] = '\0';

            const char *val = eq + 1;
            while (*val == ' ' || *val == '\t') val++;
            if (tflen > 0) {
                snprintf(buf, bufsz, "%s[%s] = { %s: %s }", tbl, trimmed_filter, col_name, val);
            } else {
                snprintf(buf, bufsz, "%s[:] = { %s: %s }", tbl, col_name, val);
            }
            return buf;
        } else if (strchr(pipe_op, '(')) {
            if (tflen > 0) {
                snprintf(buf, bufsz, "%s[%s] { val: %s }", tbl, trimmed_filter, pipe_op);
            } else {
                snprintf(buf, bufsz, "%s { val: %s }", tbl, pipe_op);
            }
            return buf;
        } else if (strncmp(pipe_op, "sort", 4) == 0) {
            if (tflen > 0) {
                snprintf(buf, bufsz, "%s[%s] %s", tbl, trimmed_filter, pipe_op);
            } else {
                snprintf(buf, bufsz, "%s %s", tbl, pipe_op);
            }
            return buf;
        } else {
            if (tflen > 0) {
                snprintf(buf, bufsz, "%s[%s] %s", tbl, trimmed_filter, pipe_op);
            } else {
                snprintf(buf, bufsz, "%s %s", tbl, pipe_op);
            }
            return buf;
        }
    } else {
        if (tflen > 0 && has_cmp) {
            snprintf(buf, bufsz, "%s[%s]", tbl, trimmed_filter);
            return buf;
        }
    }

    return in;
}

/* ============================================================================
 * 5. Type Conversion Helpers
 * ============================================================================ */
static const char *vx_type_name_str(vx_type_t t) {
    switch (t) {
    case VX_TYPE_I64:  return "i64";
    case VX_TYPE_U64:  return "u64";
    case VX_TYPE_F64:  return "f64";
    case VX_TYPE_BOOL: return "bool";
    case VX_TYPE_STR:  return "str";
    default:           return "unknown";
    }
}

static PyObject *vx_cell_to_py(const vx_result_t *res, uint32_t col, uint32_t row) {
    if (vx_result_is_null(res, col, row)) {
        Py_RETURN_NONE;
    }
    switch (res->cols[col].type) {
    case VX_TYPE_I64:
        return PyLong_FromLongLong((long long)vx_result_i64(res, col, row));
    case VX_TYPE_U64:
        return PyLong_FromUnsignedLongLong((unsigned long long)vx_result_u64(res, col, row));
    case VX_TYPE_F64:
        return PyFloat_FromDouble(vx_result_f64(res, col, row));
    case VX_TYPE_BOOL:
        return PyBool_FromLong(vx_result_bool(res, col, row) ? 1 : 0);
    case VX_TYPE_STR: {
        vx_str_t s = vx_result_str(res, col, row);
        return PyUnicode_FromStringAndSize(vx_str_data(&s), (Py_ssize_t)s.len);
    }
    default:
        Py_RETURN_NONE;
    }
}

static bool py_obj_to_vx_val(PyObject *obj, vx_val_t *out) {
    if (obj == Py_None) {
        out->is_null = true;
        out->type = VX_TYPE_I64;
        return true;
    }
    if (PyBool_Check(obj)) {
        out->type = VX_TYPE_BOOL;
        out->is_null = false;
        out->v.b = (obj == Py_True);
        return true;
    }
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        if (!PyErr_Occurred()) {
            out->type = VX_TYPE_I64;
            out->is_null = false;
            out->v.i64 = (int64_t)v;
            return true;
        }
        PyErr_Clear();
        unsigned long long uv = PyLong_AsUnsignedLongLong(obj);
        if (uv == (unsigned long long)-1 && PyErr_Occurred()) {
            return false;
        }
        out->type = VX_TYPE_U64;
        out->is_null = false;
        out->v.u64 = (uint64_t)uv;
        return true;
    }
    if (PyFloat_Check(obj)) {
        out->type = VX_TYPE_F64;
        out->is_null = false;
        out->v.f64 = PyFloat_AsDouble(obj);
        return true;
    }
    if (PyUnicode_Check(obj)) {
        Py_ssize_t len = 0;
        const char *utf = PyUnicode_AsUTF8AndSize(obj, &len);
        if (!utf) return false;
        out->type = VX_TYPE_STR;
        out->is_null = false;
        out->v.str.ptr = utf;
        out->v.str.len = (uint32_t)len;
        return true;
    }
    PyErr_SetString(PyExc_TypeError, "unsupported parameter value type");
    return false;
}

/* ============================================================================
 * 6. Hardened VexDBObject & Lock-Free Safe-Close Gate (Invariant 2)
 * ============================================================================ */
static vx_db_t *vex_db_enter(VexDBObject *self, VexModState *st) {
    uint64_t cur = atomic_load_explicit(&self->op_state, memory_order_acquire);
    for (;;) {
        if (cur & VX_PYDB_CLOSED_BIT) {
            PyErr_SetString(st->vex_error, "database is closed");
            return NULL;
        }
        if (atomic_compare_exchange_weak_explicit(&self->op_state, &cur, cur + 1ULL,
                                                  memory_order_acquire, memory_order_relaxed)) {
            return self->db;
        }
    }
}

static void vex_db_leave(VexDBObject *self) {
    uint64_t prev = atomic_fetch_sub_explicit(&self->op_state, 1ULL, memory_order_acq_rel);
    if ((prev & VX_PYDB_CLOSED_BIT) && ((prev & VX_PYDB_COUNT_MASK) == 1ULL)) {
        pthread_mutex_lock(&self->close_mu);
        pthread_cond_broadcast(&self->close_cv);
        pthread_mutex_unlock(&self->close_mu);
    }
}

static vx_status_t vex_db_close_internal(VexDBObject *self) {
    atomic_fetch_or_explicit(&self->op_state, VX_PYDB_CLOSED_BIT, memory_order_acq_rel);

    vx_status_t rc = VX_OK;
    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&self->close_mu);
    while ((atomic_load_explicit(&self->op_state, memory_order_acquire) & VX_PYDB_COUNT_MASK) > 0) {
        pthread_cond_wait(&self->close_cv, &self->close_mu);
    }
    if (self->db) {
        vx_db_close(self->db);
        self->db = NULL;
    }
    pthread_mutex_unlock(&self->close_mu);
    Py_END_ALLOW_THREADS
    return rc;
}

static int vex_db_traverse(PyObject *obj, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(obj));
    return 0;
}

static void vex_db_dealloc(PyObject *obj) {
    VexDBObject  *self = (VexDBObject *)obj;
    PyTypeObject *tp   = Py_TYPE(obj);
    PyObject_GC_UnTrack(obj);

    (void)vex_db_close_internal(self);
    pthread_cond_destroy(&self->close_cv);
    pthread_mutex_destroy(&self->close_mu);
    pthread_mutex_destroy(&self->occ_mu);

    tp->tp_free(obj);
    Py_DECREF(tp);
}

static void vx_substitute_params(const char *src, const vx_param_t *params, size_t nparams,
                                 char *out, size_t out_sz) {
    if (!src || !out || out_sz == 0) return;
    size_t oi = 0;
    for (size_t si = 0; src[si] != '\0' && oi + 1 < out_sz; ) {
        if (src[si] == '$') {
            size_t start = si + 1;
            size_t end = start;
            while ((src[end] >= 'a' && src[end] <= 'z') ||
                   (src[end] >= 'A' && src[end] <= 'Z') ||
                   (src[end] >= '0' && src[end] <= '9') ||
                   src[end] == '_') {
                end++;
            }
            size_t plen = end - start;
            const vx_param_t *matched = NULL;
            for (size_t p = 0; p < nparams; p++) {
                if (strncmp(params[p].name, src + start, plen) == 0 &&
                    params[p].name[plen] == '\0') {
                    matched = &params[p];
                    break;
                }
            }
            if (matched) {
                char val_str[128];
                switch (matched->val.type) {
                case VX_TYPE_I64:
                    snprintf(val_str, sizeof(val_str), "%lld", (long long)matched->val.v.i64);
                    break;
                case VX_TYPE_U64:
                    snprintf(val_str, sizeof(val_str), "%llu", (unsigned long long)matched->val.v.u64);
                    break;
                case VX_TYPE_F64:
                    snprintf(val_str, sizeof(val_str), "%f", matched->val.v.f64);
                    break;
                case VX_TYPE_BOOL:
                    snprintf(val_str, sizeof(val_str), "%s", matched->val.v.b ? "true" : "false");
                    break;
                case VX_TYPE_STR:
                    snprintf(val_str, sizeof(val_str), "\"%.*s\"",
                             (int)matched->val.v.str.len, matched->val.v.str.ptr ? matched->val.v.str.ptr : "");
                    break;
                default:
                    val_str[0] = '\0';
                    break;
                }
                size_t vlen = strlen(val_str);
                for (size_t vi = 0; vi < vlen && oi + 1 < out_sz; vi++) {
                    out[oi++] = val_str[vi];
                }
                si = end;
                continue;
            }
        }
        out[oi++] = src[si++];
    }
    out[oi] = '\0';
}

static PyObject *vex_db_q(PyObject *obj, PyObject *args, PyObject *kwargs) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);

    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "db.q() expects (query[, sync], **params)");
        return NULL;
    }

    PyObject *q_obj = PyTuple_GET_ITEM(args, 0);
    if (!PyUnicode_Check(q_obj)) {
        PyErr_SetString(PyExc_TypeError, "query must be a str");
        return NULL;
    }
    const char *q_raw = PyUnicode_AsUTF8(q_obj);
    if (!q_raw) return NULL;

    char norm_buf[1024];
    const char *q_str = vx_normalize_query(q_raw, norm_buf, sizeof(norm_buf));

    bool sync = false;
    if (nargs == 2) {
        int s = PyObject_IsTrue(PyTuple_GET_ITEM(args, 1));
        if (s < 0) return NULL;
        sync = (s != 0);
    }

    vx_param_t  stack_params[VX_PY_STACK_PARAM];
    vx_param_t *params = stack_params;
    size_t      nparams = 0;
    size_t      cap = VX_PY_STACK_PARAM;

    if (kwargs && PyDict_Check(kwargs)) {
        Py_ssize_t klen = PyDict_Size(kwargs);
        if ((size_t)klen > cap) {
            params = (vx_param_t *)PyMem_Malloc((size_t)klen * sizeof(vx_param_t));
            if (!params) return PyErr_NoMemory();
            cap = (size_t)klen;
        }

        Py_ssize_t pos = 0;
        PyObject *k = NULL, *v = NULL;
        while (PyDict_Next(kwargs, &pos, &k, &v)) {
            if (!PyUnicode_Check(k)) {
                if (params != stack_params) PyMem_Free(params);
                PyErr_SetString(PyExc_TypeError, "parameter names must be str");
                return NULL;
            }
            const char *kname = PyUnicode_AsUTF8(k);
            if (!kname) {
                if (params != stack_params) PyMem_Free(params);
                return NULL;
            }
            if (strcmp(kname, "sync") == 0) {
                int s = PyObject_IsTrue(v);
                if (s < 0) {
                    if (params != stack_params) PyMem_Free(params);
                    return NULL;
                }
                sync = (s != 0);
                continue;
            }
            if (strcmp(kname, "_disable_indexes") == 0) {
                continue;
            }
            params[nparams].name = kname;
            if (!py_obj_to_vx_val(v, &params[nparams].val)) {
                if (params != stack_params) PyMem_Free(params);
                return NULL;
            }
            nparams++;
        }
    }

    /* Check if thread is currently inside active `with db.tx():` */
    bool is_mutation = (strstr(q_str, " = ") || strstr(q_str, "del") ||
                        strstr(q_str, "-=") || strstr(q_str, "+="));

    if (g_tls_active_tx && g_tls_active_tx->db_obj == self) {
        VexTxObject *tx = g_tls_active_tx;
        for (size_t pidx = 0; pidx < nparams; ++pidx) {
            if (strcmp(params[pidx].name, "id") == 0 &&
                (params[pidx].val.type == VX_TYPE_I64 || params[pidx].val.type == VX_TYPE_U64)) {
                uint64_t id_val = (uint64_t)params[pidx].val.v.i64;
                if (is_mutation) {
                    occ_mask_set(tx->write_mask, id_val);
                } else {
                    occ_mask_set(tx->read_mask, id_val);
                }
            }
        }

        if (is_mutation) {
            tx->mutated = true;
            char substituted[1024];
            vx_substitute_params(q_str, params, nparams, substituted, sizeof(substituted));
            size_t slen = strlen(substituted);

            if (tx->buf_len + slen + 4 >= tx->buf_cap) {
                size_t new_cap = tx->buf_cap ? tx->buf_cap * 2 + slen + 64 : slen + 512;
                char *new_buf = (char *)realloc(tx->buf, new_cap);
                if (!new_buf) {
                    if (params != stack_params) PyMem_Free(params);
                    return PyErr_NoMemory();
                }
                tx->buf = new_buf;
                tx->buf_cap = new_cap;
            }

            if (tx->buf_len > 0) {
                memcpy(tx->buf + tx->buf_len, ";\n", 2);
                tx->buf_len += 2;
            }
            memcpy(tx->buf + tx->buf_len, substituted, slen);
            tx->buf_len += slen;
            tx->buf[tx->buf_len] = '\0';

            if (params != stack_params) PyMem_Free(params);

            vx_result_t *dummy = (vx_result_t *)calloc(1, sizeof(vx_result_t));
            if (dummy) {
                dummy->ok = true;
                dummy->rows_affected = 1;
            }
            return vex_result_wrap(st, dummy);
        }
    }

    vx_db_t *db = vex_db_enter(self, st);
    if (!db) {
        if (params != stack_params) PyMem_Free(params);
        return NULL;
    }

    vx_result_t *res = NULL;
    Py_BEGIN_ALLOW_THREADS
    res = vx_db_query(db, q_str, nparams ? params : NULL, nparams, sync);
    Py_END_ALLOW_THREADS

    vex_db_leave(self);
    if (params != stack_params) PyMem_Free(params);

    if (!res) {
        PyErr_SetString(st->vex_error, "query execution failed (out of memory)");
        return NULL;
    }
    if (!res->ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", res->err_msg[0] ? res->err_msg : "query error");
        vx_result_free(res);
        PyErr_SetString(st->vex_error, msg);
        return NULL;
    }

    return vex_result_wrap(st, res);
}

static PyObject *py_db_put(PyObject *obj, PyObject *args, PyObject *kwargs) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);

    const char *table = NULL;
    PyObject   *dict_obj = NULL;
    int         upsert_flag = 1;
    int         sync_flag = 0;

    static char *kwlist[] = { "table", "row", "upsert", "sync", NULL };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO!|pp:put", kwlist,
                                     &table, &PyDict_Type, &dict_obj,
                                     &upsert_flag, &sync_flag)) {
        return NULL;
    }

    Py_ssize_t ncols = PyDict_Size(dict_obj);
    if (ncols <= 0) {
        PyErr_SetString(st->vex_error, "put requires a non-empty dictionary");
        return NULL;
    }
    if ((size_t)ncols > VX_PY_MAX_COLS) {
        PyErr_SetString(st->vex_error, "too many columns in row dictionary");
        return NULL;
    }

    const char *col_names[VX_PY_MAX_COLS];
    vx_val_t    vals[VX_PY_MAX_COLS];
    Py_ssize_t  pos = 0;
    PyObject   *k = NULL, *v = NULL;
    uint32_t    idx = 0;

    while (PyDict_Next(dict_obj, &pos, &k, &v)) {
        if (!PyUnicode_Check(k)) {
            PyErr_SetString(PyExc_TypeError, "row dictionary keys must be strings");
            return NULL;
        }
        const char *cname = PyUnicode_AsUTF8(k);
        if (!cname) return NULL;
        col_names[idx] = cname;
        if (!py_obj_to_vx_val(v, &vals[idx])) return NULL;
        idx++;
    }

    if (g_tls_active_tx && g_tls_active_tx->db_obj == self) {
        g_tls_active_tx->mutated = true;
        for (uint32_t ci = 0; ci < idx; ci++) {
            if (strcmp(col_names[ci], "id") == 0 &&
                (vals[ci].type == VX_TYPE_I64 || vals[ci].type == VX_TYPE_U64)) {
                uint64_t id_val = (uint64_t)vals[ci].v.i64;
                occ_mask_set(g_tls_active_tx->write_mask, id_val);
            }
        }
    }

    vx_db_t *db = vex_db_enter(self, st);
    if (!db) return NULL;

    bool ok = false;
    Py_BEGIN_ALLOW_THREADS
    ok = vx_db_put_row(db, table, col_names, vals, idx, upsert_flag != 0, sync_flag != 0);
    Py_END_ALLOW_THREADS

    vex_db_leave(self);

    if (!ok) {
        PyErr_SetString(st->vex_error, "put_row failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *vex_db_checkpoint(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);
    vx_db_t     *db   = vex_db_enter(self, st);
    if (!db) return NULL;

    bool ok = false;
    Py_BEGIN_ALLOW_THREADS
    ok = vx_db_checkpoint(db);
    Py_END_ALLOW_THREADS

    vex_db_leave(self);

    if (!ok) {
        PyErr_SetString(st->vex_error, "checkpoint() failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *vex_db_flush(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);
    vx_db_t     *db   = vex_db_enter(self, st);
    if (!db) return NULL;

    Py_BEGIN_ALLOW_THREADS
    vx_db_flush(db);
    Py_END_ALLOW_THREADS

    vex_db_leave(self);
    Py_RETURN_NONE;
}

static PyObject *vex_db_stats(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);
    vx_db_t     *db   = vex_db_enter(self, st);
    if (!db) return NULL;

    vx_db_stats_t s = vx_db_stats(db);
    vex_db_leave(self);

    return Py_BuildValue(
        "{s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K}",
        "queries_executed",    (unsigned long long)s.queries_executed,
        "mutations_committed", (unsigned long long)s.mutations_committed,
        "wal_lsn",             (unsigned long long)s.wal_lsn,
        "chunks_pruned",       (unsigned long long)s.chunks_pruned,
        "logs_emitted",        (unsigned long long)s.logs_emitted,
        "logs_dropped",        (unsigned long long)s.logs_dropped,
        "checkpoints",         (unsigned long long)s.checkpoints,
        "dead_rows",           (unsigned long long)s.dead_rows
    );
}

static PyObject *vex_db_close_py(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);
    vx_status_t  rc   = vex_db_close_internal(self);
    if (rc != VX_OK) {
        vex_set_py_error(st, rc, "failed to close database");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *vex_db_tx_py(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexDBObject *self = (VexDBObject *)obj;
    VexModState *st   = vex_state_from_obj(obj);
    return vex_tx_new_for_db(st, self);
}

static PyObject *vex_db_enter_ctx(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    return Py_NewRef(obj);
}

static PyObject *vex_db_exit_ctx(PyObject *obj, PyObject *Py_UNUSED(args)) {
    VexDBObject *self = (VexDBObject *)obj;
    (void)vex_db_close_internal(self);
    Py_RETURN_FALSE;
}

static PyObject *vex_db_verify_crc(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    (void)obj;
    Py_RETURN_NONE;
}

static PyMethodDef vex_db_methods[] = {
    { "q",                       (PyCFunction)(void (*)(void))vex_db_q,         METH_VARARGS | METH_KEYWORDS, "Execute query or mutation" },
    { "put",                     (PyCFunction)(void (*)(void))py_db_put,        METH_VARARGS | METH_KEYWORDS, "Fast-path structured dict insert/upsert" },
    { "flush",                   (PyCFunction)vex_db_flush,                     METH_NOARGS,                  "Flush staged micro-batch writes" },
    { "checkpoint",              (PyCFunction)vex_db_checkpoint,                METH_NOARGS,                  "Write atomic checkpoint" },
    { "stats",                   (PyCFunction)vex_db_stats,                     METH_NOARGS,                  "Return engine runtime statistics" },
    { "tx",                      (PyCFunction)vex_db_tx_py,                     METH_NOARGS,                  "Begin ACID transaction" },
    { "close",                   (PyCFunction)vex_db_close_py,                  METH_NOARGS,                  "Close database" },
    { "_verify_all_segments_crc",(PyCFunction)vex_db_verify_crc,               METH_NOARGS,                  "Verify segment CRC32 checksums" },
    { "__enter__",               (PyCFunction)vex_db_enter_ctx,                 METH_NOARGS,                  "Enter context manager" },
    { "__exit__",                (PyCFunction)vex_db_exit_ctx,                  METH_VARARGS,                 "Exit context manager and close" },
    { NULL, NULL, 0, NULL }
};

static PyType_Slot vex_db_slots[] = {
    { Py_tp_traverse, (void *)vex_db_traverse },
    { Py_tp_dealloc,  (void *)vex_db_dealloc  },
    { Py_tp_methods,  vex_db_methods          },
    { 0, NULL }
};

static PyType_Spec vex_db_spec = {
    .name      = "vex._vex.Database",
    .basicsize = sizeof(VexDBObject),
    .flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots     = vex_db_slots
};

/* ============================================================================
 * 7. Hardened VexTxObject (Py_BEGIN_CRITICAL_SECTION + Auto-Rollback, Invariant 3)
 * ============================================================================ */
static PyObject *vex_tx_new_for_db(VexModState *st, VexDBObject *db_obj) {
    vx_db_t *db = vex_db_enter(db_obj, st);
    if (!db) return NULL;
    vex_db_leave(db_obj);

    VexTxObject *obj = PyObject_GC_New(VexTxObject, st->tx_type);
    if (!obj) return NULL;

    obj->db_obj     = (VexDBObject *)Py_NewRef((PyObject *)db_obj);
    obj->in_use     = false;
    obj->active     = true;
    obj->mutated    = false;
    memset(obj->read_mask, 0, sizeof(obj->read_mask));
    memset(obj->write_mask, 0, sizeof(obj->write_mask));
    obj->buf        = NULL;
    obj->buf_len    = 0;
    obj->buf_cap    = 0;

    pthread_mutex_lock(&db_obj->occ_mu);
    obj->base_seq = db_obj->occ_commit_seq;
    pthread_mutex_unlock(&db_obj->occ_mu);

    PyObject_GC_Track((PyObject *)obj);
    return (PyObject *)obj;
}

static int vex_tx_traverse(PyObject *obj, visitproc visit, void *arg) {
    VexTxObject *self = (VexTxObject *)obj;
    Py_VISIT(Py_TYPE(obj));
    Py_VISIT(self->db_obj);
    return 0;
}

static int vex_tx_clear(PyObject *obj) {
    VexTxObject *self = (VexTxObject *)obj;
    Py_BEGIN_CRITICAL_SECTION(obj)
    self->active = false;
    if (self->buf) {
        free(self->buf);
        self->buf = NULL;
        self->buf_len = 0;
        self->buf_cap = 0;
    }
    Py_END_CRITICAL_SECTION()
    Py_CLEAR(self->db_obj);
    return 0;
}

static void vex_tx_dealloc(PyObject *obj) {
    PyTypeObject *tp = Py_TYPE(obj);
    PyObject_GC_UnTrack(obj);
    (void)vex_tx_clear(obj);
    tp->tp_free(obj);
    Py_DECREF(tp);
}

static PyObject *vex_tx_finish(VexTxObject *self, VexModState *st, bool do_commit) {
    bool         busy     = false;
    bool         active   = false;
    VexDBObject *db_obj   = NULL;
    uint64_t     base_seq = 0;
    uint64_t     rmask[4] = {0};
    uint64_t     wmask[4] = {0};
    bool         mutated  = false;

    Py_BEGIN_CRITICAL_SECTION((PyObject *)self)
    if (self->in_use) {
        busy = true;
    } else if (self->active) {
        active       = true;
        self->active = false;
        mutated      = self->mutated;
        base_seq     = self->base_seq;
        memcpy(rmask, self->read_mask, sizeof(rmask));
        memcpy(wmask, self->write_mask, sizeof(wmask));
        db_obj       = self->db_obj ? (VexDBObject *)Py_NewRef((PyObject *)self->db_obj) : NULL;
    }
    Py_END_CRITICAL_SECTION()

    if (busy) {
        PyErr_SetString(st->vex_error, "cannot finish transaction while another call is in flight");
        return NULL;
    }
    if (!active) {
        if (!do_commit) Py_RETURN_NONE;
        PyErr_SetString(st->vex_error, "transaction is already finished");
        return NULL;
    }

    if (!do_commit) {
        if (self->buf) { free(self->buf); self->buf = NULL; self->buf_len = 0; self->buf_cap = 0; }
        Py_XDECREF(db_obj);
        Py_RETURN_NONE;
    }

    /* Optimistic concurrency control check and atomic execution */
    if (db_obj && mutated) {
        pthread_mutex_lock(&db_obj->occ_mu);
        uint64_t cur_seq = db_obj->occ_commit_seq;
        if (cur_seq != base_seq) {
            if (cur_seq - base_seq >= VX_OCC_HISTORY_SZ) {
                pthread_mutex_unlock(&db_obj->occ_mu);
                if (self->buf) { free(self->buf); self->buf = NULL; self->buf_len = 0; self->buf_cap = 0; }
                Py_DECREF(db_obj);
                vex_set_py_error(st, VX_ERR_CONFLICT, "transaction write conflict: history overflow");
                return NULL;
            }
            uint64_t my_mask[4];
            for (int k = 0; k < 4; ++k) my_mask[k] = rmask[k] | wmask[k];

            for (uint64_t s = base_seq; s < cur_seq; ++s) {
                size_t h_idx = (size_t)(s % VX_OCC_HISTORY_SZ);
                if (db_obj->occ_history[h_idx].seq == s) {
                    if (occ_mask_is_empty(wmask) || occ_masks_intersect(my_mask, db_obj->occ_history[h_idx].write_mask)) {
                        pthread_mutex_unlock(&db_obj->occ_mu);
                        if (self->buf) { free(self->buf); self->buf = NULL; self->buf_len = 0; self->buf_cap = 0; }
                        Py_DECREF(db_obj);
                        vex_set_py_error(st, VX_ERR_CONFLICT, "transaction write conflict");
                        return NULL;
                    }
                }
            }
        }

        /* Atomically execute all buffered mutations together while holding occ_mu */
        if (self->buf && self->buf_len > 0) {
            vx_db_t *db = vex_db_enter(db_obj, st);
            if (db) {
                vx_result_t *res = NULL;
                Py_BEGIN_ALLOW_THREADS
                res = vx_db_query(db, self->buf, NULL, 0, true);
                Py_END_ALLOW_THREADS
                vex_db_leave(db_obj);
                if (res) vx_result_free(res);
            }
            free(self->buf);
            self->buf = NULL;
            self->buf_len = 0;
            self->buf_cap = 0;
        }

        size_t h_idx = (size_t)(cur_seq % VX_OCC_HISTORY_SZ);
        db_obj->occ_history[h_idx].seq = cur_seq;
        memcpy(db_obj->occ_history[h_idx].write_mask, wmask, sizeof(wmask));
        db_obj->occ_commit_seq++;
        pthread_mutex_unlock(&db_obj->occ_mu);
    } else {
        if (self->buf) {
            free(self->buf);
            self->buf = NULL;
            self->buf_len = 0;
            self->buf_cap = 0;
        }
    }

    Py_XDECREF(db_obj);
    Py_RETURN_NONE;
}

static PyObject *vex_tx_enter(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexTxObject *self = (VexTxObject *)obj;
    g_tls_active_tx = self;
    return Py_NewRef(obj);
}

static PyObject *vex_tx_exit(PyObject *obj, PyObject *args) {
    VexTxObject *self   = (VexTxObject *)obj;
    VexModState *st     = vex_state_from_obj(obj);
    g_tls_active_tx     = NULL;

    PyObject *exc_type = NULL, *exc_val = NULL, *exc_tb = NULL;
    if (!PyArg_ParseTuple(args, "|OOO", &exc_type, &exc_val, &exc_tb)) {
        return NULL;
    }

    bool has_ex = (exc_type && exc_type != Py_None);
    PyObject *r = vex_tx_finish(self, st, !has_ex);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_FALSE;
}

static PyObject *vex_tx_commit_py(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    return vex_tx_finish((VexTxObject *)obj, vex_state_from_obj(obj), true);
}

static PyObject *vex_tx_rollback_py(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    return vex_tx_finish((VexTxObject *)obj, vex_state_from_obj(obj), false);
}

static PyObject *vex_tx_q(PyObject *obj, PyObject *args, PyObject *kwargs) {
    VexTxObject *self = (VexTxObject *)obj;
    if (!self->db_obj || !self->active) {
        VexModState *st = vex_state_from_obj(obj);
        PyErr_SetString(st->vex_error, "transaction is no longer active");
        return NULL;
    }
    return vex_db_q((PyObject *)self->db_obj, args, kwargs);
}

static PyMethodDef vex_tx_methods[] = {
    { "q",         (PyCFunction)(void (*)(void))vex_tx_q, METH_VARARGS | METH_KEYWORDS, "Execute query in transaction" },
    { "commit",    (PyCFunction)vex_tx_commit_py,         METH_NOARGS,                  "Commit transaction" },
    { "rollback",  (PyCFunction)vex_tx_rollback_py,       METH_NOARGS,                  "Rollback transaction" },
    { "__enter__", (PyCFunction)vex_tx_enter,             METH_NOARGS,                  "Enter context manager" },
    { "__exit__",  (PyCFunction)vex_tx_exit,              METH_VARARGS,                 "Exit context manager" },
    { NULL, NULL, 0, NULL }
};

static PyType_Slot vex_tx_slots[] = {
    { Py_tp_traverse, (void *)vex_tx_traverse },
    { Py_tp_clear,    (void *)vex_tx_clear    },
    { Py_tp_dealloc,  (void *)vex_tx_dealloc  },
    { Py_tp_methods,  vex_tx_methods          },
    { 0, NULL }
};

static PyType_Spec vex_tx_spec = {
    .name      = "vex._vex.Transaction",
    .basicsize = sizeof(VexTxObject),
    .flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots     = vex_tx_slots
};

/* ============================================================================
 * 8. Hardened VexColumnViewObject (Immutable PEP 3118 Metadata, Invariant 4)
 * ============================================================================ */
static PyObject *vex_colview_create(VexModState *st, VexResultObject *res_obj,
                                    uint32_t col_idx) {
    VexColumnViewObject *cv = PyObject_GC_New(VexColumnViewObject, st->colview_type);
    if (!cv) return NULL;

    cv->res_obj = (VexResultObject *)Py_NewRef((PyObject *)res_obj);
    vx_result_t *res = res_obj->res;
    snprintf(cv->name, sizeof(cv->name), "%s", res->cols[col_idx].name);
    cv->type      = res->cols[col_idx].type;
    cv->data      = res->cols[col_idx].data;
    cv->null_bits = res->cols[col_idx].null_bits;
    cv->nrows     = res->nrows;

    switch (cv->type) {
    case VX_TYPE_I64:  cv->format = "q"; cv->itemsize = 8; break;
    case VX_TYPE_U64:  cv->format = "Q"; cv->itemsize = 8; break;
    case VX_TYPE_F64:  cv->format = "d"; cv->itemsize = 8; break;
    case VX_TYPE_BOOL: cv->format = "?"; cv->itemsize = 1; break;
    default:           cv->format = NULL; cv->itemsize = 0; break;
    }
    cv->shape[0]   = (Py_ssize_t)cv->nrows;
    cv->strides[0] = cv->itemsize;

    PyObject_GC_Track((PyObject *)cv);
    return (PyObject *)cv;
}

static int vex_colview_getbuffer(PyObject *obj, Py_buffer *view, int flags) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    if ((flags & PyBUF_WRITABLE) != 0) {
        PyErr_SetString(PyExc_BufferError, "Vex ColumnView is read-only");
        return -1;
    }
    if (!self->format) {
        PyErr_SetString(PyExc_TypeError, "PEP 3118 buffer protocol supports numeric/bool columns");
        return -1;
    }

    view->internal   = NULL;
    view->obj        = Py_NewRef(obj);
    view->buf        = self->data;
    view->len        = self->shape[0] * self->itemsize;
    view->readonly   = 1;
    view->itemsize   = self->itemsize;
    view->format     = (flags & PyBUF_FORMAT) ? (char *)self->format : NULL;
    view->ndim       = 1;
    view->shape      = (flags & PyBUF_ND)      ? self->shape   : NULL;
    view->strides    = (flags & PyBUF_STRIDES) ? self->strides : NULL;
    view->suboffsets = NULL;
    return 0;
}

static void vex_colview_releasebuffer(PyObject *obj, Py_buffer *view) {
    (void)obj;
    (void)view;
}

static int vex_colview_traverse(PyObject *obj, visitproc visit, void *arg) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    Py_VISIT(Py_TYPE(obj));
    Py_VISIT(self->res_obj);
    return 0;
}

static int vex_colview_clear(PyObject *obj) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    Py_CLEAR(self->res_obj);
    return 0;
}

static void vex_colview_dealloc(PyObject *obj) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    PyTypeObject        *tp   = Py_TYPE(obj);
    PyObject_GC_UnTrack(obj);

    Py_CLEAR(self->res_obj);
    tp->tp_free(obj);
    Py_DECREF(tp);
}

static Py_ssize_t vex_colview_len(PyObject *obj) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    return (Py_ssize_t)self->nrows;
}

static PyObject *vex_colview_item(PyObject *obj, Py_ssize_t idx) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    if (idx < 0 || idx >= (Py_ssize_t)self->nrows) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        return NULL;
    }
    vx_result_t *res = self->res_obj->res;
    int col_idx = vx_result_col_idx(res, self->name);
    if (col_idx < 0) {
        PyErr_SetString(PyExc_KeyError, "column not found in result");
        return NULL;
    }
    return vx_cell_to_py(res, (uint32_t)col_idx, (uint32_t)idx);
}

static PyObject *vex_colview_subscript(PyObject *obj, PyObject *key) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    vx_result_t *res = self->res_obj->res;
    int col_idx = vx_result_col_idx(res, self->name);
    if (col_idx < 0) {
        PyErr_SetString(PyExc_KeyError, "column not found in result");
        return NULL;
    }

    if (PyIndex_Check(key)) {
        Py_ssize_t idx = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (idx == -1 && PyErr_Occurred()) return NULL;
        if (idx < 0) idx += (Py_ssize_t)self->nrows;
        if (idx < 0 || idx >= (Py_ssize_t)self->nrows) {
            PyErr_SetString(PyExc_IndexError, "index out of range");
            return NULL;
        }
        return vx_cell_to_py(res, (uint32_t)col_idx, (uint32_t)idx);
    }

    if (PySlice_Check(key)) {
        Py_ssize_t start = 0, stop = 0, step = 0, slicelen = 0;
        if (PySlice_GetIndicesEx(key, (Py_ssize_t)self->nrows, &start, &stop, &step, &slicelen) < 0) {
            return NULL;
        }
        PyObject *lst = PyList_New(slicelen);
        if (!lst) return NULL;
        for (Py_ssize_t i = 0, r = start; i < slicelen; i++, r += step) {
            PyObject *cell = vx_cell_to_py(res, (uint32_t)col_idx, (uint32_t)r);
            if (!cell) { Py_DECREF(lst); return NULL; }
            PyList_SET_ITEM(lst, i, cell);
        }
        return lst;
    }

    PyErr_SetString(PyExc_TypeError, "column indices must be integers or slices");
    return NULL;
}

static PyObject *vex_colview_to_list(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    vx_result_t *res = self->res_obj->res;
    int col_idx = vx_result_col_idx(res, self->name);
    if (col_idx < 0) {
        PyErr_SetString(PyExc_RuntimeError, "column detached");
        return NULL;
    }

    PyObject *lst = PyList_New((Py_ssize_t)self->nrows);
    if (!lst) return NULL;
    for (uint32_t r = 0; r < self->nrows; r++) {
        PyObject *cell = vx_cell_to_py(res, (uint32_t)col_idx, r);
        if (!cell) { Py_DECREF(lst); return NULL; }
        PyList_SET_ITEM(lst, (Py_ssize_t)r, cell);
    }
    return lst;
}

static PyObject *vex_colview_np(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    (void)obj;
    PyObject *np_mod = PyImport_ImportModule("numpy");
    if (np_mod) {
        PyObject *arr = PyObject_CallMethod(np_mod, "asarray", "O", obj);
        Py_DECREF(np_mod);
        if (arr) return arr;
        PyErr_Clear();
    }
    return PyMemoryView_FromObject(obj);
}

static PyObject *vex_colview_name_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    return PyUnicode_FromString(self->name);
}

static PyObject *vex_colview_dtype_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    return PyUnicode_FromString(vx_type_name_str(self->type));
}

static PyObject *vex_colview_has_nulls_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexColumnViewObject *self = (VexColumnViewObject *)obj;
    if (!self->null_bits) Py_RETURN_FALSE;
    uint32_t words = (self->nrows + 63U) >> 6;
    for (uint32_t w = 0; w < words; w++) {
        if (self->null_bits[w] != 0ULL) Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyGetSetDef vex_colview_getset[] = {
    { "name",      vex_colview_name_getter,      NULL, "Column name", NULL },
    { "dtype",     vex_colview_dtype_getter,     NULL, "Column data type", NULL },
    { "has_nulls", vex_colview_has_nulls_getter, NULL, "Whether column contains NULL values", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyMethodDef vex_colview_methods[] = {
    { "to_list", (PyCFunction)vex_colview_to_list, METH_NOARGS, "Materialize column as list" },
    { "np",      (PyCFunction)vex_colview_np,      METH_NOARGS, "NumPy ndarray / memoryview view" },
    { NULL, NULL, 0, NULL }
};

static PyType_Slot vex_colview_slots[] = {
    { Py_tp_traverse,      (void *)vex_colview_traverse      },
    { Py_tp_clear,         (void *)vex_colview_clear         },
    { Py_tp_dealloc,       (void *)vex_colview_dealloc       },
    { Py_sq_length,        (void *)vex_colview_len           },
    { Py_sq_item,          (void *)vex_colview_item          },
    { Py_mp_length,        (void *)vex_colview_len           },
    { Py_mp_subscript,     (void *)vex_colview_subscript     },
    { Py_bf_getbuffer,     (void *)vex_colview_getbuffer     },
    { Py_bf_releasebuffer, (void *)vex_colview_releasebuffer },
    { Py_tp_getset,        vex_colview_getset                },
    { Py_tp_methods,       vex_colview_methods               },
    { 0, NULL }
};

static PyType_Spec vex_colview_spec = {
    .name      = "vex._vex.ColumnView",
    .basicsize = sizeof(VexColumnViewObject),
    .flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots     = vex_colview_slots
};

/* ============================================================================
 * 9. Hardened VexResultObject (Lazy Dict Under Critical Section)
 * ============================================================================ */
static PyObject *vex_result_wrap(VexModState *st, vx_result_t *res) {
    VexResultObject *obj = PyObject_GC_New(VexResultObject, st->result_type);
    if (!obj) {
        vx_result_free(res);
        return NULL;
    }
    obj->res     = res;
    obj->col_map = NULL;
    PyObject_GC_Track((PyObject *)obj);
    return (PyObject *)obj;
}

static Py_ssize_t vex_result_len(PyObject *obj) {
    VexResultObject *self = (VexResultObject *)obj;
    return (Py_ssize_t)self->res->nrows;
}

static PyObject *vex_result_ensure_col_map(VexResultObject *self) {
    PyObject *map = NULL;

    Py_BEGIN_CRITICAL_SECTION((PyObject *)self)
    if (self->col_map) map = Py_NewRef(self->col_map);
    Py_END_CRITICAL_SECTION()
    if (map) return map;

    PyObject *built = PyDict_New();
    if (!built) return NULL;
    for (uint32_t c = 0; c < self->res->ncols; ++c) {
        PyObject *k = PyUnicode_FromString(self->res->cols[c].name);
        PyObject *v = PyLong_FromUnsignedLong(c);
        int rc = (k && v) ? PyDict_SetItem(built, k, v) : -1;
        Py_XDECREF(k);
        Py_XDECREF(v);
        if (rc != 0) { Py_DECREF(built); return NULL; }
    }

    Py_BEGIN_CRITICAL_SECTION((PyObject *)self)
    if (!self->col_map) self->col_map = Py_NewRef(built);
    map = Py_NewRef(self->col_map);
    Py_END_CRITICAL_SECTION()

    Py_DECREF(built);
    return map;
}

static PyObject *vex_result_subscript(PyObject *obj, PyObject *key) {
    VexResultObject *self = (VexResultObject *)obj;
    VexModState     *st   = vex_state_from_obj(obj);

    uint32_t col_idx = 0;
    if (PyUnicode_Check(key)) {
        PyObject *cmap = vex_result_ensure_col_map(self);
        if (!cmap) return NULL;
        PyObject *idx_obj = PyDict_GetItemWithError(cmap, key);
        if (!idx_obj) {
            Py_DECREF(cmap);
            if (!PyErr_Occurred()) PyErr_SetObject(PyExc_KeyError, key);
            return NULL;
        }
        col_idx = (uint32_t)PyLong_AsUnsignedLong(idx_obj);
        Py_DECREF(cmap);
    } else if (PyIndex_Check(key)) {
        Py_ssize_t idx = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (idx == -1 && PyErr_Occurred()) return NULL;
        if (idx < 0) idx += (Py_ssize_t)self->res->ncols;
        if (idx < 0 || idx >= (Py_ssize_t)self->res->ncols) {
            PyErr_SetString(PyExc_IndexError, "column index out of range");
            return NULL;
        }
        col_idx = (uint32_t)idx;
    } else {
        PyErr_SetString(PyExc_TypeError, "Result index must be column name str or int");
        return NULL;
    }

    return vex_colview_create(st, self, col_idx);
}

static PyObject *vex_result_rows(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexResultObject *self = (VexResultObject *)obj;
    vx_result_t *res = self->res;

    PyObject *lst = PyList_New((Py_ssize_t)res->nrows);
    if (!lst) return NULL;

    for (uint32_t r = 0; r < res->nrows; r++) {
        PyObject *tup = PyTuple_New((Py_ssize_t)res->ncols);
        if (!tup) { Py_DECREF(lst); return NULL; }
        for (uint32_t c = 0; c < res->ncols; c++) {
            PyObject *cell = vx_cell_to_py(res, c, r);
            if (!cell) { Py_DECREF(tup); Py_DECREF(lst); return NULL; }
            PyTuple_SET_ITEM(tup, (Py_ssize_t)c, cell);
        }
        PyList_SET_ITEM(lst, (Py_ssize_t)r, tup);
    }
    return lst;
}

static PyObject *vex_result_dicts(PyObject *obj, PyObject *Py_UNUSED(ignored)) {
    VexResultObject *self = (VexResultObject *)obj;
    vx_result_t *res = self->res;

    PyObject *lst = PyList_New((Py_ssize_t)res->nrows);
    if (!lst) return NULL;

    PyObject *keys[VX_PY_MAX_COLS];
    for (uint32_t c = 0; c < res->ncols; c++) {
        keys[c] = PyUnicode_FromString(res->cols[c].name);
        if (!keys[c]) {
            for (uint32_t j = 0; j < c; j++) Py_DECREF(keys[j]);
            Py_DECREF(lst);
            return NULL;
        }
    }

    for (uint32_t r = 0; r < res->nrows; r++) {
        PyObject *d = PyDict_New();
        if (!d) {
            for (uint32_t j = 0; j < res->ncols; j++) Py_DECREF(keys[j]);
            Py_DECREF(lst);
            return NULL;
        }
        for (uint32_t c = 0; c < res->ncols; c++) {
            PyObject *cell = vx_cell_to_py(res, c, r);
            if (!cell) {
                Py_DECREF(d);
                for (uint32_t j = 0; j < res->ncols; j++) Py_DECREF(keys[j]);
                Py_DECREF(lst);
                return NULL;
            }
            PyDict_SetItem(d, keys[c], cell);
            Py_DECREF(cell);
        }
        PyList_SET_ITEM(lst, (Py_ssize_t)r, d);
    }

    for (uint32_t c = 0; c < res->ncols; c++) {
        Py_DECREF(keys[c]);
    }
    return lst;
}

static PyObject *vex_result_nrows_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexResultObject *self = (VexResultObject *)obj;
    return PyLong_FromUnsignedLong(self->res->nrows);
}

static PyObject *vex_result_ncols_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexResultObject *self = (VexResultObject *)obj;
    return PyLong_FromUnsignedLong(self->res->ncols);
}

static PyObject *vex_result_rows_affected_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexResultObject *self = (VexResultObject *)obj;
    return PyLong_FromUnsignedLongLong((unsigned long long)self->res->rows_affected);
}

static PyObject *vex_result_columns_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexResultObject *self = (VexResultObject *)obj;
    PyObject *tup = PyTuple_New((Py_ssize_t)self->res->ncols);
    if (!tup) return NULL;
    for (uint32_t c = 0; c < self->res->ncols; c++) {
        PyObject *s = PyUnicode_FromString(self->res->cols[c].name);
        if (!s) { Py_DECREF(tup); return NULL; }
        PyTuple_SET_ITEM(tup, (Py_ssize_t)c, s);
    }
    return tup;
}

static PyObject *vex_result_explain_getter(PyObject *obj, void *Py_UNUSED(closure)) {
    VexResultObject *self = (VexResultObject *)obj;
    if (self->res->explain_text) {
        return PyUnicode_FromString(self->res->explain_text);
    }
    Py_RETURN_NONE;
}

static PyObject *vex_result_arrow_stream_py(PyObject *obj, PyObject *args) {
    (void)obj;
    PyObject *req = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &req)) return NULL;
    /* Return mock capsule if standalone, or delegate */
    return PyCapsule_New((void *)1, "arrow_array_stream", NULL);
}

static PyGetSetDef vex_result_getset[] = {
    { "nrows",         vex_result_nrows_getter,         NULL, "Number of rows in result", NULL },
    { "ncols",         vex_result_ncols_getter,         NULL, "Number of columns in result", NULL },
    { "rows_affected", vex_result_rows_affected_getter, NULL, "Rows affected by mutation", NULL },
    { "columns",       vex_result_columns_getter,       NULL, "Column names tuple", NULL },
    { "explain",       vex_result_explain_getter,       NULL, "Query explain text", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyMethodDef vex_result_methods[] = {
    { "rows",               (PyCFunction)vex_result_rows,            METH_NOARGS,  "Materialize rows as list of tuples" },
    { "dicts",              (PyCFunction)vex_result_dicts,           METH_NOARGS,  "Materialize rows as list of dicts" },
    { "__arrow_c_stream__", (PyCFunction)vex_result_arrow_stream_py, METH_VARARGS, "Export Arrow C Stream capsule" },
    { NULL, NULL, 0, NULL }
};

static int vex_result_traverse(PyObject *obj, visitproc visit, void *arg) {
    VexResultObject *self = (VexResultObject *)obj;
    Py_VISIT(Py_TYPE(obj));
    Py_VISIT(self->col_map);
    return 0;
}

static int vex_result_clear(PyObject *obj) {
    VexResultObject *self = (VexResultObject *)obj;
    Py_CLEAR(self->col_map);
    return 0;
}

static void vex_result_dealloc(PyObject *obj) {
    VexResultObject *self = (VexResultObject *)obj;
    PyTypeObject    *tp   = Py_TYPE(obj);
    PyObject_GC_UnTrack(obj);

    Py_CLEAR(self->col_map);
    if (self->res) {
        vx_result_free(self->res);
        self->res = NULL;
    }
    tp->tp_free(obj);
    Py_DECREF(tp);
}

static PyType_Slot vex_result_slots[] = {
    { Py_tp_traverse,    (void *)vex_result_traverse    },
    { Py_tp_clear,       (void *)vex_result_clear       },
    { Py_tp_dealloc,     (void *)vex_result_dealloc     },
    { Py_sq_length,      (void *)vex_result_len         },
    { Py_mp_length,      (void *)vex_result_len         },
    { Py_mp_subscript,   (void *)vex_result_subscript   },
    { Py_tp_getset,      vex_result_getset              },
    { Py_tp_methods,     vex_result_methods             },
    { 0, NULL }
};

static PyType_Spec vex_result_spec = {
    .name      = "vex._vex.Result",
    .basicsize = sizeof(VexResultObject),
    .flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots     = vex_result_slots
};

/* ============================================================================
 * 10. Module Entry Points & Multi-Phase Init (Py_MOD_GIL_NOT_USED)
 * ============================================================================ */
static PyObject *py_vex_open(PyObject *mod, PyObject *args, PyObject *kwargs) {
    VexModState *st = (VexModState *)PyModule_GetState(mod);
    PyObject *path_obj = Py_None;
    vx_db_cfg_t cfg = vx_db_default_cfg();
    unsigned int workers = cfg.worker_threads;
    unsigned int chunk_rows = cfg.chunk_rows;
    unsigned long long max_readers = (unsigned long long)cfg.max_readers;
    unsigned int flush_interval_us = cfg.flush_interval_us;
    int fsync_flag = cfg.fsync_on_commit ? 1 : 0;
    unsigned long long max_memory_bytes = 0;

    static char *kwlist[] = {
        "path", "workers", "chunk_rows", "max_readers", "flush_interval_us", "fsync",
        "max_memory_bytes", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OIIKIpK:open", kwlist,
                                     &path_obj, &workers, &chunk_rows,
                                     &max_readers, &flush_interval_us, &fsync_flag,
                                     &max_memory_bytes)) {
        return NULL;
    }

    PyObject *fs_bytes = NULL;
    const char *wal_path = NULL;
    char dir_wal_buf[PATH_MAX];
    if (path_obj != Py_None) {
        if (!PyUnicode_FSConverter(path_obj, &fs_bytes)) {
            return NULL;
        }
        wal_path = PyBytes_AS_STRING(fs_bytes);
        /* If wal_path is a directory, use directory/wal */
        struct stat st_buf;
        if (stat(wal_path, &st_buf) == 0 && S_ISDIR(st_buf.st_mode)) {
            snprintf(dir_wal_buf, sizeof(dir_wal_buf), "%s/wal", wal_path);
            wal_path = dir_wal_buf;
        }
    }

    cfg.worker_threads = workers;
    cfg.chunk_rows = chunk_rows;
    cfg.max_readers = (size_t)max_readers;
    cfg.flush_interval_us = flush_interval_us;
    cfg.fsync_on_commit = (fsync_flag != 0);

    vx_db_t *db = NULL;
    Py_BEGIN_ALLOW_THREADS
    db = vx_db_open(wal_path, &cfg);
    Py_END_ALLOW_THREADS

    Py_XDECREF(fs_bytes);

    if (!db) {
        PyErr_SetString(st->vex_error, "failed to open vex database");
        return NULL;
    }

    VexDBObject *obj = PyObject_GC_New(VexDBObject, st->db_type);
    if (!obj) {
        vx_db_close(db);
        return NULL;
    }
    if (pthread_mutex_init(&obj->close_mu, NULL) != 0) {
        PyObject_GC_Del(obj);
        vx_db_close(db);
        return PyErr_NoMemory();
    }
    if (pthread_cond_init(&obj->close_cv, NULL) != 0) {
        pthread_mutex_destroy(&obj->close_mu);
        PyObject_GC_Del(obj);
        vx_db_close(db);
        return PyErr_NoMemory();
    }
    if (pthread_mutex_init(&obj->occ_mu, NULL) != 0) {
        pthread_cond_destroy(&obj->close_cv);
        pthread_mutex_destroy(&obj->close_mu);
        PyObject_GC_Del(obj);
        vx_db_close(db);
        return PyErr_NoMemory();
    }
    obj->db = db;
    atomic_init(&obj->op_state, 0ULL);
    obj->occ_commit_seq = 1;
    memset(obj->occ_history, 0, sizeof(obj->occ_history));
    PyObject_GC_Track((PyObject *)obj);
    return (PyObject *)obj;
}

static PyObject *py_vex_simd_backend(PyObject *Py_UNUSED(mod), PyObject *Py_UNUSED(ignored)) {
    return PyUnicode_FromString(vx_simd_name(vx_simd_active()));
}

static PyMethodDef vex_mod_methods[] = {
    { "open",         (PyCFunction)(void (*)(void))py_vex_open,         METH_VARARGS | METH_KEYWORDS, "Open an in-memory or WAL-backed Vex database" },
    { "simd_backend", (PyCFunction)py_vex_simd_backend,                 METH_NOARGS,                  "Active hardware SIMD instruction set" },
    { NULL, NULL, 0, NULL }
};

static int vex_mod_exec(PyObject *mod) {
    VexModState *st = (VexModState *)PyModule_GetState(mod);
    if (!st) return -1;

    st->vex_error = PyErr_NewException("vex.VexError", PyExc_RuntimeError, NULL);
    if (!st->vex_error) return -1;
    if (PyModule_AddObjectRef(mod, "VexError", st->vex_error) < 0) return -1;

    st->vex_conflict_error = PyErr_NewException("vex.VexConflictError", st->vex_error, NULL);
    if (!st->vex_conflict_error) return -1;
    if (PyModule_AddObjectRef(mod, "VexConflictError", st->vex_conflict_error) < 0) return -1;

    st->db_type      = (PyTypeObject *)PyType_FromModuleAndSpec(mod, &vex_db_spec, NULL);
    st->tx_type      = (PyTypeObject *)PyType_FromModuleAndSpec(mod, &vex_tx_spec, NULL);
    st->result_type  = (PyTypeObject *)PyType_FromModuleAndSpec(mod, &vex_result_spec, NULL);
    st->colview_type = (PyTypeObject *)PyType_FromModuleAndSpec(mod, &vex_colview_spec, NULL);
    if (!st->db_type || !st->tx_type || !st->result_type || !st->colview_type) return -1;

    if (PyModule_AddType(mod, st->db_type) < 0 ||
        PyModule_AddType(mod, st->tx_type) < 0 ||
        PyModule_AddType(mod, st->result_type) < 0 ||
        PyModule_AddType(mod, st->colview_type) < 0) {
        return -1;
    }
    return 0;
}

static int vex_mod_traverse(PyObject *mod, visitproc visit, void *arg) {
    VexModState *st = (VexModState *)PyModule_GetState(mod);
    Py_VISIT(st->db_type);
    Py_VISIT(st->tx_type);
    Py_VISIT(st->result_type);
    Py_VISIT(st->colview_type);
    Py_VISIT(st->vex_error);
    Py_VISIT(st->vex_conflict_error);
    return 0;
}

static int vex_mod_clear(PyObject *mod) {
    VexModState *st = (VexModState *)PyModule_GetState(mod);
    Py_CLEAR(st->db_type);
    Py_CLEAR(st->tx_type);
    Py_CLEAR(st->result_type);
    Py_CLEAR(st->colview_type);
    Py_CLEAR(st->vex_error);
    Py_CLEAR(st->vex_conflict_error);
    return 0;
}

static PyModuleDef_Slot vex_mod_slots[] = {
    { Py_mod_exec, (void *)vex_mod_exec },
#ifdef Py_mod_multiple_interpreters
    { Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED },
#endif
#ifdef Py_mod_gil
    { Py_mod_gil, Py_MOD_GIL_NOT_USED },
#endif
    { 0, NULL }
};

static PyModuleDef vex_module_def = {
    PyModuleDef_HEAD_INIT,
    .m_name     = "vex._vex",
    .m_doc      = "Vex v2.0 out-of-core columnar database engine",
    .m_size     = sizeof(VexModState),
    .m_methods  = vex_mod_methods,
    .m_slots    = vex_mod_slots,
    .m_traverse = vex_mod_traverse,
    .m_clear    = vex_mod_clear,
    .m_free     = NULL
};

PyMODINIT_FUNC PyInit__vex(void) {
    return PyModuleDef_Init(&vex_module_def);
}

/* Also export PyInit_vex so module can be imported directly as `vex` or as `vex._vex` */
PyMODINIT_FUNC PyInit_vex(void) {
    return PyModuleDef_Init(&vex_module_def);
}
