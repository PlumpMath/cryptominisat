/*************
Python bindings to CryptoMiniSat (http://msoos.org)

Copyright (c) 2013, Ilan Schnell, Continuum Analytics, Inc.
            2014, Mate Soos

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
**********************************/

#define PYCRYPTOSAT_URL  "https://pypi.python.org/pypi/pycryptosat"

#include <Python.h>
#include <structmember.h>
#include <limits>

#include "assert.h"
#include <cryptominisat5/cryptominisat.h>
using namespace CMSat;

#define MODULE_NAME "pycryptosat"
#define MODULE_DOC "CryptoSAT satisfiability solver."

#if PY_MAJOR_VERSION >= 3
#define IS_PY3K
#endif

#ifndef IS_PY3K
#define IS_INT(x)  (PyInt_Check(x) || PyLong_Check(x))
#else
#define IS_INT(x)  PyLong_Check(x)
#endif

#if PY_MAJOR_VERSION == 2 && PY_MINOR_VERSION <= 5
#define PyUnicode_FromString  PyString_FromString
#endif

// Should only be necessary on Python <2.6
#ifndef Py_TYPE
    #define Py_TYPE(ob) (((PyObject*)(ob))->ob_type)
#endif

#ifndef PyVarObject_HEAD_INIT
    #define PyVarObject_HEAD_INIT(type, size) \
        PyObject_HEAD_INIT(type) size,
#endif

typedef struct {
    PyObject_HEAD
    /* Type-specific fields go here. */
    SATSolver* cmsat;
} Solver;

static PyObject *outofconflerr = NULL;

static SATSolver* setup_solver(PyObject *args, PyObject *kwds)
{
    static const char * kwlist[] = {"verbose", "confl_limit", "threads", NULL};

    int verbose = 0;
    int num_threads = 1;
    long confl_limit = std::numeric_limits<long>::max();
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ili", const_cast<char **>(kwlist), &verbose, &confl_limit, &num_threads)) {
        return NULL;
    }
    if (verbose < 0) {
        PyErr_SetString(PyExc_ValueError, "verbosity must be at least 0");
        return NULL;
    }
    if (confl_limit < 0) {
        PyErr_SetString(PyExc_ValueError, "conflict limit must be at least 0");
        return NULL;
    }
    if (num_threads <= 0) {
        PyErr_SetString(PyExc_ValueError, "number of threads must be at least 1");
        return NULL;
    }

    SATSolver *cmsat = new SATSolver;
    cmsat->set_max_confl(confl_limit);
    cmsat->set_verbosity(verbose);
    cmsat->set_num_threads(num_threads);

    return cmsat;
}

static int convert_lit_to_sign_and_var(PyObject* lit, long& var, bool& sign)
{
    if (!IS_INT(lit))  {
        PyErr_SetString(PyExc_TypeError, "integer expected");
        return 0;
    }

    long val = PyLong_AsLong(lit);
    if (val == 0) {
        PyErr_SetString(PyExc_ValueError, "non-zero integer expected");
        return 0;
    }
    if (val > std::numeric_limits<int>::max()/2
        || val < std::numeric_limits<int>::min()/2
    ) {
        PyErr_Format(PyExc_ValueError, "integer %ld is too small or too large", val);
        return 0;
    }

    sign = false;
    if (val < 0) {
        val *= -1;
        sign = true;
    }
    val--;
    var = val;

    return 1;
}

static int parse_clause(
    Solver *self
    , PyObject *clause
    , std::vector<Lit>& lits
) {
    PyObject *iterator = PyObject_GetIter(clause);
    if (iterator == NULL) {
        PyErr_SetString(PyExc_TypeError, "interable object expected");
        return 0;
    }

    PyObject *lit;
    while ((lit = PyIter_Next(iterator)) != NULL) {
        long var;
        bool sign;
        int ret = convert_lit_to_sign_and_var(lit, var, sign);
        Py_DECREF(lit);
        if (!ret) {
            Py_DECREF(iterator);
            return 0;
        }

        if (var >= self->cmsat->nVars()) {
            for(long i = (long)self->cmsat->nVars(); i <= var ; i++) {
                self->cmsat->new_var();
            }
        }

        lits.push_back(Lit(var, sign));
    }
    Py_DECREF(iterator);
    if (PyErr_Occurred()) {
        return 0;
    }

    return 1;
}

static int parse_xor_clause(
    Solver *self
    , PyObject *clause
    , std::vector<uint32_t>& vars
) {
    PyObject *iterator = PyObject_GetIter(clause);
    if (iterator == NULL) {
        PyErr_SetString(PyExc_TypeError, "interable object expected");
        return 0;
    }

    PyObject *lit;
    while ((lit = PyIter_Next(iterator)) != NULL) {
        long var;
        bool sign;
        int ret = convert_lit_to_sign_and_var(lit, var, sign);
        Py_DECREF(lit);
        if (!ret) {
            Py_DECREF(iterator);
            return 0;
        }
        if (sign) {
            PyErr_SetString(PyExc_ValueError, "XOR clause must contiain only positive variables (not inverted literals)");
            Py_DECREF(iterator);
            return 0;
        }

        if (var >= self->cmsat->nVars()) {
            for(long i = (long)self->cmsat->nVars(); i <= var ; i++) {
                self->cmsat->new_var();
            }
        }

        vars.push_back(var);
    }
    Py_DECREF(iterator);
    if (PyErr_Occurred()) {
        return 0;
    }

    return 1;
}

static PyObject* add_clause(Solver *self, PyObject *args, PyObject *kwds)
{
    static const char* kwlist[] = {"clause", NULL};
    PyObject *clause;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", const_cast<char **>(kwlist), &clause)) {
        return NULL;
    }

    std::vector<Lit> lits;
    if (!parse_clause(self, clause, lits)) {
        return 0;
    }
    self->cmsat->add_clause(lits);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* add_xor_clause(Solver *self, PyObject *args, PyObject *kwds)
{
    static const char* kwlist[] = {"xor_clause", "rhs", NULL};
    PyObject *rhs;
    PyObject *clause;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", const_cast<char **>(kwlist), &clause, &rhs)) {
        return NULL;
    }
    if (!PyBool_Check(rhs)) {
        PyErr_SetString(PyExc_TypeError, "rhs must be boolean");
        return NULL;
    }
    bool real_rhs = PyObject_IsTrue(rhs);

    std::vector<uint32_t> vars;
    if (!parse_xor_clause(self, clause, vars)) {
        return 0;
    }

    self->cmsat->add_xor_clause(vars, real_rhs);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject* get_solution(SATSolver *cmsat)
{
    PyObject *tuple;

    unsigned max_idx = cmsat->nVars();
    tuple = PyTuple_New((Py_ssize_t) max_idx+1);
    if (tuple == NULL) {
        PyErr_SetString(PyExc_SystemError, "failed to create a tuple");
        return NULL;
    }

    Py_INCREF(Py_None);
    if (PyTuple_SetItem(tuple, (Py_ssize_t)0, Py_None) < 0) {
        PyErr_SetString(PyExc_SystemError, "failed to add 1st element to tuple");
        Py_DECREF(tuple);
        return NULL;
    }

    for (unsigned i = 0; i < max_idx; i++) {
        lbool v = cmsat->get_model()[i];
        PyObject *py_value = NULL;
        if (v == l_True) {
            Py_INCREF(Py_True);
            py_value = Py_True;
        } else if (v == l_False) {
            Py_INCREF(Py_False);
            py_value = Py_False;
        } else if (v == l_Undef) {
            Py_INCREF(Py_None);
            py_value = Py_None;
        }

        if (PyTuple_SetItem(tuple, (Py_ssize_t)i+1, py_value) < 0) {
            PyErr_SetString(PyExc_SystemError, "failed to add to tuple");
            Py_DECREF(tuple);
            return NULL;
        }
    }
    return tuple;
}

static int parse_assumption_lits(PyObject* assumptions, SATSolver* cmsat, std::vector<Lit>& assumption_lits)
{
    PyObject *iterator = PyObject_GetIter(assumptions);
    if (iterator == NULL) {
        PyErr_SetString(PyExc_TypeError, "interable object expected");
        return 0;
    }

    PyObject *lit;
    while ((lit = PyIter_Next(iterator)) != NULL) {
        long var;
        bool sign;
        int ret = convert_lit_to_sign_and_var(lit, var, sign);
        Py_DECREF(lit);
        if (!ret) {
            Py_DECREF(iterator);
            return 0;
        }

        if (var >= cmsat->nVars()) {
            Py_DECREF(iterator);
            PyErr_Format(PyExc_ValueError, "Variable %ld not used in clauses", var+1);
            return 0;
        }

        assumption_lits.push_back(Lit(var, sign));
    }
    Py_DECREF(iterator);
    if (PyErr_Occurred()) {
        return 0;
    }

    return 1;
}

static PyObject* solve(Solver *self, PyObject *args, PyObject *kwds)
{
    PyObject* assumptions = NULL;
    static const char* kwlist[] = {"assumptions", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", const_cast<char **>(kwlist), &assumptions)) {
        return NULL;
    }

    std::vector<Lit> assumption_lits;
    if (assumptions) {
        if (!parse_assumption_lits(assumptions, self->cmsat, assumption_lits)) {
            return 0;
        }
    }

    PyObject *result = NULL;

    result = PyTuple_New((Py_ssize_t) 2);
    if (result == NULL) {
        PyErr_SetString(PyExc_SystemError, "failed to create a tuple");
        return NULL;
    }

    lbool res;
    Py_BEGIN_ALLOW_THREADS      /* release GIL */
    res = self->cmsat->solve(&assumption_lits);
    Py_END_ALLOW_THREADS

    if (res == l_True) {
        PyObject* solution = get_solution(self->cmsat);
        if (!solution) {
            Py_DECREF(result);
            return NULL;
        }
        Py_INCREF(Py_True);
        PyTuple_SetItem(result, 0, Py_True);
        PyTuple_SetItem(result, 1, solution);
    } else if (res == l_False) {
        Py_INCREF(Py_False);
        PyTuple_SetItem(result, 0, Py_False);
        Py_INCREF(Py_None);
        PyTuple_SetItem(result, 1, Py_None);
    } else if (res == l_Undef) {
        Py_DECREF(result);
        return PyErr_SetFromErrno(outofconflerr);
    }

    return result;
}

/*************************** Method definitions *************************/

static PyMethodDef module_methods[] = {
    //{"solve",     (PyCFunction) full_solve,  METH_VARARGS | METH_KEYWORDS, "my new solver stuff"},
    {NULL,        NULL, 0, NULL}  /* sentinel */
};

static PyMethodDef Solver_methods[] = {
    {"solve",     (PyCFunction) solve,       METH_VARARGS | METH_KEYWORDS, "solves the system"},
    {"add_clause",(PyCFunction) add_clause,  METH_VARARGS | METH_KEYWORDS, "adds a clause to the system"},
    {"add_xor_clause",(PyCFunction) add_xor_clause,  METH_VARARGS | METH_KEYWORDS, "adds an XOR clause to the system"},
    {NULL,        NULL, 0, NULL}  /* sentinel */
};

#ifdef IS_PY3K
static PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    MODULE_NAME,      // m_name
    MODULE_DOC,       // m_doc
    -1,               // m_size
    module_methods,   // m_methods
    NULL,             // m_reload
    NULL,             // m_traverse
    NULL,             // m_clear
    NULL              // m_free
};

#endif

static void
Solver_dealloc(Solver* self)
{
    delete self->cmsat;
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *
Solver_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Solver *self;

    self = (Solver *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->cmsat = setup_solver(args, kwds);
        if (self->cmsat == NULL) {
            Py_DECREF(self);
            return NULL;
        }
    }

    return (PyObject *)self;
}

static int
Solver_init(Solver *self, PyObject *args, PyObject *kwds)
{
    self->cmsat = setup_solver(args, kwds);
    if (!self->cmsat) {
        return -1;
    }
    return 0;
}

static PyMemberDef Solver_members[] = {
    /*{"first", T_OBJECT_EX, offsetof(Noddy, first), 0,
     "first name"},
    {"last", T_OBJECT_EX, offsetof(Noddy, last), 0,
     "last name"},
    {"number", T_INT, offsetof(Noddy, number), 0,
     "noddy number"},*/
    {NULL, 0, 0, 0, NULL}  /* Sentinel */
};

static const char solver_create_docstring[] = "Create Solver object.\n"
"Supported arguments: verbose, clause_limit, threads.\n"
"   'verbose' -- integer. 0: nothing printed. 15: very verbose. Default: 0\n"
"   'confl_limit' -- integer. Abort after this many conflicts. Default: never abort.\n"
"   'threads' -- integer. Number of threads to use. Default: 1"
;

static PyTypeObject pycryptosat_SolverType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pycryptosat.Solver",             /*tp_name*/
    sizeof(Solver),             /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)Solver_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    solver_create_docstring,           /* tp_doc */
    0,                     /* tp_traverse */
    0,                     /* tp_clear */
    0,                     /* tp_richcompare */
    0,                     /* tp_weaklistoffset */
    0,                     /* tp_iter */
    0,                     /* tp_iternext */
    Solver_methods,             /* tp_methods */
    Solver_members,            /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)Solver_init,      /* tp_init */
    0,                         /* tp_alloc */
    Solver_new,                 /* tp_new */
};

#ifndef PyMODINIT_FUNC  /* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif

#ifdef IS_PY3K
#define INIT_ERROR NULL
#define INIT_OK(module) (module)
#else
#define INIT_ERROR
#define INIT_OK(module)
#define PyInit_pycryptosat initpycryptosat
#endif

PyMODINIT_FUNC
PyInit_pycryptosat(void)
{
    PyObject* m;

    pycryptosat_SolverType.tp_new = PyType_GenericNew;
    if (PyType_Ready(&pycryptosat_SolverType) < 0)
        return INIT_ERROR;

#ifdef IS_PY3K
    m = PyModule_Create(&module_def);
#else
    m = Py_InitModule3(MODULE_NAME, module_methods, MODULE_DOC);
#endif

    Py_INCREF(&pycryptosat_SolverType);
    PyModule_AddObject(m, "Solver", (PyObject *)&pycryptosat_SolverType);
    PyModule_AddObject(m, "__version__", PyUnicode_FromString(SATSolver::get_version()));

    outofconflerr = PyErr_NewExceptionWithDoc(const_cast<char *>("Solver.OutOfConflicts"), const_cast<char *>("Ran out of the number of conflicts"), NULL, NULL);
    Py_INCREF(outofconflerr);
    PyModule_AddObject(m, "OutOfConflicts",  outofconflerr);

    return INIT_OK(m);
}
