
/* Python interpreter top-level routines, including init/exit */

#include "Python.h"

#undef Yield /* undefine macro conflicting with winbase.h */
#include "marshal.h"
#include "abstract.h"

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#ifdef MS_WINDOWS
#include "malloc.h" /* for alloca */
#endif

#ifdef HAVE_LANGINFO_H
#include <locale.h>
#include <langinfo.h>
#endif

#ifdef MS_WINDOWS
#undef BYTE
#include "windows.h"
#endif

#ifndef Py_REF_DEBUG
#define PRINT_TOTAL_REFS()
#else /* Py_REF_DEBUG */
#define PRINT_TOTAL_REFS() fprintf(stderr,                              \
                   "[%" PY_FORMAT_SIZE_T "d refs]\n",                   \
                   _Py_GetRefTotal())
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern char *Py_GetPath(void);

/* Forward */
static void initmain(void);
static void initsite(void);
static void initsigs(void);
static void wait_for_thread_shutdown(void);
static void call_sys_exitfunc(void);
static void call_ll_exitfuncs(void);
extern void _PyUnicode_Init(void);
extern void _PyUnicode_Fini(void);

#ifdef WITH_THREAD
extern void _PyGILState_Init(PyInterpreterState *, PyThreadState *);
extern void _PyGILState_Fini(void);
#endif /* WITH_THREAD */

int Py_DebugFlag; /* Needed by parser.c */
int Py_VerboseFlag; /* Needed by import.c */
int Py_InteractiveFlag; /* Needed by Py_FdIsInteractive() below */
int Py_InspectFlag; /* Needed to determine whether to exit at SystemExit */
int Py_NoSiteFlag; /* Suppress 'import site' */
int Py_BytesWarningFlag; /* Warn on str(bytes) and str(buffer) */
int Py_DontWriteBytecodeFlag; /* Suppress writing bytecode files (*.py[co]) */
int Py_UseClassExceptionsFlag = 1; /* Needed by bltinmodule.c: deprecated */
int Py_FrozenFlag; /* Needed by getpath.c */
int Py_UnicodeFlag = 0; /* Needed by compile.c */
int Py_IgnoreEnvironmentFlag; /* e.g. PYTHONPATH, PYTHONHOME */
/* _XXX Py_QnewFlag should go away in 2.3.  It's true iff -Qnew is passed,
  on the command line, and is used in 2.2 by ceval.c to make all "/" divisions
  true divisions (which they will be in 2.3). */
int _Py_QnewFlag = 0;
int Py_NoUserSiteDirectory = 0; /* for -s and site.py */
int Py_HashRandomizationFlag = 0; /* for -R and PYTHONHASHSEED */


/* Hack to force loading of object files */
int (*_PyOS_mystrnicmp_hack)(const char *, const char *, Py_ssize_t) = \
    PyOS_mystrnicmp; /* Python/pystrcmp.o */

/* PyModule_GetWarningsModule is no longer necessary as of 2.6
since _warnings is builtin.  This API should not be used. */
PyObject *
PyModule_GetWarningsModule(void)
{
    return Py_ImportBuiltin("warnings");
}

static int initialized = 0;

/* API to access the initialized flag -- useful for esoteric use */

int
Py_IsInitialized(void)
{
    return initialized;
}

/* Global initializations.  Can be undone by Py_Finalize().  Don't
   call this twice without an intervening Py_Finalize() call.  When
   initializations fail, a fatal error is issued and the function does
   not return.  On return, the first thread and interpreter state have
   been created.

   Locking: you must hold the interpreter lock while calling this.
   (If the lock has not yet been initialized, that's equivalent to
   having the lock, but you cannot use multiple threads.)

*/

static int
add_flag(int flag, const char *envs)
{
    int env = atoi(envs);
    if (flag < env)
        flag = env;
    if (flag < 1)
        flag = 1;
    return flag;
}

void
Py_InitializeEx(int install_sigs)
{
    PyInterpreterState *interp;
    PyThreadState *tstate;
    PyObject *bimod, *sysmod;
    char *p;
    char *icodeset = NULL; /* On Windows, input codeset may theoretically
                              differ from output codeset. */
    char *codeset = NULL;
    char *errors = NULL;
    int free_codeset = 0;
    int overridden = 0;
    PyObject *sys_stream, *sys_isatty;
#if defined(Py_USING_UNICODE) && defined(HAVE_LANGINFO_H) && defined(CODESET)
    char *saved_locale, *loc_codeset;
#endif
#ifdef MS_WINDOWS
    char ibuf[128];
    char buf[128];
#endif
    extern void _Py_ReadyTypes(void);

    if (initialized)
        return;
    initialized = 1;

    if ((p = Py_GETENV("PYTHONDEBUG")) && *p != '\0')
        Py_DebugFlag = add_flag(Py_DebugFlag, p);
    if ((p = Py_GETENV("PYTHONVERBOSE")) && *p != '\0')
        Py_VerboseFlag = add_flag(Py_VerboseFlag, p);
    if ((p = Py_GETENV("PYTHONOPTIMIZE")) && *p != '\0')
        Py_OptimizeFlag = add_flag(Py_OptimizeFlag, p);
    if ((p = Py_GETENV("PYTHONDONTWRITEBYTECODE")) && *p != '\0')
        Py_DontWriteBytecodeFlag = add_flag(Py_DontWriteBytecodeFlag, p);
    /* The variable is only tested for existence here; _PyRandom_Init will
       check its value further. */
    if ((p = Py_GETENV("PYTHONHASHSEED")) && *p != '\0')
        Py_HashRandomizationFlag = add_flag(Py_HashRandomizationFlag, p);

    _PyRandom_Init();

    interp = PyInterpreterState_New();
    if (interp == NULL)
        Py_FatalError("Py_Initialize: can't make first interpreter");

    tstate = PyThreadState_New(interp);
    if (tstate == NULL)
        Py_FatalError("Py_Initialize: can't make first thread");
    (void) PyThreadState_Swap(tstate);

    _Py_ReadyTypes();

    if (!_PyInt_Init())
        Py_FatalError("Py_Initialize: can't init ints");

    if (!_PyLong_Init())
        Py_FatalError("Py_Initialize: can't init longs");

    if (!PyByteArray_Init())
        Py_FatalError("Py_Initialize: can't init bytearray");

    _PyFloat_Init();

    interp->modules = PyDict_New();
    if (interp->modules == NULL)
        Py_FatalError("Py_Initialize: can't make modules dictionary");
    interp->modules_reloading = PyDict_New();
    if (interp->modules_reloading == NULL)
        Py_FatalError("Py_Initialize: can't make modules_reloading dictionary");

#ifdef Py_USING_UNICODE
    /* Init Unicode implementation; relies on the codec registry */
    _PyUnicode_Init();
#endif

    bimod = _PyBuiltin_Init();
    if (bimod == NULL)
        Py_FatalError("Py_Initialize: can't initialize __builtin__");
    interp->builtins = PyModule_GetDict(bimod);
    if (interp->builtins == NULL)
        Py_FatalError("Py_Initialize: can't initialize builtins dict");
    Py_INCREF(interp->builtins);

    sysmod = _PySys_Init();
    if (sysmod == NULL)
        Py_FatalError("Py_Initialize: can't initialize sys");
    interp->sysdict = PyModule_GetDict(sysmod);
    if (interp->sysdict == NULL)
        Py_FatalError("Py_Initialize: can't initialize sys dict");
    Py_INCREF(interp->sysdict);
    PySys_SetPath(Py_GetPath());
    PyDict_SetItemString(interp->sysdict, "modules",
                         interp->modules);

    /* initialize builtin exceptions */
    _PyExc_Init();

    /* phase 2 of builtins */
    if (install_sigs)
        initsigs(); /* Signal handling stuff, including initintr() */

    /* Initialize warnings. */
    _PyWarnings_Init();
    if (PySys_HasWarnOptions()) {
        PyObject *warnings_module = Py_ImportBuiltin("warnings");
        if (!warnings_module)
            PyErr_Clear();
        Py_XDECREF(warnings_module);
    }

    initmain(); /* Module __main__ */

    /* auto-thread-state API, if available */
#ifdef WITH_THREAD
    _PyGILState_Init(interp, tstate);
#endif /* WITH_THREAD */

    if (!Py_NoSiteFlag)
        initsite(); /* Module site */

    if ((p = Py_GETENV("PYTHONIOENCODING")) && *p != '\0') {
        p = icodeset = codeset = strdup(p);
        free_codeset = 1;
        errors = strchr(p, ':');
        if (errors) {
            *errors = '\0';
            errors++;
        }
        overridden = 1;
    }

#ifdef MS_WINDOWS
    if (!overridden) {
        icodeset = ibuf;
        codeset = buf;
        sprintf(ibuf, "cp%d", GetConsoleCP());
        sprintf(buf, "cp%d", GetConsoleOutputCP());
    }
#endif

    if (codeset) {
        sys_stream = PySys_GetObject("stdin");
        sys_isatty = PyObject_CallMethod(sys_stream, "isatty", "");
        if (!sys_isatty)
            PyErr_Clear();
        if ((overridden ||
             (sys_isatty && PyObject_IsTrue(sys_isatty))) &&
           PyFile_Check(sys_stream)) {
            if (!PyFile_SetEncodingAndErrors(sys_stream, icodeset, errors))
                Py_FatalError("Cannot set codeset of stdin");
        }
        Py_XDECREF(sys_isatty);

        sys_stream = PySys_GetObject("stdout");
        sys_isatty = PyObject_CallMethod(sys_stream, "isatty", "");
        if (!sys_isatty)
            PyErr_Clear();
        if ((overridden ||
             (sys_isatty && PyObject_IsTrue(sys_isatty))) &&
           PyFile_Check(sys_stream)) {
            if (!PyFile_SetEncodingAndErrors(sys_stream, codeset, errors))
                Py_FatalError("Cannot set codeset of stdout");
        }
        Py_XDECREF(sys_isatty);

        sys_stream = PySys_GetObject("stderr");
        sys_isatty = PyObject_CallMethod(sys_stream, "isatty", "");
        if (!sys_isatty)
            PyErr_Clear();
        if((overridden ||
            (sys_isatty && PyObject_IsTrue(sys_isatty))) &&
           PyFile_Check(sys_stream)) {
            if (!PyFile_SetEncodingAndErrors(sys_stream, codeset, errors))
                Py_FatalError("Cannot set codeset of stderr");
        }
        Py_XDECREF(sys_isatty);

        if (free_codeset)
            free(codeset);
    }
}

void
Py_Initialize(void)
{
    Py_InitializeEx(1);
}


#ifdef COUNT_ALLOCS
extern void dump_counts(FILE*);
#endif

/* Undo the effect of Py_Initialize().

   Beware: if multiple interpreter and/or thread states exist, these
   are not wiped out; only the current thread and interpreter state
   are deleted.  But since everything else is deleted, those other
   interpreter and thread states should no longer be used.

   (XXX We should do better, e.g. wipe out all interpreters and
   threads.)

   Locking: as above.

*/

void
Py_Finalize(void)
{
    PyInterpreterState *interp;
    PyThreadState *tstate;

    if (!initialized)
        return;

    wait_for_thread_shutdown();

    /* The interpreter is still entirely intact at this point, and the
     * exit funcs may be relying on that.  In particular, if some thread
     * or exit func is still waiting to do an import, the import machinery
     * expects Py_IsInitialized() to return true.  So don't say the
     * interpreter is uninitialized until after the exit funcs have run.
     * Note that Threading.py uses an exit func to do a join on all the
     * threads created thru it, so this also protects pending imports in
     * the threads created via Threading.
     */
    call_sys_exitfunc();
    initialized = 0;

    /* Get current thread state and interpreter pointer */
    tstate = PyThreadState_GET();
    interp = tstate->interp;

    /* Disable signal handling */
    PyOS_FiniInterrupts();

    /* Clear type lookup cache */
    PyType_ClearCache();

    /* Collect garbage.  This may call finalizers; it's nice to call these
     * before all modules are destroyed.
     * XXX If a __del__ or weakref callback is triggered here, and tries to
     * XXX import a module, bad things can happen, because Python no
     * XXX longer believes it's initialized.
     * XXX     Fatal Python error: Interpreter not initialized (version mismatch?)
     * XXX is easy to provoke that way.  I've also seen, e.g.,
     * XXX     Exception exceptions.ImportError: 'No module named sha'
     * XXX         in <function callback at 0x008F5718> ignored
     * XXX but I'm unclear on exactly how that one happens.  In any case,
     * XXX I haven't seen a real-life report of either of these.
     */
    PyGC_Collect();
#ifdef COUNT_ALLOCS
    /* With COUNT_ALLOCS, it helps to run GC multiple times:
       each collection might release some types from the type
       list, so they become garbage. */
    while (PyGC_Collect() > 0)
        /* nothing */;
#endif

    /* Collect final garbage.  This disposes of cycles created by
     * new-style class definitions, for example.
     * XXX This is disabled because it caused too many problems.  If
     * XXX a __del__ or weakref callback triggers here, Python code has
     * XXX a hard time running, because even the sys module has been
     * XXX cleared out (sys.stdout is gone, sys.excepthook is gone, etc).
     * XXX One symptom is a sequence of information-free messages
     * XXX coming from threads (if a __del__ or callback is invoked,
     * XXX other threads can execute too, and any exception they encounter
     * XXX triggers a comedy of errors as subsystem after subsystem
     * XXX fails to find what it *expects* to find in sys to help report
     * XXX the exception and consequent unexpected failures).  I've also
     * XXX seen segfaults then, after adding print statements to the
     * XXX Python code getting called.
     */
#if 0
    PyGC_Collect();
#endif

    /* Debugging stuff */
#ifdef COUNT_ALLOCS
    dump_counts(stdout);
#endif

    PRINT_TOTAL_REFS();

#ifdef Py_TRACE_REFS
    /* Display all objects still alive -- this can invoke arbitrary
     * __repr__ overrides, so requires a mostly-intact interpreter.
     * Alas, a lot of stuff may still be alive now that will be cleaned
     * up later.
     */
    if (Py_GETENV("PYTHONDUMPREFS"))
        _Py_PrintReferences(stderr);
#endif /* Py_TRACE_REFS */

    /* Clear interpreter state */
    PyInterpreterState_Clear(interp);

    /* Now we decref the exception classes.  After this point nothing
       can raise an exception.  That's okay, because each Fini() method
       below has been checked to make sure no exceptions are ever
       raised.
    */

    _PyExc_Fini();

    /* Cleanup auto-thread-state */
#ifdef WITH_THREAD
    _PyGILState_Fini();
#endif /* WITH_THREAD */

    /* Delete current thread */
    PyThreadState_Swap(NULL);
    PyInterpreterState_Delete(interp);

    /* Sundry finalizers */
    PyMethod_Fini();
    PyCFunction_Fini();
    PyTuple_Fini();
    PyList_Fini();
    PySet_Fini();
    PyString_Fini();
    PyByteArray_Fini();
    PyInt_Fini();
    PyFloat_Fini();
    PyDict_Fini();
    _PyRandom_Fini();

#ifdef Py_USING_UNICODE
    /* Cleanup Unicode implementation */
    _PyUnicode_Fini();
#endif

    /* XXX Still allocated:
       - various static ad-hoc pointers to interned strings
       - int and float free list blocks
       - whatever various modules and libraries allocate
    */

#ifdef Py_TRACE_REFS
    /* Display addresses (& refcnts) of all objects still alive.
     * An address can be used to find the repr of the object, printed
     * above by _Py_PrintReferences.
     */
    if (Py_GETENV("PYTHONDUMPREFS"))
        _Py_PrintReferenceAddresses(stderr);
#endif /* Py_TRACE_REFS */
#ifdef PYMALLOC_DEBUG
    if (Py_GETENV("PYTHONMALLOCSTATS"))
        _PyObject_DebugMallocStats();
#endif

    call_ll_exitfuncs();
}

/* Create and initialize a new interpreter and thread, and return the
   new thread.  This requires that Py_Initialize() has been called
   first.

   Unsuccessful initialization yields a NULL pointer.  Note that *no*
   exception information is available even in this case -- the
   exception information is held in the thread, and there is no
   thread.

   Locking: as above.

*/

PyThreadState *
Py_NewInterpreter(void)
{
    PyInterpreterState *interp;
    PyThreadState *tstate, *save_tstate;
    PyObject *bimod, *sysmod;

    if (!initialized)
        Py_FatalError("Py_NewInterpreter: call Py_Initialize first");

    interp = PyInterpreterState_New();
    if (interp == NULL)
        return NULL;

    tstate = PyThreadState_New(interp);
    if (tstate == NULL) {
        PyInterpreterState_Delete(interp);
        return NULL;
    }

    save_tstate = PyThreadState_Swap(tstate);

    /* XXX The following is lax in error checking */

    interp->modules = PyDict_New();
    interp->modules_reloading = PyDict_New();

    bimod = Py_ImportBuiltin("__builtin__");
    if (bimod != NULL) {
        interp->builtins = PyModule_GetDict(bimod);
        if (interp->builtins == NULL)
            goto handle_error;
        Py_INCREF(interp->builtins);
    }
    sysmod = Py_ImportBuiltin("sys");
    if (bimod != NULL && sysmod != NULL) {
        interp->sysdict = PyModule_GetDict(sysmod);
        if (interp->sysdict == NULL)
            goto handle_error;
        Py_INCREF(interp->sysdict);
        PySys_SetPath(Py_GetPath());
        PyDict_SetItemString(interp->sysdict, "modules",
                             interp->modules);
        initmain();
        if (!Py_NoSiteFlag)
            initsite();
    }

    if (!PyErr_Occurred())
        return tstate;

handle_error:
    /* Oops, it didn't work.  Undo it all. */

    PyErr_Print();
    PyThreadState_Clear(tstate);
    PyThreadState_Swap(save_tstate);
    PyThreadState_Delete(tstate);
    PyInterpreterState_Delete(interp);

    return NULL;
}

/* Delete an interpreter and its last thread.  This requires that the
   given thread state is current, that the thread has no remaining
   frames, and that it is its interpreter's only remaining thread.
   It is a fatal error to violate these constraints.

   (Py_Finalize() doesn't have these constraints -- it zaps
   everything, regardless.)

   Locking: as above.

*/

void
Py_EndInterpreter(PyThreadState *tstate)
{
    PyInterpreterState *interp = tstate->interp;

    if (tstate != PyThreadState_GET())
        Py_FatalError("Py_EndInterpreter: thread is not current");
    if (tstate->frame != NULL)
        Py_FatalError("Py_EndInterpreter: thread still has a frame");
    if (tstate != interp->tstate_head || tstate->next != NULL)
        Py_FatalError("Py_EndInterpreter: not the last thread");

    PyInterpreterState_Clear(interp);
    PyThreadState_Swap(NULL);
    PyInterpreterState_Delete(interp);
}

static char *progname = "python";

void
Py_SetProgramName(char *pn)
{
    if (pn && *pn)
        progname = pn;
}

char *
Py_GetProgramName(void)
{
    return progname;
}

static char *default_home = NULL;

void
Py_SetPythonHome(char *home)
{
    default_home = home;
}

char *
Py_GetPythonHome(void)
{
    char *home = default_home;
    if (home == NULL && !Py_IgnoreEnvironmentFlag)
        home = Py_GETENV("PYTHONHOME");
    return home;
}

/* Create __main__ module */

static void
initmain(void)
{
    PyObject *m, *d;
    m = PyImport_AddModule("__main__");
    if (m == NULL)
        Py_FatalError("can't create __main__ module");
    d = PyModule_GetDict(m);
    if (PyDict_GetItemString(d, "__builtins__") == NULL) {
        PyObject *bimod = Py_GetBuiltinModule();
        if (bimod == NULL ||
            PyDict_SetItemString(d, "__builtins__", bimod) != 0)
            Py_FatalError("can't add __builtins__ to __main__");
        Py_XDECREF(bimod);
    }
}

/* Import the site module (not into __main__ though) */

static void
initsite(void)
{
    PyObject *m;
    m = Py_ImportBuiltin("site");
    if (m == NULL) {
        PyErr_Print();
        Py_Finalize();
        exit(1);
    }
    else {
        Py_DECREF(m);
    }
}

#if 0
/* compute parser flags based on compiler flags */
#define PARSER_FLAGS(flags) \
    ((flags) ? ((((flags)->cf_flags & PyCF_DONT_IMPLY_DEDENT) ? \
                  PyPARSE_DONT_IMPLY_DEDENT : 0)) : 0)
#endif
#if 1
/* Keep an example of flags with future keyword support. */
#define PARSER_FLAGS(flags) \
    ((flags) ? ((((flags)->cf_flags & PyCF_DONT_IMPLY_DEDENT) ? \
                  PyPARSE_DONT_IMPLY_DEDENT : 0) \
                | (((flags)->cf_flags & CO_FUTURE_PRINT_FUNCTION) ? \
                   PyPARSE_PRINT_IS_FUNCTION : 0) \
                | (((flags)->cf_flags & CO_FUTURE_UNICODE_LITERALS) ? \
                   PyPARSE_UNICODE_LITERALS : 0) \
                ) : 0)
#endif

static int
parse_syntax_error(PyObject *err, PyObject **message, const char **filename,
                   int *lineno, int *offset, const char **text)
{
    long hold;
    PyObject *v;

    /* old style errors */
    if (PyTuple_Check(err))
        return PyArg_ParseTuple(err, "O(ziiz)", message, filename,
                                lineno, offset, text);

    *message = NULL;

    /* new style errors.  `err' is an instance */
    *message = PyObject_GetAttrString(err, "msg");
    if (!*message)
        goto finally;

    v = PyObject_GetAttrString(err, "filename");
    if (!v)
        goto finally;
    if (v == Py_None) {
        Py_DECREF(v);
        *filename = NULL;
    }
    else {
        *filename = PyString_AsString(v);
        Py_DECREF(v);
        if (!*filename)
            goto finally;
    }

    v = PyObject_GetAttrString(err, "lineno");
    if (!v)
        goto finally;
    hold = PyInt_AsLong(v);
    Py_DECREF(v);
    if (hold < 0 && PyErr_Occurred())
        goto finally;
    *lineno = (int)hold;

    v = PyObject_GetAttrString(err, "offset");
    if (!v)
        goto finally;
    if (v == Py_None) {
        *offset = -1;
        Py_DECREF(v);
    } else {
        hold = PyInt_AsLong(v);
        Py_DECREF(v);
        if (hold < 0 && PyErr_Occurred())
            goto finally;
        *offset = (int)hold;
    }

    v = PyObject_GetAttrString(err, "text");
    if (!v)
        goto finally;
    if (v == Py_None) {
        Py_DECREF(v);
        *text = NULL;
    }
    else {
        *text = PyString_AsString(v);
        Py_DECREF(v);
        if (!*text)
            goto finally;
    }
    return 1;

finally:
    Py_XDECREF(*message);
    return 0;
}

void
PyErr_Print(void)
{
    PyErr_PrintEx(1);
}

static void
print_error_text(PyObject *f, int offset, const char *text)
{
    char *nl;
    if (offset >= 0) {
        if (offset > 0 && offset == strlen(text) && text[offset - 1] == '\n')
            offset--;
        for (;;) {
            nl = strchr(text, '\n');
            if (nl == NULL || nl-text >= offset)
                break;
            offset -= (int)(nl+1-text);
            text = nl+1;
        }
        while (*text == ' ' || *text == '\t') {
            text++;
            offset--;
        }
    }
    PyFile_WriteString("    ", f);
    PyFile_WriteString(text, f);
    if (*text == '\0' || text[strlen(text)-1] != '\n')
        PyFile_WriteString("\n", f);
    if (offset == -1)
        return;
    PyFile_WriteString("    ", f);
    offset--;
    while (offset > 0) {
        PyFile_WriteString(" ", f);
        offset--;
    }
    PyFile_WriteString("^\n", f);
}

static void
handle_system_exit(void)
{
    PyObject *exception, *value, *tb;
    int exitcode = 0;

    if (Py_InspectFlag)
        /* Don't exit if -i flag was given. This flag is set to 0
         * when entering interactive mode for inspecting. */
        return;

    PyErr_Fetch(&exception, &value, &tb);
    if (Py_FlushLine())
        PyErr_Clear();
    fflush(stdout);
    if (value == NULL || value == Py_None)
        goto done;
    if (PyExceptionInstance_Check(value)) {
        /* The error code should be in the `code' attribute. */
        PyObject *code = PyObject_GetAttrString(value, "code");
        if (code) {
            Py_DECREF(value);
            value = code;
            if (value == Py_None)
                goto done;
        }
        /* If we failed to dig out the 'code' attribute,
           just let the else clause below print the error. */
    }
    if (PyInt_Check(value))
        exitcode = (int)PyInt_AsLong(value);
    else {
        PyObject *sys_stderr = PySys_GetObject("stderr");
        if (sys_stderr != NULL && sys_stderr != Py_None) {
            PyFile_WriteObject(value, sys_stderr, Py_PRINT_RAW);
        } else {
            PyObject_Print(value, stderr, Py_PRINT_RAW);
            fflush(stderr);
        }
        PySys_WriteStderr("\n");
        exitcode = 1;
    }
 done:
    /* Restore and clear the exception info, in order to properly decref
     * the exception, value, and traceback.      If we just exit instead,
     * these leak, which confuses PYTHONDUMPREFS output, and may prevent
     * some finalizers from running.
     */
    PyErr_Restore(exception, value, tb);
    PyErr_Clear();
    Py_Exit(exitcode);
    /* NOTREACHED */
}

void
PyErr_PrintEx(int set_sys_last_vars)
{
    PyObject *exception, *v, *tb, *hook;

    if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
        handle_system_exit();
    }
    PyErr_Fetch(&exception, &v, &tb);
    if (exception == NULL)
        return;
    PyErr_NormalizeException(&exception, &v, &tb);
    if (exception == NULL)
        return;
    /* Now we know v != NULL too */
    if (set_sys_last_vars) {
        PySys_SetObject("last_type", exception);
        PySys_SetObject("last_value", v);
        PySys_SetObject("last_traceback", tb);
    }
    hook = PySys_GetObject("excepthook");
    if (hook && hook != Py_None) {
        PyObject *args = PyTuple_Pack(3,
            exception, v, tb ? tb : Py_None);
        PyObject *result = PyEval_CallObject(hook, args);
        if (result == NULL) {
            PyObject *exception2, *v2, *tb2;
            if (PyErr_ExceptionMatches(PyExc_SystemExit)) {
                handle_system_exit();
            }
            PyErr_Fetch(&exception2, &v2, &tb2);
            PyErr_NormalizeException(&exception2, &v2, &tb2);
            /* It should not be possible for exception2 or v2
               to be NULL. However PyErr_Display() can't
               tolerate NULLs, so just be safe. */
            if (exception2 == NULL) {
                exception2 = Py_None;
                Py_INCREF(exception2);
            }
            if (v2 == NULL) {
                v2 = Py_None;
                Py_INCREF(v2);
            }
            if (Py_FlushLine())
                PyErr_Clear();
            fflush(stdout);
            PySys_WriteStderr("Error in sys.excepthook:\n");
            PyErr_Display(exception2, v2, tb2);
            PySys_WriteStderr("\nOriginal exception was:\n");
            PyErr_Display(exception, v, tb);
            Py_DECREF(exception2);
            Py_DECREF(v2);
            Py_XDECREF(tb2);
        }
        Py_XDECREF(result);
        Py_XDECREF(args);
    } else {
        PySys_WriteStderr("sys.excepthook is missing\n");
        PyErr_Display(exception, v, tb);
    }
    Py_XDECREF(exception);
    Py_XDECREF(v);
    Py_XDECREF(tb);
}

void
PyErr_Display(PyObject *exception, PyObject *value, PyObject *tb)
{
    int err = 0;
    PyObject *f = PySys_GetObject("stderr");
    Py_INCREF(value);
    if (f == NULL || f == Py_None)
        fprintf(stderr, "lost sys.stderr\n");
    else {
        if (Py_FlushLine())
            PyErr_Clear();
        fflush(stdout);
        if (err == 0 &&
            PyObject_HasAttrString(value, "print_file_and_line"))
        {
            PyObject *message;
            const char *filename, *text;
            int lineno, offset;
            if (!parse_syntax_error(value, &message, &filename,
                                    &lineno, &offset, &text))
                PyErr_Clear();
            else {
                char buf[10];
                PyFile_WriteString("  File \"", f);
                if (filename == NULL)
                    PyFile_WriteString("<string>", f);
                else
                    PyFile_WriteString(filename, f);
                PyFile_WriteString("\", line ", f);
                PyOS_snprintf(buf, sizeof(buf), "%d", lineno);
                PyFile_WriteString(buf, f);
                PyFile_WriteString("\n", f);
                if (text != NULL)
                    print_error_text(f, offset, text);
                Py_DECREF(value);
                value = message;
                /* Can't be bothered to check all those
                   PyFile_WriteString() calls */
                if (PyErr_Occurred())
                    err = -1;
            }
        }
        if (err) {
            /* Don't do anything else */
        }
        else if (PyExceptionClass_Check(exception)) {
            PyObject* moduleName;
            char* className = PyExceptionClass_Name(exception);
            if (className != NULL) {
                char *dot = strrchr(className, '.');
                if (dot != NULL)
                    className = dot+1;
            }

            moduleName = PyObject_GetAttrString(exception, "__module__");
            if (moduleName == NULL)
                err = PyFile_WriteString("<unknown>", f);
            else {
                char* modstr = PyString_AsString(moduleName);
                if (modstr && strcmp(modstr, "exceptions"))
                {
                    err = PyFile_WriteString(modstr, f);
                    err += PyFile_WriteString(".", f);
                }
                Py_DECREF(moduleName);
            }
            if (err == 0) {
                if (className == NULL)
                      err = PyFile_WriteString("<unknown>", f);
                else
                      err = PyFile_WriteString(className, f);
            }
        }
        else
            err = PyFile_WriteObject(exception, f, Py_PRINT_RAW);
        if (err == 0 && (value != Py_None)) {
            PyObject *s = PyObject_Str(value);
            /* only print colon if the str() of the
               object is not the empty string
            */
            if (s == NULL)
                err = -1;
            else if (!PyString_Check(s) ||
                     PyString_GET_SIZE(s) != 0)
                err = PyFile_WriteString(": ", f);
            if (err == 0)
              err = PyFile_WriteObject(s, f, Py_PRINT_RAW);
            Py_XDECREF(s);
        }
        /* try to write a newline in any case */
        err += PyFile_WriteString("\n", f);
    }
    Py_DECREF(value);
    /* If an error happened here, don't show it.
       XXX This is wrong, but too many callers rely on this behavior. */
    if (err != 0)
        PyErr_Clear();
}

/* Print fatal error message and abort */

void
Py_FatalError(const char *msg)
{
    fprintf(stderr, "Fatal Python error: %s\n", msg);
    fflush(stderr); /* it helps in Windows debug build */

#ifdef MS_WINDOWS
    {
        size_t len = strlen(msg);
        WCHAR* buffer;
        size_t i;

        /* Convert the message to wchar_t. This uses a simple one-to-one
        conversion, assuming that the this error message actually uses ASCII
        only. If this ceases to be true, we will have to convert. */
        buffer = alloca( (len+1) * (sizeof *buffer));
        for( i=0; i<=len; ++i)
            buffer[i] = msg[i];
        OutputDebugStringW(L"Fatal Python error: ");
        OutputDebugStringW(buffer);
        OutputDebugStringW(L"\n");
    }
#ifdef _DEBUG
    DebugBreak();
#endif
#endif /* MS_WINDOWS */
    abort();
}

/* Clean up and exit */

#ifdef WITH_THREAD
#include "pythread.h"
#endif

/* Wait until threading._shutdown completes, provided
   the threading module was imported in the first place.
   The shutdown routine will wait until all non-daemon
   "threading" threads have completed. */
static void
wait_for_thread_shutdown(void)
{
#ifdef WITH_THREAD
    PyObject *result;
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *threading = PyMapping_GetItemString(tstate->interp->modules,
                                                  "threading");
    if (threading == NULL) {
        /* threading not imported */
        PyErr_Clear();
        return;
    }
    result = PyObject_CallMethod(threading, "_shutdown", "");
    if (result == NULL)
        PyErr_WriteUnraisable(threading);
    else
        Py_DECREF(result);
    Py_DECREF(threading);
#endif
}

#define NEXITFUNCS 32
static void (*exitfuncs[NEXITFUNCS])(void);
static int nexitfuncs = 0;

int Py_AtExit(void (*func)(void))
{
    if (nexitfuncs >= NEXITFUNCS)
        return -1;
    exitfuncs[nexitfuncs++] = func;
    return 0;
}

static void
call_sys_exitfunc(void)
{
    PyObject *exitfunc = PySys_GetObject("exitfunc");

    if (exitfunc) {
        PyObject *res;
        Py_INCREF(exitfunc);
        PySys_SetObject("exitfunc", (PyObject *)NULL);
        res = PyEval_CallObject(exitfunc, (PyObject *)NULL);
        if (res == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_SystemExit)) {
                PySys_WriteStderr("Error in sys.exitfunc:\n");
            }
            PyErr_Print();
        }
        Py_DECREF(exitfunc);
    }

    if (Py_FlushLine())
        PyErr_Clear();
}

static void
call_ll_exitfuncs(void)
{
    while (nexitfuncs > 0)
        (*exitfuncs[--nexitfuncs])();

    fflush(stdout);
    fflush(stderr);
}

void
Py_Exit(int sts)
{
    Py_Finalize();

    exit(sts);
}

static void
initsigs(void)
{
#ifdef SIGPIPE
    PyOS_setsig(SIGPIPE, SIG_IGN);
#endif
#ifdef SIGXFZ
    PyOS_setsig(SIGXFZ, SIG_IGN);
#endif
#ifdef SIGXFSZ
    PyOS_setsig(SIGXFSZ, SIG_IGN);
#endif
    PyOS_InitInterrupts(); /* May imply initsignal() */
}


/*
 * The file descriptor fd is considered ``interactive'' if either
 *   a) isatty(fd) is TRUE, or
 *   b) the -i flag was given, and the filename associated with
 *      the descriptor is NULL or "<stdin>" or "???".
 */
int
Py_FdIsInteractive(FILE *fp, const char *filename)
{
    if (isatty((int)fileno(fp)))
        return 1;
    if (!Py_InteractiveFlag)
        return 0;
    return (filename == NULL) ||
           (strcmp(filename, "<stdin>") == 0) ||
           (strcmp(filename, "???") == 0);
}


#if defined(USE_STACKCHECK)
#if defined(WIN32) && defined(_MSC_VER)

/* Stack checking for Microsoft C */

#include <malloc.h>
#include <excpt.h>

/*
 * Return non-zero when we run out of memory on the stack; zero otherwise.
 */
int
PyOS_CheckStack(void)
{
    __try {
        /* alloca throws a stack overflow exception if there's
           not enough space left on the stack */
        alloca(PYOS_STACK_MARGIN * sizeof(void*));
        return 0;
    } __except (GetExceptionCode() == STATUS_STACK_OVERFLOW ?
                    EXCEPTION_EXECUTE_HANDLER :
            EXCEPTION_CONTINUE_SEARCH) {
        int errcode = _resetstkoflw();
        if (errcode == 0)
        {
            Py_FatalError("Could not reset the stack!");
        }
    }
    return 1;
}

#endif /* WIN32 && _MSC_VER */

/* Alternate implementations can be added here... */

#endif /* USE_STACKCHECK */


/* Wrappers around sigaction() or signal(). */

PyOS_sighandler_t
PyOS_getsig(int sig)
{
#ifdef HAVE_SIGACTION
    struct sigaction context;
    if (sigaction(sig, NULL, &context) == -1)
        return SIG_ERR;
    return context.sa_handler;
#else
    PyOS_sighandler_t handler;
/* Special signal handling for the secure CRT in Visual Studio 2005 */
#if defined(_MSC_VER) && _MSC_VER >= 1400
    switch (sig) {
    /* Only these signals are valid */
    case SIGINT:
    case SIGILL:
    case SIGFPE:
    case SIGSEGV:
    case SIGTERM:
    case SIGBREAK:
    case SIGABRT:
        break;
    /* Don't call signal() with other values or it will assert */
    default:
        return SIG_ERR;
    }
#endif /* _MSC_VER && _MSC_VER >= 1400 */
    handler = signal(sig, SIG_IGN);
    if (handler != SIG_ERR)
        signal(sig, handler);
    return handler;
#endif
}

PyOS_sighandler_t
PyOS_setsig(int sig, PyOS_sighandler_t handler)
{
#ifdef HAVE_SIGACTION
    /* Some code in Modules/signalmodule.c depends on sigaction() being
     * used here if HAVE_SIGACTION is defined.  Fix that if this code
     * changes to invalidate that assumption.
     */
    struct sigaction context, ocontext;
    context.sa_handler = handler;
    sigemptyset(&context.sa_mask);
    context.sa_flags = 0;
    if (sigaction(sig, &context, &ocontext) == -1)
        return SIG_ERR;
    return ocontext.sa_handler;
#else
    PyOS_sighandler_t oldhandler;
    oldhandler = signal(sig, handler);
#ifdef HAVE_SIGINTERRUPT
    siginterrupt(sig, 1);
#endif
    return oldhandler;
#endif
}

#ifdef __cplusplus
}
#endif
