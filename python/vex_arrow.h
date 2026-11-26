#pragma once
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "vx_result.h"

PyObject *vex_result_arrow_c_schema(vx_result_t *res);
PyObject *vex_result_arrow_c_stream(vx_result_t *res, PyObject *requested_schema);
PyObject *vex_colview_arrow_c_array(const vx_vec_t *vec, const char *col_name,
                                    vx_owner_t *aux_owner, PyObject *requested_schema);

