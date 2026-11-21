#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "vx_db.h"
#include "vx_simd.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define VX_PY_MAX_COLS    64U
#define VX_PY_STACK_PARAM 32U

static PyObject *VexError = NULL;

typedef struct {
    PyObject_HEAD
    vx_db_t *db;
} PyVexDB;

typedef struct {
    PyObject_HEAD
    vx_result_t *res;
} PyVexResult;

typedef struct {
    PyObject_HEAD
    PyVexResult *owner;
    uint32_t     col_idx;
    Py_ssize_t   shape[1];
    Py_ssize_t   strides[1];
    Py_ssize_t   exports;
} PyVexColumn;

static PyTypeObject PyVexDBType;
static PyTypeObject PyVexResultType;
static PyTypeObject PyVexColumnType;

/* ==================== TYPE CONVERSION HELPERS ==================== */

static const char *vx_type_name(vx_type_t t) {
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
    memset(out, 0, sizeof(*out));
    if (obj == Py_None) {
        out->type = VX_TYPE_I64;
        out->is_null = true;
        return true;
    }
    if (PyBool_Check(obj)) {
        out->type = VX_TYPE_BOOL;
        out->is_null = false;
        out->v.b = (obj == Py_True);
        return true;
    }
    if (PyLong_Check(obj)) {
        int overflow = 0;
        long long sv = PyLong_AsLongLongAndOverflow(obj, &overflow);
        if (!overflow && !(sv == -1 && PyErr_Occurred())) {
            out->type = VX_TYPE_I64;
            out->is_null = false;
            out->v.i64 = (int64_t)sv;
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
        out->v.f64 = PyFloat_AS_DOUBLE(obj);
        return true;
    }
    if (PyUnicode_Check(obj)) {
        Py_ssize_t slen = 0;
        const char *s = PyUnicode_AsUTF8AndSize(obj, &slen);
        if (!s) {
            return false;
        }
        if ((uint64_t)slen > UINT32_MAX) {
            PyErr_SetString(PyExc_ValueError, "string value exceeds 4GB limit");
            return false;
        }
        out->type = VX_TYPE_STR;
        out->is_null = false;
        out->v.str.ptr = s;
        out->v.str.len = (uint32_t)slen;
        return true;
    }
    if (PyBytes_Check(obj)) {
        Py_ssize_t slen = PyBytes_GET_SIZE(obj);
        if ((uint64_t)slen > UINT32_MAX) {
            PyErr_SetString(PyExc_ValueError, "bytes value exceeds 4GB limit");
            return false;
        }
        out->type = VX_TYPE_STR;
        out->is_null = false;
        out->v.str.ptr = PyBytes_AS_STRING(obj);
        out->v.str.len = (uint32_t)slen;
        return true;
    }

    PyErr_Format(PyExc_TypeError, "unsupported vex value type: %.100s",
                 Py_TYPE(obj)->tp_name);
    return false;
}

/* ==================== ColumnView (PEP 3118 ZERO-COPY) ==================== */

static PyObject *py_col_new(PyVexResult *owner, uint32_t col_idx) {
    PyVexColumn *col = PyObject_New(PyVexColumn, &PyVexColumnType);
    if (!col) {
        return NULL;
    }
    Py_INCREF(owner);
    col->owner = owner;
    col->col_idx = col_idx;
    col->shape[0] = (Py_ssize_t)owner->res->nrows;
    col->strides[0] = 0;
    col->exports = 0;
    return (PyObject *)col;
}

static void py_col_dealloc(PyVexColumn *self) {
    Py_XDECREF(self->owner);
    PyObject_Del(self);
}

static Py_ssize_t py_col_len(PyVexColumn *self) {
    return (Py_ssize_t)self->owner->res->nrows;
}

static PyObject *py_col_subscript(PyVexColumn *self, PyObject *key) {
    const vx_result_t *res = self->owner->res;
    Py_ssize_t nrows = (Py_ssize_t)res->nrows;

    if (PyIndex_Check(key)) {
        Py_ssize_t idx = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (idx == -1 && PyErr_Occurred()) {
            return NULL;
        }
        if (idx < 0) {
            idx += nrows;
        }
        if (idx < 0 || idx >= nrows) {
            PyErr_SetString(PyExc_IndexError, "column index out of range");
            return NULL;
        }
        return vx_cell_to_py(res, self->col_idx, (uint32_t)idx);
    }

    if (PySlice_Check(key)) {
        Py_ssize_t start = 0, stop = 0, step = 0, slen = 0;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
            return NULL;
        }
        slen = PySlice_AdjustIndices(nrows, &start, &stop, step);
        PyObject *list = PyList_New(slen);
        if (!list) {
            return NULL;
        }
        for (Py_ssize_t i = 0, r = start; i < slen; i++, r += step) {
            PyObject *val = vx_cell_to_py(res, self->col_idx, (uint32_t)r);
            if (!val) {
                Py_DECREF(list);
                return NULL;
            }
            PyList_SET_ITEM(list, i, val);
        }
        return list;
    }

    PyErr_SetString(PyExc_TypeError, "column indices must be integers or slices");
    return NULL;
}

static int py_col_getbuffer(PyVexColumn *self, Py_buffer *view, int flags) {
    static uint8_t k_empty_dummy = 0;

    if (flags & PyBUF_WRITABLE) {
        PyErr_SetString(PyExc_BufferError, "vex column buffer is read-only");
        return -1;
    }

    const vx_result_t *res = self->owner->res;
    const vx_result_col_t *col = &res->cols[self->col_idx];
    const char *fmt = NULL;
    Py_ssize_t itemsize = 0;

    switch (col->type) {
        case VX_TYPE_I64:  fmt = "q"; itemsize = (Py_ssize_t)sizeof(int64_t);  break;
        case VX_TYPE_U64:  fmt = "Q"; itemsize = (Py_ssize_t)sizeof(uint64_t); break;
        case VX_TYPE_F64:  fmt = "d"; itemsize = (Py_ssize_t)sizeof(double);   break;
        case VX_TYPE_BOOL: fmt = "?"; itemsize = (Py_ssize_t)sizeof(uint8_t);  break;
        case VX_TYPE_STR:
        default:
            PyErr_SetString(PyExc_BufferError,
                            "string columns cannot be exported via numeric buffer protocol; use to_list()");
            return -1;
    }

    self->shape[0] = (Py_ssize_t)res->nrows;
    self->strides[0] = itemsize;

    view->buf = (res->nrows > 0 && col->data) ? col->data : &k_empty_dummy;
    view->obj = (PyObject *)self;
    Py_INCREF(self);
    view->len = (Py_ssize_t)res->nrows * itemsize;
    view->itemsize = itemsize;
    view->readonly = 1;
    view->ndim = 1;
    view->format = (flags & PyBUF_FORMAT) ? (char *)fmt : NULL;
    view->shape = (flags & PyBUF_ND) ? self->shape : NULL;
    view->strides = (flags & PyBUF_STRIDES) ? self->strides : NULL;
    view->suboffsets = NULL;
    view->internal = NULL;

    self->exports++;
    return 0;
}

static void py_col_releasebuffer(PyVexColumn *self, Py_buffer *Py_UNUSED(view)) {
    if (self->exports > 0) {
        self->exports--;
    }
}

static PyObject *py_col_name(PyVexColumn *self, void *Py_UNUSED(closure)) {
    return PyUnicode_FromString(self->owner->res->cols[self->col_idx].name);
}

static PyObject *py_col_dtype(PyVexColumn *self, void *Py_UNUSED(closure)) {
    return PyUnicode_FromString(vx_type_name(self->owner->res->cols[self->col_idx].type));
}

static PyObject *py_col_has_nulls(PyVexColumn *self, void *Py_UNUSED(closure)) {
    const vx_result_t *res = self->owner->res;
    const vx_result_col_t *col = &res->cols[self->col_idx];
    if (!col->null_bits || res->nrows == 0) {
        Py_RETURN_FALSE;
    }
    uint32_t full_words = res->nrows >> 6;
    for (uint32_t w = 0; w < full_words; w++) {
        if (col->null_bits[w] != 0ULL) {
            Py_RETURN_TRUE;
        }
    }
    uint32_t rem = res->nrows & 63U;
    if (rem > 0) {
        uint64_t mask = (1ULL << rem) - 1ULL;
        if ((col->null_bits[full_words] & mask) != 0ULL) {
            Py_RETURN_TRUE;
        }
    }
    Py_RETURN_FALSE;
}

static PyObject *py_col_to_list(PyVexColumn *self, PyObject *Py_UNUSED(ignored)) {
    const vx_result_t *res = self->owner->res;
    PyObject *list = PyList_New((Py_ssize_t)res->nrows);
    if (!list) {
        return NULL;
    }
    for (uint32_t r = 0; r < res->nrows; r++) {
        PyObject *v = vx_cell_to_py(res, self->col_idx, r);
        if (!v) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, (Py_ssize_t)r, v);
    }
    return list;
}

static PyObject *py_col_np(PyVexColumn *self, PyObject *Py_UNUSED(ignored)) {
    PyObject *np_mod = PyImport_ImportModule("numpy");
    if (!np_mod) {
        return NULL;
    }
    PyObject *arr = PyObject_CallMethod(np_mod, "asarray", "O", (PyObject *)self);
    Py_DECREF(np_mod);
    return arr;
}

static PySequenceMethods py_col_as_sequence = {
    .sq_length = (lenfunc)py_col_len,
};

static PyMappingMethods py_col_as_mapping = {
    .mp_length = (lenfunc)py_col_len,
    .mp_subscript = (binaryfunc)py_col_subscript,
};

static PyBufferProcs py_col_as_buffer = {
    .bf_getbuffer = (getbufferproc)py_col_getbuffer,
    .bf_releasebuffer = (releasebufferproc)py_col_releasebuffer,
};

static PyGetSetDef py_col_getset[] = {
    { "name",      (getter)py_col_name,      NULL, "Column name", NULL },
    { "dtype",     (getter)py_col_dtype,     NULL, "Column data type", NULL },
    { "has_nulls", (getter)py_col_has_nulls, NULL, "Whether column contains NULL values", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyMethodDef py_col_methods[] = {
    { "to_list", (PyCFunction)py_col_to_list, METH_NOARGS, "Materialize column as a Python list" },
    { "np",      (PyCFunction)py_col_np,      METH_NOARGS, "Zero-copy read-only NumPy ndarray view" },
    { NULL, NULL, 0, NULL }
};

static PyTypeObject PyVexColumnType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "vex.ColumnView",
    .tp_basicsize = sizeof(PyVexColumn),
    .tp_dealloc = (destructor)py_col_dealloc,
    .tp_as_sequence = &py_col_as_sequence,
    .tp_as_mapping = &py_col_as_mapping,
    .tp_as_buffer = &py_col_as_buffer,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = py_col_methods,
    .tp_getset = py_col_getset,
};

/* ==================== Result ==================== */

static PyObject *py_res_new(vx_result_t *res) {
    PyVexResult *obj = PyObject_New(PyVexResult, &PyVexResultType);
    if (!obj) {
        vx_result_free(res);
        return NULL;
    }
    obj->res = res;
    return (PyObject *)obj;
}

static void py_res_dealloc(PyVexResult *self) {
    vx_result_free(self->res);
    self->res = NULL;
    PyObject_Del(self);
}

static Py_ssize_t py_res_len(PyVexResult *self) {
    return self->res ? (Py_ssize_t)self->res->nrows : 0;
}

static PyObject *py_res_subscript(PyVexResult *self, PyObject *key) {
    if (PyUnicode_Check(key)) {
        const char *name = PyUnicode_AsUTF8(key);
        if (!name) {
            return NULL;
        }
        int ci = vx_result_col_idx(self->res, name);
        if (ci < 0) {
            PyErr_SetObject(PyExc_KeyError, key);
            return NULL;
        }
        return py_col_new(self, (uint32_t)ci);
    }

    if (PyIndex_Check(key)) {
        Py_ssize_t idx = PyNumber_AsSsize_t(key, PyExc_IndexError);
        if (idx == -1 && PyErr_Occurred()) {
            return NULL;
        }
        Py_ssize_t ncols = (Py_ssize_t)self->res->ncols;
        if (idx < 0) {
            idx += ncols;
        }
        if (idx < 0 || idx >= ncols) {
            PyErr_SetString(PyExc_IndexError, "column index out of range");
            return NULL;
        }
        return py_col_new(self, (uint32_t)idx);
    }

    PyErr_SetString(PyExc_TypeError, "result subscript must be a column name (str) or index (int)");
    return NULL;
}

static PyObject *py_res_nrows(PyVexResult *self, void *Py_UNUSED(closure)) {
    return PyLong_FromUnsignedLong(self->res->nrows);
}

static PyObject *py_res_ncols(PyVexResult *self, void *Py_UNUSED(closure)) {
    return PyLong_FromUnsignedLong(self->res->ncols);
}

static PyObject *py_res_rows_affected(PyVexResult *self, void *Py_UNUSED(closure)) {
    return PyLong_FromUnsignedLongLong((unsigned long long)self->res->rows_affected);
}

static PyObject *py_res_columns(PyVexResult *self, void *Py_UNUSED(closure)) {
    const vx_result_t *res = self->res;
    PyObject *tup = PyTuple_New((Py_ssize_t)res->ncols);
    if (!tup) {
        return NULL;
    }
    for (uint32_t c = 0; c < res->ncols; c++) {
        PyObject *s = PyUnicode_FromString(res->cols[c].name);
        if (!s) {
            Py_DECREF(tup);
            return NULL;
        }
        PyTuple_SET_ITEM(tup, (Py_ssize_t)c, s);
    }
    return tup;
}

static PyObject *py_res_explain(PyVexResult *self, void *Py_UNUSED(closure)) {
    if (!self->res->explain_text) {
        Py_RETURN_NONE;
    }
    return PyUnicode_FromString(self->res->explain_text);
}

static PyObject *py_res_rows(PyVexResult *self, PyObject *Py_UNUSED(ignored)) {
    const vx_result_t *res = self->res;
    PyObject *list = PyList_New((Py_ssize_t)res->nrows);
    if (!list) {
        return NULL;
    }
    for (uint32_t r = 0; r < res->nrows; r++) {
        PyObject *tup = PyTuple_New((Py_ssize_t)res->ncols);
        if (!tup) {
            Py_DECREF(list);
            return NULL;
        }
        for (uint32_t c = 0; c < res->ncols; c++) {
            PyObject *v = vx_cell_to_py(res, c, r);
            if (!v) {
                Py_DECREF(tup);
                Py_DECREF(list);
                return NULL;
            }
            PyTuple_SET_ITEM(tup, (Py_ssize_t)c, v);
        }
        PyList_SET_ITEM(list, (Py_ssize_t)r, tup);
    }
    return list;
}

static PyObject *py_res_dicts(PyVexResult *self, PyObject *Py_UNUSED(ignored)) {
    const vx_result_t *res = self->res;
    PyObject *list = PyList_New((Py_ssize_t)res->nrows);
    if (!list) {
        return NULL;
    }

    PyObject **keys = NULL;
    if (res->ncols > 0) {
        keys = (PyObject **)PyMem_Malloc(res->ncols * sizeof(PyObject *));
        if (!keys) {
            Py_DECREF(list);
            return PyErr_NoMemory();
        }
        for (uint32_t c = 0; c < res->ncols; c++) {
            keys[c] = PyUnicode_FromString(res->cols[c].name);
            if (!keys[c]) {
                for (uint32_t j = 0; j < c; j++) {
                    Py_DECREF(keys[j]);
                }
                PyMem_Free(keys);
                Py_DECREF(list);
                return NULL;
            }
        }
    }

    for (uint32_t r = 0; r < res->nrows; r++) {
        PyObject *d = PyDict_New();
        if (!d) {
            goto fail;
        }
        for (uint32_t c = 0; c < res->ncols; c++) {
            PyObject *v = vx_cell_to_py(res, c, r);
            if (!v) {
                Py_DECREF(d);
                goto fail;
            }
            int rc = PyDict_SetItem(d, keys[c], v);
            Py_DECREF(v);
            if (rc < 0) {
                Py_DECREF(d);
                goto fail;
            }
        }
        PyList_SET_ITEM(list, (Py_ssize_t)r, d);
    }

    for (uint32_t c = 0; c < res->ncols; c++) {
        Py_DECREF(keys[c]);
    }
    PyMem_Free(keys);
    return list;

fail:
    for (uint32_t c = 0; c < res->ncols; c++) {
        Py_DECREF(keys[c]);
    }
    PyMem_Free(keys);
    Py_DECREF(list);
    return NULL;
}

static PySequenceMethods py_res_as_sequence = {
    .sq_length = (lenfunc)py_res_len,
};

static PyMappingMethods py_res_as_mapping = {
    .mp_length = (lenfunc)py_res_len,
    .mp_subscript = (binaryfunc)py_res_subscript,
};

static PyGetSetDef py_res_getset[] = {
    { "nrows",         (getter)py_res_nrows,         NULL, "Number of rows", NULL },
    { "ncols",         (getter)py_res_ncols,         NULL, "Number of columns", NULL },
    { "rows_affected", (getter)py_res_rows_affected, NULL, "Rows affected by mutation", NULL },
    { "columns",       (getter)py_res_columns,       NULL, "Tuple of column names", NULL },
    { "explain",       (getter)py_res_explain,       NULL, "Query plan string if '?' was used", NULL },
    { NULL, NULL, NULL, NULL, NULL }
};

static PyMethodDef py_res_methods[] = {
    { "rows",  (PyCFunction)py_res_rows,  METH_NOARGS, "Materialize result as list of tuples" },
    { "dicts", (PyCFunction)py_res_dicts, METH_NOARGS, "Materialize result as list of dicts" },
    { NULL, NULL, 0, NULL }
};

static PyTypeObject PyVexResultType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "vex.Result",
    .tp_basicsize = sizeof(PyVexResult),
    .tp_dealloc = (destructor)py_res_dealloc,
    .tp_as_sequence = &py_res_as_sequence,
    .tp_as_mapping = &py_res_as_mapping,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = py_res_methods,
    .tp_getset = py_res_getset,
};

/* ==================== Database (GIL-FREE C EXECUTION) ==================== */

static bool db_check_open(const PyVexDB *self) {
    if (!self->db) {
        PyErr_SetString(VexError, "database is closed");
        return false;
    }
    return true;
}

static void py_db_close_internal(PyVexDB *self) {
    if (self->db) {
        vx_db_t *db = self->db;
        self->db = NULL;
        Py_BEGIN_ALLOW_THREADS
        vx_db_close(db);
        Py_END_ALLOW_THREADS
    }
}

static void py_db_dealloc(PyVexDB *self) {
    py_db_close_internal(self);
    PyObject_Del(self);
}

static PyObject *py_db_close(PyVexDB *self, PyObject *Py_UNUSED(ignored)) {
    py_db_close_internal(self);
    Py_RETURN_NONE;
}

static PyObject *py_db_enter(PyVexDB *self, PyObject *Py_UNUSED(ignored)) {
    if (!db_check_open(self)) {
        return NULL;
    }
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *py_db_exit(PyVexDB *self, PyObject *Py_UNUSED(args)) {
    py_db_close_internal(self);
    Py_RETURN_FALSE;
}

static PyObject *py_db_flush(PyVexDB *self, PyObject *Py_UNUSED(ignored)) {
    if (!db_check_open(self)) {
        return NULL;
    }
    vx_db_t *db = self->db;
    Py_BEGIN_ALLOW_THREADS
    vx_db_flush(db);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

static PyObject *py_db_checkpoint(PyVexDB *self, PyObject *Py_UNUSED(ignored)) {
    if (!db_check_open(self)) {
        return NULL;
    }
    vx_db_t *db = self->db;
    bool ok = false;
    Py_BEGIN_ALLOW_THREADS
    ok = vx_db_checkpoint(db);
    Py_END_ALLOW_THREADS
    if (!ok) {
        PyErr_SetString(VexError, "checkpoint() failed (requires WAL-backed database)");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *py_db_stats(PyVexDB *self, PyObject *Py_UNUSED(ignored)) {
    if (!db_check_open(self)) {
        return NULL;
    }
    vx_db_stats_t st = vx_db_stats(self->db);
    return Py_BuildValue(
        "{s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K}",
        "queries_executed",    (unsigned long long)st.queries_executed,
        "mutations_committed", (unsigned long long)st.mutations_committed,
        "wal_lsn",             (unsigned long long)st.wal_lsn,
        "chunks_pruned",       (unsigned long long)st.chunks_pruned,
        "logs_emitted",        (unsigned long long)st.logs_emitted,
        "logs_dropped",        (unsigned long long)st.logs_dropped,
        "checkpoints",         (unsigned long long)st.checkpoints,
        "dead_rows",           (unsigned long long)st.dead_rows
    );
}

static PyObject *py_db_q(PyVexDB *self, PyObject *args, PyObject *kwargs) {
    if (!db_check_open(self)) {
        return NULL;
    }

    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    if (nargs < 1 || nargs > 2) {
        PyErr_SetString(PyExc_TypeError, "q() expects (query[, sync], **params)");
        return NULL;
    }

    PyObject *q_obj = PyTuple_GET_ITEM(args, 0);
    if (!PyUnicode_Check(q_obj)) {
        PyErr_SetString(PyExc_TypeError, "query must be a str");
        return NULL;
    }
    const char *q_str = PyUnicode_AsUTF8(q_obj);
    if (!q_str) {
        return NULL;
    }

    bool sync = false;
    if (nargs == 2) {
        int s = PyObject_IsTrue(PyTuple_GET_ITEM(args, 1));
        if (s < 0) {
            return NULL;
        }
        sync = (s != 0);
    }

    vx_param_t stack_params[VX_PY_STACK_PARAM];
    vx_param_t *params = stack_params;
    size_t nparams = 0;
    size_t cap = VX_PY_STACK_PARAM;

    if (kwargs && PyDict_Check(kwargs)) {
        Py_ssize_t klen = PyDict_Size(kwargs);
        if ((size_t)klen > cap) {
            params = (vx_param_t *)PyMem_Malloc((size_t)klen * sizeof(vx_param_t));
            if (!params) {
                return PyErr_NoMemory();
            }
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
            params[nparams].name = kname;
            if (!py_obj_to_vx_val(v, &params[nparams].val)) {
                if (params != stack_params) PyMem_Free(params);
                return NULL;
            }
            nparams++;
        }
    }

    vx_db_t *db = self->db;
    vx_result_t *res = NULL;

    Py_BEGIN_ALLOW_THREADS
    res = vx_db_query(db, q_str, nparams ? params : NULL, nparams, sync);
    Py_END_ALLOW_THREADS

    if (params != stack_params) {
        PyMem_Free(params);
    }

    if (!res) {
        PyErr_SetString(VexError, "query execution failed (out of memory)");
        return NULL;
    }
    if (!res->ok) {
        PyErr_SetString(VexError, res->err_msg[0] ? res->err_msg : "query error");
        vx_result_free(res);
        return NULL;
    }
    return py_res_new(res);
}

static PyObject *py_db_put(PyVexDB *self, PyObject *args, PyObject *kwargs) {
    if (!db_check_open(self)) {
        return NULL;
    }

    const char *table = NULL;
    PyObject *row_dict = NULL;
    int upsert = 0;
    int sync = 0;
    static char *kwlist[] = { "table", "row", "upsert", "sync", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO!|pp:put", kwlist,
                                     &table, &PyDict_Type, &row_dict,
                                     &upsert, &sync)) {
        return NULL;
    }

    Py_ssize_t nfields = PyDict_Size(row_dict);
    if (nfields <= 0 || (uint32_t)nfields > VX_PY_MAX_COLS) {
        PyErr_SetString(VexError, "row must contain between 1 and 64 fields");
        return NULL;
    }

    const char *col_names[VX_PY_MAX_COLS];
    vx_val_t vals[VX_PY_MAX_COLS];
    uint32_t ncols = 0;

    Py_ssize_t pos = 0;
    PyObject *k = NULL, *v = NULL;
    while (PyDict_Next(row_dict, &pos, &k, &v)) {
        if (!PyUnicode_Check(k)) {
            PyErr_SetString(PyExc_TypeError, "row keys must be str");
            return NULL;
        }
        const char *cname = PyUnicode_AsUTF8(k);
        if (!cname) {
            return NULL;
        }
        col_names[ncols] = cname;
        if (!py_obj_to_vx_val(v, &vals[ncols])) {
            return NULL;
        }
        ncols++;
    }

    vx_db_t *db = self->db;
    bool ok = false;

    Py_BEGIN_ALLOW_THREADS
    ok = vx_db_put_row(db, table, col_names, vals, ncols, upsert != 0, sync != 0);
    Py_END_ALLOW_THREADS

    if (!ok) {
        PyErr_SetString(VexError, "put() failed");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyMethodDef py_db_methods[] = {
    { "q",          (PyCFunction)(void (*)(void))py_db_q,   METH_VARARGS | METH_KEYWORDS, "Execute a Vex query or mutation (GIL-free)" },
    { "put",        (PyCFunction)(void (*)(void))py_db_put, METH_VARARGS | METH_KEYWORDS, "Fast-path structured dict insert/upsert" },
    { "flush",      (PyCFunction)py_db_flush,               METH_NOARGS,                  "Flush staged micro-batch writes to MVCC snapshot" },
    { "checkpoint", (PyCFunction)py_db_checkpoint,          METH_NOARGS,                  "Write atomic snapshot and compact WAL" },
    { "stats",      (PyCFunction)py_db_stats,               METH_NOARGS,                  "Return engine runtime statistics" },
    { "close",      (PyCFunction)py_db_close,               METH_NOARGS,                  "Flush and close the database" },
    { "__enter__",  (PyCFunction)py_db_enter,               METH_NOARGS,                  "Enter context manager" },
    { "__exit__",   (PyCFunction)py_db_exit,                METH_VARARGS,                 "Exit context manager and close database" },
    { NULL, NULL, 0, NULL }
};

static PyTypeObject PyVexDBType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "vex.Database",
    .tp_basicsize = sizeof(PyVexDB),
    .tp_dealloc = (destructor)py_db_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = py_db_methods,
};

/* ==================== MODULE ENTRYPOINTS ==================== */

static PyObject *py_vex_open(PyObject *Py_UNUSED(mod), PyObject *args, PyObject *kwargs) {
    PyObject *path_obj = Py_None;
    vx_db_cfg_t cfg = vx_db_default_cfg();
    unsigned int workers = cfg.worker_threads;
    unsigned int chunk_rows = cfg.chunk_rows;
    unsigned long long max_readers = (unsigned long long)cfg.max_readers;
    unsigned int flush_interval_us = cfg.flush_interval_us;
    int fsync_flag = cfg.fsync_on_commit ? 1 : 0;

    static char *kwlist[] = {
        "path", "workers", "chunk_rows", "max_readers", "flush_interval_us", "fsync", NULL
    };
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OIIKIp:open", kwlist,
                                     &path_obj, &workers, &chunk_rows,
                                     &max_readers, &flush_interval_us, &fsync_flag)) {
        return NULL;
    }

    PyObject *fs_bytes = NULL;
    const char *wal_path = NULL;
    if (path_obj != Py_None) {
        if (!PyUnicode_FSConverter(path_obj, &fs_bytes)) {
            return NULL;
        }
        wal_path = PyBytes_AS_STRING(fs_bytes);
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
        PyErr_SetString(VexError, "failed to open vex database");
        return NULL;
    }

    PyVexDB *obj = PyObject_New(PyVexDB, &PyVexDBType);
    if (!obj) {
        vx_db_close(db);
        return NULL;
    }
    obj->db = db;
    return (PyObject *)obj;
}

static PyObject *py_vex_simd_backend(PyObject *Py_UNUSED(mod), PyObject *Py_UNUSED(ignored)) {
    return PyUnicode_FromString(vx_simd_name(vx_simd_active()));
}

static PyMethodDef vex_module_methods[] = {
    { "open", (PyCFunction)(void (*)(void))py_vex_open, METH_VARARGS | METH_KEYWORDS,
      "Open an in-memory or WAL-backed Vex database" },
    { "simd_backend", (PyCFunction)py_vex_simd_backend, METH_NOARGS,
      "Return the active runtime SIMD backend name" },
    { NULL, NULL, 0, NULL }
};

static struct PyModuleDef vex_module_def = {
    PyModuleDef_HEAD_INIT,
    .m_name = "vex",
    .m_doc = "High-speed C11 columnar storage engine with a Pythonic query language",
    .m_size = -1,
    .m_methods = vex_module_methods,
};

PyMODINIT_FUNC PyInit_vex(void) {
    if (PyType_Ready(&PyVexDBType) < 0 ||
        PyType_Ready(&PyVexResultType) < 0 ||
        PyType_Ready(&PyVexColumnType) < 0) {
        return NULL;
    }

    PyObject *m = PyModule_Create(&vex_module_def);
    if (!m) {
        return NULL;
    }

    VexError = PyErr_NewException("vex.VexError", PyExc_Exception, NULL);
    if (!VexError) {
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(VexError);
    if (PyModule_AddObject(m, "VexError", VexError) < 0) {
        Py_DECREF(VexError);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&PyVexDBType);
    if (PyModule_AddObject(m, "Database", (PyObject *)&PyVexDBType) < 0) {
        Py_DECREF(&PyVexDBType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&PyVexResultType);
    if (PyModule_AddObject(m, "Result", (PyObject *)&PyVexResultType) < 0) {
        Py_DECREF(&PyVexResultType);
        Py_DECREF(m);
        return NULL;
    }

    Py_INCREF(&PyVexColumnType);
    if (PyModule_AddObject(m, "ColumnView", (PyObject *)&PyVexColumnType) < 0) {
        Py_DECREF(&PyVexColumnType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

