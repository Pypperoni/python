#include "Python.h"

/* The interpreter's recursion limit */

#ifndef Py_DEFAULT_RECURSION_LIMIT
#define Py_DEFAULT_RECURSION_LIMIT 1000
#endif
static int recursion_limit = Py_DEFAULT_RECURSION_LIMIT;
int _Py_CheckRecursionLimit = Py_DEFAULT_RECURSION_LIMIT;

int
Py_GetRecursionLimit(void)
{
    return recursion_limit;
}

void
Py_SetRecursionLimit(int new_limit)
{
    recursion_limit = new_limit;
    _Py_CheckRecursionLimit = recursion_limit;
}

/* the macro Py_EnterRecursiveCall() only calls _Py_CheckRecursiveCall()
   if the recursion_depth reaches _Py_CheckRecursionLimit.
   If USE_STACKCHECK, the macro decrements _Py_CheckRecursionLimit
   to guarantee that _Py_CheckRecursiveCall() is regularly called.
   Without USE_STACKCHECK, there is no need for this. */
int
_Py_CheckRecursiveCall(char *where)
{
    PyThreadState *tstate = PyThreadState_GET();

#ifdef USE_STACKCHECK
    if (PyOS_CheckStack()) {
        --tstate->recursion_depth;
        PyErr_SetString(PyExc_MemoryError, "Stack overflow");
        return -1;
    }
#endif
    if (tstate->recursion_depth > recursion_limit) {
        --tstate->recursion_depth;
        PyErr_Format(PyExc_RuntimeError,
                     "maximum recursion depth exceeded%s",
                     where);
        return -1;
    }
    _Py_CheckRecursionLimit = recursion_limit;
    return 0;
}

#ifdef WITH_THREAD

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include "pythread.h"

static PyThread_type_lock interpreter_lock = 0; /* This is the GIL */
static PyThread_type_lock pending_lock = 0; /* for pending calls */
static long main_thread = 0;

int
PyEval_ThreadsInitialized(void)
{
    return interpreter_lock != 0;
}

void
PyEval_InitThreads(void)
{
    if (interpreter_lock)
        return;
    interpreter_lock = PyThread_allocate_lock();
    PyThread_acquire_lock(interpreter_lock, 1);
    main_thread = PyThread_get_thread_ident();
}

void
PyEval_AcquireLock(void)
{
    PyThread_acquire_lock(interpreter_lock, 1);
}

void
PyEval_ReleaseLock(void)
{
    PyThread_release_lock(interpreter_lock);
}

void
PyEval_AcquireThread(PyThreadState *tstate)
{
    if (tstate == NULL)
        Py_FatalError("PyEval_AcquireThread: NULL new thread state");
    /* Check someone has called PyEval_InitThreads() to create the lock */
    assert(interpreter_lock);
    PyThread_acquire_lock(interpreter_lock, 1);
    if (PyThreadState_Swap(tstate) != NULL)
        Py_FatalError(
            "PyEval_AcquireThread: non-NULL old thread state");
}

void
PyEval_ReleaseThread(PyThreadState *tstate)
{
    if (tstate == NULL)
        Py_FatalError("PyEval_ReleaseThread: NULL thread state");
    if (PyThreadState_Swap(NULL) != tstate)
        Py_FatalError("PyEval_ReleaseThread: wrong thread state");
    PyThread_release_lock(interpreter_lock);
}

/* This function is called from PyOS_AfterFork to ensure that newly
   created child processes don't hold locks referring to threads which
   are not running in the child process.  (This could also be done using
   pthread_atfork mechanism, at least for the pthreads implementation.) */

void
PyEval_ReInitThreads(void)
{
    PyObject *threading, *result;
    PyThreadState *tstate;

    if (!interpreter_lock)
        return;
    /*XXX Can't use PyThread_free_lock here because it does too
      much error-checking.  Doing this cleanly would require
      adding a new function to each thread_*.h.  Instead, just
      create a new lock and waste a little bit of memory */
    interpreter_lock = PyThread_allocate_lock();
    pending_lock = PyThread_allocate_lock();
    PyThread_acquire_lock(interpreter_lock, 1);
    main_thread = PyThread_get_thread_ident();

    /* Update the threading module with the new state.
     */
    tstate = PyThreadState_GET();
    threading = PyMapping_GetItemString(tstate->interp->modules,
                                        "threading");
    if (threading == NULL) {
        /* threading not imported */
        PyErr_Clear();
        return;
    }
    result = PyObject_CallMethod(threading, "_after_fork", NULL);
    if (result == NULL)
        PyErr_WriteUnraisable(threading);
    else
        Py_DECREF(result);
    Py_DECREF(threading);
}
#endif

/* Functions save_thread and restore_thread are always defined so
   dynamically loaded modules needn't be compiled separately for use
   with and without threads: */

PyThreadState *
PyEval_SaveThread(void)
{
    PyThreadState *tstate = PyThreadState_Swap(NULL);
    if (tstate == NULL)
        Py_FatalError("PyEval_SaveThread: NULL tstate");
#ifdef WITH_THREAD
    if (interpreter_lock)
        PyThread_release_lock(interpreter_lock);
#endif
    return tstate;
}

void
PyEval_RestoreThread(PyThreadState *tstate)
{
    if (tstate == NULL)
        Py_FatalError("PyEval_RestoreThread: NULL tstate");
#ifdef WITH_THREAD
    if (interpreter_lock) {
        int err = errno;
        PyThread_acquire_lock(interpreter_lock, 1);
        errno = err;
    }
#endif
    PyThreadState_Swap(tstate);
}

int _Py_CheckInterval = 100;
volatile int _Py_Ticker = 0; /* so that we hit a "tick" first thing */

#ifdef WITH_THREAD

/* The WITH_THREAD implementation is thread-safe.  It allows
   scheduling to be made from any thread, and even from an executing
   callback.
 */

#define NPENDINGCALLS 32
static struct {
    int (*func)(void *);
    void *arg;
} pendingcalls[NPENDINGCALLS];
static int pendingfirst = 0;
static int pendinglast = 0;
static volatile int pendingcalls_to_do = 1; /* trigger initialization of lock */
static char pendingbusy = 0;

int
Py_AddPendingCall(int (*func)(void *), void *arg)
{
    int i, j, result=0;
    PyThread_type_lock lock = pending_lock;

    /* try a few times for the lock.  Since this mechanism is used
     * for signal handling (on the main thread), there is a (slim)
     * chance that a signal is delivered on the same thread while we
     * hold the lock during the Py_MakePendingCalls() function.
     * This avoids a deadlock in that case.
     * Note that signals can be delivered on any thread.  In particular,
     * on Windows, a SIGINT is delivered on a system-created worker
     * thread.
     * We also check for lock being NULL, in the unlikely case that
     * this function is called before any bytecode evaluation takes place.
     */
    if (lock != NULL) {
        for (i = 0; i<100; i++) {
            if (PyThread_acquire_lock(lock, NOWAIT_LOCK))
                break;
        }
        if (i == 100)
            return -1;
    }

    i = pendinglast;
    j = (i + 1) % NPENDINGCALLS;
    if (j == pendingfirst) {
        result = -1; /* Queue full */
    } else {
        pendingcalls[i].func = func;
        pendingcalls[i].arg = arg;
        pendinglast = j;
    }
    /* signal main loop */
    _Py_Ticker = 0;
    pendingcalls_to_do = 1;
    if (lock != NULL)
        PyThread_release_lock(lock);
    return result;
}

int
Py_MakePendingCalls(void)
{
    int i;
    int r = 0;

    if (!pending_lock) {
        /* initial allocation of the lock */
        pending_lock = PyThread_allocate_lock();
        if (pending_lock == NULL)
            return -1;
    }

    /* only service pending calls on main thread */
    if (main_thread && PyThread_get_thread_ident() != main_thread)
        return 0;
    /* don't perform recursive pending calls */
    if (pendingbusy)
        return 0;
    pendingbusy = 1;
    /* perform a bounded number of calls, in case of recursion */
    for (i=0; i<NPENDINGCALLS; i++) {
        int j;
        int (*func)(void *);
        void *arg = NULL;

        /* pop one item off the queue while holding the lock */
        PyThread_acquire_lock(pending_lock, WAIT_LOCK);
        j = pendingfirst;
        if (j == pendinglast) {
            func = NULL; /* Queue empty */
        } else {
            func = pendingcalls[j].func;
            arg = pendingcalls[j].arg;
            pendingfirst = (j + 1) % NPENDINGCALLS;
        }
        pendingcalls_to_do = pendingfirst != pendinglast;
        PyThread_release_lock(pending_lock);
        /* having released the lock, perform the callback */
        if (func == NULL)
            break;
        r = func(arg);
        if (r)
            break;
    }
    pendingbusy = 0;
    return r;
}

#else /* if ! defined WITH_THREAD */

#define NPENDINGCALLS 32
static struct {
    int (*func)(void *);
    void *arg;
} pendingcalls[NPENDINGCALLS];
static volatile int pendingfirst = 0;
static volatile int pendinglast = 0;
static volatile int pendingcalls_to_do = 0;

int
Py_AddPendingCall(int (*func)(void *), void *arg)
{
    static volatile int busy = 0;
    int i, j;
    /* XXX Begin critical section */
    if (busy)
        return -1;
    busy = 1;
    i = pendinglast;
    j = (i + 1) % NPENDINGCALLS;
    if (j == pendingfirst) {
        busy = 0;
        return -1; /* Queue full */
    }
    pendingcalls[i].func = func;
    pendingcalls[i].arg = arg;
    pendinglast = j;

    _Py_Ticker = 0;
    pendingcalls_to_do = 1; /* Signal main loop */
    busy = 0;
    /* XXX End critical section */
    return 0;
}

int
Py_MakePendingCalls(void)
{
    static int busy = 0;
    if (busy)
        return 0;
    busy = 1;
    pendingcalls_to_do = 0;
    for (;;) {
        int i;
        int (*func)(void *);
        void *arg;
        i = pendingfirst;
        if (i == pendinglast)
            break; /* Queue empty */
        func = pendingcalls[i].func;
        arg = pendingcalls[i].arg;
        pendingfirst = (i + 1) % NPENDINGCALLS;
        if (func(arg) < 0) {
            busy = 0;
            pendingcalls_to_do = 1; /* We're not done yet */
            return -1;
        }
    }
    busy = 0;
    return 0;
}

#endif /* WITH_THREAD */

int
PyEval_GetRestricted(void)
{
    return 0;
}

/* Extract a slice index from a PyInt or PyLong or an object with the
   nb_index slot defined, and store in *pi.
   Silently reduce values larger than PY_SSIZE_T_MAX to PY_SSIZE_T_MAX,
   and silently boost values less than -PY_SSIZE_T_MAX-1 to -PY_SSIZE_T_MAX-1.
   Return 0 on error, 1 on success.
*/
/* Note:  If v is NULL, return success without storing into *pi.  This
   is because_PyEval_SliceIndex() is called by apply_slice(), which can be
   called by the SLICE opcode with v and/or w equal to NULL.
*/
int
_PyEval_SliceIndex(PyObject *v, Py_ssize_t *pi)
{
    if (v != NULL) {
        Py_ssize_t x;
        if (PyInt_Check(v)) {
            /* XXX(nnorwitz): I think PyInt_AS_LONG is correct,
               however, it looks like it should be AsSsize_t.
               There should be a comment here explaining why.
            */
            x = PyInt_AS_LONG(v);
        }
        else if (PyIndex_Check(v)) {
            x = PyNumber_AsSsize_t(v, NULL);
            if (x == -1 && PyErr_Occurred())
                return 0;
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "slice indices must be integers or "
                            "None or have an __index__ method");
            return 0;
        }
        *pi = x;
    }
    return 1;
}


int
Py_FlushLine(void)
{
    PyObject *f = PySys_GetObject("stdout");
    if (f == NULL)
        return 0;
    if (!PyFile_SoftSpace(f, 0))
        return 0;
    return PyFile_WriteString("\n", f);
}

/* External interface to call any callable object.
   The arg must be a tuple or NULL.  The kw must be a dict or NULL. */

PyObject *
PyEval_CallObjectWithKeywords(PyObject *func, PyObject *arg, PyObject *kw)
{
    PyObject *result;

    if (arg == NULL) {
        arg = PyTuple_New(0);
        if (arg == NULL)
            return NULL;
    }
    else if (!PyTuple_Check(arg)) {
        PyErr_SetString(PyExc_TypeError,
                        "argument list must be a tuple");
        return NULL;
    }
    else
        Py_INCREF(arg);

    if (kw != NULL && !PyDict_Check(kw)) {
        PyErr_SetString(PyExc_TypeError,
                        "keyword list must be a dictionary");
        Py_DECREF(arg);
        return NULL;
    }

    result = PyObject_Call(func, arg, kw);
    Py_DECREF(arg);
    return result;
}

int Py_OptimizeFlag = 1;

PyObject *
_Py_Mangle(PyObject *privateobj, PyObject *ident)
{
    /* Name mangling: __private becomes _classname__private.
       This is independent from how the name is used. */
    const char *p, *name = PyString_AsString(ident);
    char *buffer;
    size_t nlen, plen;
    if (privateobj == NULL || !PyString_Check(privateobj) ||
        name == NULL || name[0] != '_' || name[1] != '_') {
        Py_INCREF(ident);
        return ident;
    }
    p = PyString_AsString(privateobj);
    nlen = strlen(name);
    /* Don't mangle __id__ or names with dots.
       The only time a name with a dot can occur is when
       we are compiling an import statement that has a
       package name.
       TODO(jhylton): Decide whether we want to support
       mangling of the module name, e.g. __M.X.
    */
    if ((name[nlen-1] == '_' && name[nlen-2] == '_')
        || strchr(name, '.')) {
        Py_INCREF(ident);
        return ident; /* Don't mangle __whatever__ */
    }
    /* Strip leading underscores from class name */
    while (*p == '_')
        p++;
    if (*p == '\0') {
        Py_INCREF(ident);
        return ident; /* Don't mangle if class is just underscores */
    }
    plen = strlen(p);

    if (plen + nlen >= PY_SSIZE_T_MAX - 1) {
        PyErr_SetString(PyExc_OverflowError,
                        "private identifier too large to be mangled");
        return NULL;
    }

    ident = PyString_FromStringAndSize(NULL, 1 + nlen + plen);
    if (!ident)
        return 0;
    /* ident = "_" + p[:plen] + name # i.e. 1+plen+nlen bytes */
    buffer = PyString_AS_STRING(ident);
    buffer[0] = '_';
    strncpy(buffer+1, p, plen);
    strcpy(buffer+1+plen, name);
    return ident;
}

const char *
PyEval_GetFuncName(PyObject *func)
{
    if (PyMethod_Check(func))
        return PyEval_GetFuncName(PyMethod_GET_FUNCTION(func));
    else if (PyCFunction_Check(func))
        return ((PyCFunctionObject*)func)->m_ml->ml_name;
    else if (PyClass_Check(func))
        return PyString_AsString(((PyClassObject*)func)->cl_name);
    else if (PyInstance_Check(func)) {
        return PyString_AsString(
            ((PyInstanceObject*)func)->in_class->cl_name);
    } else {
        PyObject* t = PyObject_Repr(func);
        const char* r = PyString_AS_STRING(t);
        Py_DECREF(t);
        return r; // func->ob_type->tp_name;
    }
}

const char *
PyEval_GetFuncDesc(PyObject *func)
{
    if (PyMethod_Check(func))
        return "()";
    else if (PyCFunction_Check(func))
        return "()";
    else if (PyClass_Check(func))
        return " constructor";
    else if (PyInstance_Check(func)) {
        return " instance";
    } else {
        return " object";
    }
}
