
/* Interfaces to parse and execute pieces of python code */

#ifndef Py_PYTHONRUN_H
#define Py_PYTHONRUN_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(void) Py_SetProgramName(char *);
PyAPI_FUNC(char *) Py_GetProgramName(void);

PyAPI_FUNC(void) Py_SetPythonHome(char *);
PyAPI_FUNC(char *) Py_GetPythonHome(void);

PyAPI_FUNC(void) Py_Initialize(void);
PyAPI_FUNC(void) Py_InitializeEx(int);
PyAPI_FUNC(void) Py_Finalize(void);
PyAPI_FUNC(int) Py_IsInitialized(void);
PyAPI_FUNC(PyThreadState *) Py_NewInterpreter(void);
PyAPI_FUNC(void) Py_EndInterpreter(PyThreadState *);

PyAPI_FUNC(void) PyErr_Print(void);
PyAPI_FUNC(void) PyErr_PrintEx(int);
PyAPI_FUNC(void) PyErr_Display(PyObject *, PyObject *, PyObject *);

PyAPI_FUNC(int) Py_AtExit(void (*func)(void));

PyAPI_FUNC(void) Py_Exit(int);

PyAPI_FUNC(int) Py_FdIsInteractive(FILE *, const char *);

/* Bootstrap */
PyAPI_FUNC(int) Py_Main(int argc, char **argv);

/* In getpath.c */
PyAPI_FUNC(char *) Py_GetProgramFullPath(void);
PyAPI_FUNC(char *) Py_GetPrefix(void);
PyAPI_FUNC(char *) Py_GetExecPrefix(void);
PyAPI_FUNC(char *) Py_GetPath(void);

/* In their own files */
PyAPI_FUNC(const char *) Py_GetVersion(void);
PyAPI_FUNC(const char *) Py_GetPlatform(void);
PyAPI_FUNC(const char *) Py_GetCopyright(void);
PyAPI_FUNC(const char *) Py_GetCompiler(void);
PyAPI_FUNC(const char *) Py_GetBuildInfo(void);
PyAPI_FUNC(const char *) _Py_svnversion(void);
PyAPI_FUNC(const char *) Py_SubversionRevision(void);
PyAPI_FUNC(const char *) Py_SubversionShortBranch(void);
PyAPI_FUNC(const char *) _Py_hgidentifier(void);
PyAPI_FUNC(const char *) _Py_hgversion(void);

/* Internal -- various one-time initializations */
PyAPI_FUNC(PyObject *) _PyBuiltin_Init(void);
PyAPI_FUNC(PyObject *) _PySys_Init(void);
PyAPI_FUNC(void) _PyImport_Init(void);
PyAPI_FUNC(void) _PyExc_Init(void);
PyAPI_FUNC(void) _PyImportHooks_Init(void);
PyAPI_FUNC(int) _PyFrame_Init(void);
PyAPI_FUNC(int) _PyInt_Init(void);
PyAPI_FUNC(int) _PyLong_Init(void);
PyAPI_FUNC(void) _PyFloat_Init(void);
PyAPI_FUNC(int) PyByteArray_Init(void);
PyAPI_FUNC(void) _PyRandom_Init(void);

/* Various internal finalizers */
PyAPI_FUNC(void) _PyExc_Fini(void);
PyAPI_FUNC(void) _PyImport_Fini(void);
PyAPI_FUNC(void) PyMethod_Fini(void);
PyAPI_FUNC(void) PyFrame_Fini(void);
PyAPI_FUNC(void) PyCFunction_Fini(void);
PyAPI_FUNC(void) PyDict_Fini(void);
PyAPI_FUNC(void) PyTuple_Fini(void);
PyAPI_FUNC(void) PyList_Fini(void);
PyAPI_FUNC(void) PySet_Fini(void);
PyAPI_FUNC(void) PyString_Fini(void);
PyAPI_FUNC(void) PyInt_Fini(void);
PyAPI_FUNC(void) PyFloat_Fini(void);
PyAPI_FUNC(void) PyOS_FiniInterrupts(void);
PyAPI_FUNC(void) PyByteArray_Fini(void);
PyAPI_FUNC(void) _PyRandom_Fini(void);

/* Stuff with no proper home (yet) */
PyAPI_FUNC(char *) PyOS_Readline(FILE *, FILE *, char *);
PyAPI_DATA(int) (*PyOS_InputHook)(void);
PyAPI_DATA(char) *(*PyOS_ReadlineFunctionPointer)(FILE *, FILE *, char *);
PyAPI_DATA(PyThreadState*) _PyOS_ReadlineTState;

/* Stack size, in "pointers" (so we get extra safety margins
   on 64-bit platforms).  On a 32-bit platform, this translates
   to a 8k margin. */
#define PYOS_STACK_MARGIN 2048

#if defined(WIN32) && !defined(MS_WIN64) && defined(_MSC_VER) && _MSC_VER >= 1300
/* Enable stack checking under Microsoft C */
#define USE_STACKCHECK
#endif

#ifdef USE_STACKCHECK
/* Check that we aren't overflowing our stack */
PyAPI_FUNC(int) PyOS_CheckStack(void);
#endif

/* Signals */
typedef void (*PyOS_sighandler_t)(int);
PyAPI_FUNC(PyOS_sighandler_t) PyOS_getsig(int);
PyAPI_FUNC(PyOS_sighandler_t) PyOS_setsig(int, PyOS_sighandler_t);

/* Random */
PyAPI_FUNC(int) _PyOS_URandom (void *buffer, Py_ssize_t size);

#ifdef __cplusplus
}
#endif
#endif /* !Py_PYTHONRUN_H */
