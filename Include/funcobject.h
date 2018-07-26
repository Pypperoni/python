
/* Function object interface */

#ifndef Py_FUNCOBJECT_H
#define Py_FUNCOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

/* The classmethod and staticmethod types lives here, too */
PyAPI_DATA(PyTypeObject) PyClassMethod_Type;
PyAPI_DATA(PyTypeObject) PyStaticMethod_Type;

PyAPI_FUNC(PyObject *) PyClassMethod_New(PyObject *);
PyAPI_FUNC(PyObject *) PyStaticMethod_New(PyObject *);

int PyFunction_Check(PyObject*); // fwd decl

#ifdef __cplusplus
}
#endif
#endif /* !Py_FUNCOBJECT_H */
