
/* Write Python objects to files and read them back.
   This is intended for writing and reading compiled Python code only;
   a true persistent storage facility would be much harder, since
   it would have to take circular links and sharing into account. */

#define PY_SSIZE_T_CLEAN

#include "Python.h"
#include "longintrepr.h"
#include "marshal.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))

/* High water mark to determine when the marshalled object is dangerously deep
 * and risks coring the interpreter.  When the object stack gets this deep,
 * raise an exception instead of continuing.
 */
#define MAX_MARSHAL_STACK_DEPTH 2000

#define TYPE_NULL               '0'
#define TYPE_NONE               'N'
#define TYPE_FALSE              'F'
#define TYPE_TRUE               'T'
#define TYPE_STOPITER           'S'
#define TYPE_ELLIPSIS           '.'
#define TYPE_INT                'i'
#define TYPE_INT64              'I'
#define TYPE_FLOAT              'f'
#define TYPE_BINARY_FLOAT       'g'
#define TYPE_COMPLEX            'x'
#define TYPE_BINARY_COMPLEX     'y'
#define TYPE_LONG               'l'
#define TYPE_STRING             's'
#define TYPE_INTERNED           't'
#define TYPE_STRINGREF          'R'
#define TYPE_TUPLE              '('
#define TYPE_LIST               '['
#define TYPE_DICT               '{'
#define TYPE_UNICODE            'u'
#define TYPE_UNKNOWN            '?'
#define TYPE_SET                '<'
#define TYPE_FROZENSET          '>'

#define SIZE32_MAX  0x7FFFFFFF

/* We assume that Python longs are stored internally in base some power of
   2**15; for the sake of portability we'll always read and write them in base
   exactly 2**15. */

#define PyLong_MARSHAL_SHIFT 15
#define PyLong_MARSHAL_BASE ((short)1 << PyLong_MARSHAL_SHIFT)
#define PyLong_MARSHAL_MASK (PyLong_MARSHAL_BASE - 1)
#if PyLong_SHIFT % PyLong_MARSHAL_SHIFT != 0
#error "PyLong_SHIFT must be a multiple of PyLong_MARSHAL_SHIFT"
#endif
#define PyLong_MARSHAL_RATIO (PyLong_SHIFT / PyLong_MARSHAL_SHIFT)

#define rs_byte(p) (((p)->ptr < (p)->end) ? (unsigned char)*(p)->ptr++ : EOF)

#define r_byte(p) ((p)->fp ? getc((p)->fp) : rs_byte(p))

typedef struct {
    FILE *fp;
    int error;  /* see WFERR_* values */
    int depth;
    /* If fp == NULL, the following are valid: */
    PyObject *str;
    char *ptr;
    char *end;
    PyObject *strings; /* dict on marshal, list on unmarshal */
    int version;
} RFILE;

static Py_ssize_t
r_string(char *s, Py_ssize_t n, RFILE *p)
{
    if (p->fp != NULL)
        /* The result fits into int because it must be <=n. */
        return fread(s, 1, n, p->fp);
    if (p->end - p->ptr < n)
        n = p->end - p->ptr;
    memcpy(s, p->ptr, n);
    p->ptr += n;
    return n;
}

static int
r_short(RFILE *p)
{
    register short x;
    x = r_byte(p);
    x |= r_byte(p) << 8;
    /* Sign-extension, in case short greater than 16 bits */
    x |= -(x & 0x8000);
    return x;
}

static long
r_long(RFILE *p)
{
    register long x;
    register FILE *fp = p->fp;
    if (fp) {
        x = getc(fp);
        x |= (long)getc(fp) << 8;
        x |= (long)getc(fp) << 16;
        x |= (long)getc(fp) << 24;
    }
    else {
        x = rs_byte(p);
        x |= (long)rs_byte(p) << 8;
        x |= (long)rs_byte(p) << 16;
        x |= (long)rs_byte(p) << 24;
    }
#if SIZEOF_LONG > 4
    /* Sign extension for 64-bit machines */
    x |= -(x & 0x80000000L);
#endif
    return x;
}

/* r_long64 deals with the TYPE_INT64 code.  On a machine with
   sizeof(long) > 4, it returns a Python int object, else a Python long
   object.  Note that w_long64 writes out TYPE_INT if 32 bits is enough,
   so there's no inefficiency here in returning a PyLong on 32-bit boxes
   for everything written via TYPE_INT64 (i.e., if an int is written via
   TYPE_INT64, it *needs* more than 32 bits).
*/
static PyObject *
r_long64(RFILE *p)
{
    long lo4 = r_long(p);
    long hi4 = r_long(p);
#if SIZEOF_LONG > 4
    long x = (hi4 << 32) | (lo4 & 0xFFFFFFFFL);
    return PyInt_FromLong(x);
#else
    unsigned char buf[8];
    int one = 1;
    int is_little_endian = (int)*(char*)&one;
    if (is_little_endian) {
        memcpy(buf, &lo4, 4);
        memcpy(buf+4, &hi4, 4);
    }
    else {
        memcpy(buf, &hi4, 4);
        memcpy(buf+4, &lo4, 4);
    }
    return _PyLong_FromByteArray(buf, 8, is_little_endian, 1);
#endif
}

static PyObject *
r_PyLong(RFILE *p)
{
    PyLongObject *ob;
    long n, size, i;
    int j, md, shorts_in_top_digit;
    digit d;

    n = r_long(p);
    if (n == 0)
        return (PyObject *)_PyLong_New(0);
    if (n < -SIZE32_MAX || n > SIZE32_MAX) {
        PyErr_SetString(PyExc_ValueError,
                       "bad marshal data (long size out of range)");
        return NULL;
    }

    size = 1 + (ABS(n) - 1) / PyLong_MARSHAL_RATIO;
    shorts_in_top_digit = 1 + (ABS(n) - 1) % PyLong_MARSHAL_RATIO;
    ob = _PyLong_New(size);
    if (ob == NULL)
        return NULL;
    Py_SIZE(ob) = n > 0 ? size : -size;

    for (i = 0; i < size-1; i++) {
        d = 0;
        for (j=0; j < PyLong_MARSHAL_RATIO; j++) {
            md = r_short(p);
            if (md < 0 || md > PyLong_MARSHAL_BASE)
                goto bad_digit;
            d += (digit)md << j*PyLong_MARSHAL_SHIFT;
        }
        ob->ob_digit[i] = d;
    }
    d = 0;
    for (j=0; j < shorts_in_top_digit; j++) {
        md = r_short(p);
        if (md < 0 || md > PyLong_MARSHAL_BASE)
            goto bad_digit;
        /* topmost marshal digit should be nonzero */
        if (md == 0 && j == shorts_in_top_digit - 1) {
            Py_DECREF(ob);
            PyErr_SetString(PyExc_ValueError,
                "bad marshal data (unnormalized long data)");
            return NULL;
        }
        d += (digit)md << j*PyLong_MARSHAL_SHIFT;
    }
    /* top digit should be nonzero, else the resulting PyLong won't be
       normalized */
    ob->ob_digit[size-1] = d;
    return (PyObject *)ob;
  bad_digit:
    Py_DECREF(ob);
    PyErr_SetString(PyExc_ValueError,
                    "bad marshal data (digit out of range in long)");
    return NULL;
}


static PyObject *
r_object(RFILE *p)
{
    /* NULL is a valid return value, it does not necessarily means that
       an exception is set. */
    PyObject *v, *v2;
    long i, n;
    int type = r_byte(p);
    PyObject *retval;

    p->depth++;

    if (p->depth > MAX_MARSHAL_STACK_DEPTH) {
        p->depth--;
        PyErr_SetString(PyExc_ValueError, "recursion limit exceeded");
        return NULL;
    }

    switch (type) {

    case EOF:
        PyErr_SetString(PyExc_EOFError,
                        "EOF read where object expected");
        retval = NULL;
        break;

    case TYPE_NULL:
        retval = NULL;
        break;

    case TYPE_STRINGREF:
        n = r_long(p);
        if (n < 0 || n >= PyList_GET_SIZE(p->strings)) {
            PyErr_SetString(PyExc_ValueError, "bad marshal data (string ref out of range)");
            retval = NULL;
            break;
        }
        v = PyList_GET_ITEM(p->strings, n);
        Py_INCREF(v);
        retval = v;
        break;

    case TYPE_NONE:
        Py_INCREF(Py_None);
        retval = Py_None;
        break;

    case TYPE_STOPITER:
        Py_INCREF(PyExc_StopIteration);
        retval = PyExc_StopIteration;
        break;

    case TYPE_ELLIPSIS:
        Py_INCREF(Py_Ellipsis);
        retval = Py_Ellipsis;
        break;

    case TYPE_FALSE:
        Py_INCREF(Py_False);
        retval = Py_False;
        break;

    case TYPE_TRUE:
        Py_INCREF(Py_True);
        retval = Py_True;
        break;

    case TYPE_INT:
        retval = PyInt_FromLong(r_long(p));
        break;

    case TYPE_INT64:
        retval = r_long64(p);
        break;

    case TYPE_LONG:
        retval = r_PyLong(p);
        break;

    case TYPE_FLOAT:
        {
            char buf[256];
            double dx;
            n = r_byte(p);
            if (n == EOF || r_string(buf, n, p) != n) {
                PyErr_SetString(PyExc_EOFError,
                    "EOF read where object expected");
                retval = NULL;
                break;
            }
            buf[n] = '\0';
            dx = PyOS_string_to_double(buf, NULL, NULL);
            if (dx == -1.0 && PyErr_Occurred()) {
                retval = NULL;
                break;
            }
            retval = PyFloat_FromDouble(dx);
            break;
        }

    case TYPE_BINARY_FLOAT:
        {
            unsigned char buf[8];
            double x;
            if (r_string((char*)buf, 8, p) != 8) {
                PyErr_SetString(PyExc_EOFError,
                    "EOF read where object expected");
                retval = NULL;
                break;
            }
            x = _PyFloat_Unpack8(buf, 1);
            if (x == -1.0 && PyErr_Occurred()) {
                retval = NULL;
                break;
            }
            retval = PyFloat_FromDouble(x);
            break;
        }

#ifndef WITHOUT_COMPLEX
    case TYPE_COMPLEX:
        {
            char buf[256];
            Py_complex c;
            n = r_byte(p);
            if (n == EOF || r_string(buf, n, p) != n) {
                PyErr_SetString(PyExc_EOFError,
                    "EOF read where object expected");
                retval = NULL;
                break;
            }
            buf[n] = '\0';
            c.real = PyOS_string_to_double(buf, NULL, NULL);
            if (c.real == -1.0 && PyErr_Occurred()) {
                retval = NULL;
                break;
            }
            n = r_byte(p);
            if (n == EOF || r_string(buf, n, p) != n) {
                PyErr_SetString(PyExc_EOFError,
                    "EOF read where object expected");
                retval = NULL;
                break;
            }
            buf[n] = '\0';
            c.imag = PyOS_string_to_double(buf, NULL, NULL);
            if (c.imag == -1.0 && PyErr_Occurred()) {
                retval = NULL;
                break;
            }
            retval = PyComplex_FromCComplex(c);
            break;
        }

    case TYPE_BINARY_COMPLEX:
        {
            unsigned char buf[8];
            Py_complex c;
            if (r_string((char*)buf, 8, p) != 8) {
                PyErr_SetString(PyExc_EOFError,
                    "EOF read where object expected");
                retval = NULL;
                break;
            }
            c.real = _PyFloat_Unpack8(buf, 1);
            if (c.real == -1.0 && PyErr_Occurred()) {
                retval = NULL;
                break;
            }
            if (r_string((char*)buf, 8, p) != 8) {
                PyErr_SetString(PyExc_EOFError,
                    "EOF read where object expected");
                retval = NULL;
                break;
            }
            c.imag = _PyFloat_Unpack8(buf, 1);
            if (c.imag == -1.0 && PyErr_Occurred()) {
                retval = NULL;
                break;
            }
            retval = PyComplex_FromCComplex(c);
            break;
        }
#endif

    case TYPE_INTERNED:
    case TYPE_STRING:
        n = r_long(p);
        if (n < 0 || n > SIZE32_MAX) {
            PyErr_SetString(PyExc_ValueError, "bad marshal data (string size out of range)");
            retval = NULL;
            break;
        }
        v = PyString_FromStringAndSize((char *)NULL, n);
        if (v == NULL) {
            retval = NULL;
            break;
        }
        if (r_string(PyString_AS_STRING(v), n, p) != n) {
            Py_DECREF(v);
            PyErr_SetString(PyExc_EOFError,
                            "EOF read where object expected");
            retval = NULL;
            break;
        }
        if (type == TYPE_INTERNED) {
            PyString_InternInPlace(&v);
            if (PyList_Append(p->strings, v) < 0) {
                retval = NULL;
                break;
            }
        }
        retval = v;
        break;

#ifdef Py_USING_UNICODE
    case TYPE_UNICODE:
        {
        char *buffer;

        n = r_long(p);
        if (n < 0 || n > SIZE32_MAX) {
            PyErr_SetString(PyExc_ValueError, "bad marshal data (unicode size out of range)");
            retval = NULL;
            break;
        }
        buffer = PyMem_NEW(char, n);
        if (buffer == NULL) {
            retval = PyErr_NoMemory();
            break;
        }
        if (r_string(buffer, n, p) != n) {
            PyMem_DEL(buffer);
            PyErr_SetString(PyExc_EOFError,
                "EOF read where object expected");
            retval = NULL;
            break;
        }
        v = PyUnicode_DecodeUTF8(buffer, n, NULL);
        PyMem_DEL(buffer);
        retval = v;
        break;
        }
#endif

    case TYPE_TUPLE:
        n = r_long(p);
        if (n < 0 || n > SIZE32_MAX) {
            PyErr_SetString(PyExc_ValueError, "bad marshal data (tuple size out of range)");
            retval = NULL;
            break;
        }
        v = PyTuple_New(n);
        if (v == NULL) {
            retval = NULL;
            break;
        }
        for (i = 0; i < n; i++) {
            v2 = r_object(p);
            if ( v2 == NULL ) {
                if (!PyErr_Occurred())
                    PyErr_SetString(PyExc_TypeError,
                        "NULL object in marshal data for tuple");
                Py_DECREF(v);
                v = NULL;
                break;
            }
            PyTuple_SET_ITEM(v, i, v2);
        }
        retval = v;
        break;

    case TYPE_LIST:
        n = r_long(p);
        if (n < 0 || n > SIZE32_MAX) {
            PyErr_SetString(PyExc_ValueError, "bad marshal data (list size out of range)");
            retval = NULL;
            break;
        }
        v = PyList_New(n);
        if (v == NULL) {
            retval = NULL;
            break;
        }
        for (i = 0; i < n; i++) {
            v2 = r_object(p);
            if ( v2 == NULL ) {
                if (!PyErr_Occurred())
                    PyErr_SetString(PyExc_TypeError,
                        "NULL object in marshal data for list");
                Py_DECREF(v);
                v = NULL;
                break;
            }
            PyList_SET_ITEM(v, i, v2);
        }
        retval = v;
        break;

    case TYPE_DICT:
        v = PyDict_New();
        if (v == NULL) {
            retval = NULL;
            break;
        }
        for (;;) {
            PyObject *key, *val;
            key = r_object(p);
            if (key == NULL)
                break;
            val = r_object(p);
            if (val != NULL)
                PyDict_SetItem(v, key, val);
            Py_DECREF(key);
            Py_XDECREF(val);
        }
        if (PyErr_Occurred()) {
            Py_DECREF(v);
            v = NULL;
        }
        retval = v;
        break;

    case TYPE_SET:
    case TYPE_FROZENSET:
        n = r_long(p);
        if (n < 0 || n > SIZE32_MAX) {
            PyErr_SetString(PyExc_ValueError, "bad marshal data (set size out of range)");
            retval = NULL;
            break;
        }
        v = (type == TYPE_SET) ? PySet_New(NULL) : PyFrozenSet_New(NULL);
        if (v == NULL) {
            retval = NULL;
            break;
        }
        for (i = 0; i < n; i++) {
            v2 = r_object(p);
            if ( v2 == NULL ) {
                if (!PyErr_Occurred())
                    PyErr_SetString(PyExc_TypeError,
                        "NULL object in marshal data for set");
                Py_DECREF(v);
                v = NULL;
                break;
            }
            if (PySet_Add(v, v2) == -1) {
                Py_DECREF(v);
                Py_DECREF(v2);
                v = NULL;
                break;
            }
            Py_DECREF(v2);
        }
        retval = v;
        break;

    default:
        /* Bogus data got written, which isn't ideal.
           This will let you keep working and recover. */
        PyErr_SetString(PyExc_ValueError, "bad marshal data (unknown type code)");
        retval = NULL;
        break;

    }
    p->depth--;
    return retval;
}

static PyObject *
read_object(RFILE *p)
{
    PyObject *v;
    if (PyErr_Occurred()) {
        fprintf(stderr, "XXX readobject called with exception set\n");
        return NULL;
    }
    v = r_object(p);
    if (v == NULL && !PyErr_Occurred())
        PyErr_SetString(PyExc_TypeError, "NULL object in marshal data for object");
    return v;
}

int
PyMarshal_ReadShortFromFile(FILE *fp)
{
    RFILE rf;
    assert(fp);
    rf.fp = fp;
    rf.strings = NULL;
    rf.end = rf.ptr = NULL;
    return r_short(&rf);
}

long
PyMarshal_ReadLongFromFile(FILE *fp)
{
    RFILE rf;
    rf.fp = fp;
    rf.strings = NULL;
    rf.ptr = rf.end = NULL;
    return r_long(&rf);
}

#ifdef HAVE_FSTAT
/* Return size of file in bytes; < 0 if unknown. */
static off_t
getfilesize(FILE *fp)
{
    struct stat st;
    if (fstat(fileno(fp), &st) != 0)
        return -1;
    else
        return st.st_size;
}
#endif

/* If we can get the size of the file up-front, and it's reasonably small,
 * read it in one gulp and delegate to ...FromString() instead.  Much quicker
 * than reading a byte at a time from file; speeds .pyc imports.
 * CAUTION:  since this may read the entire remainder of the file, don't
 * call it unless you know you're done with the file.
 */
PyObject *
PyMarshal_ReadLastObjectFromFile(FILE *fp)
{
/* REASONABLE_FILE_LIMIT is by defn something big enough for Tkinter.pyc. */
#define REASONABLE_FILE_LIMIT (1L << 18)
#ifdef HAVE_FSTAT
    off_t filesize;
    filesize = getfilesize(fp);
    if (filesize > 0 && filesize <= REASONABLE_FILE_LIMIT) {
        char* pBuf = (char *)PyMem_MALLOC(filesize);
        if (pBuf != NULL) {
            size_t n = fread(pBuf, 1, (size_t)filesize, fp);
            PyObject* v = PyMarshal_ReadObjectFromString(pBuf, n);
            PyMem_FREE(pBuf);
            return v;
        }

    }
#endif
    /* We don't have fstat, or we do but the file is larger than
     * REASONABLE_FILE_LIMIT or malloc failed -- read a byte at a time.
     */
    return PyMarshal_ReadObjectFromFile(fp);

#undef REASONABLE_FILE_LIMIT
}

PyObject *
PyMarshal_ReadObjectFromFile(FILE *fp)
{
    RFILE rf;
    PyObject *result;
    rf.fp = fp;
    rf.strings = PyList_New(0);
    rf.depth = 0;
    rf.ptr = rf.end = NULL;
    result = r_object(&rf);
    Py_DECREF(rf.strings);
    return result;
}

PyObject *
PyMarshal_ReadObjectFromString(char *str, Py_ssize_t len)
{
    RFILE rf;
    PyObject *result;
    rf.fp = NULL;
    rf.ptr = str;
    rf.end = str + len;
    rf.strings = PyList_New(0);
    rf.depth = 0;
    result = r_object(&rf);
    Py_DECREF(rf.strings);
    return result;
}

static PyObject *
marshal_load(PyObject *self, PyObject *f)
{
    RFILE rf;
    PyObject *result;
    if (!PyFile_Check(f)) {
        PyErr_SetString(PyExc_TypeError,
                        "marshal.load() arg must be file");
        return NULL;
    }
    rf.fp = PyFile_AsFile(f);
    rf.strings = PyList_New(0);
    rf.depth = 0;
    result = read_object(&rf);
    Py_DECREF(rf.strings);
    return result;
}

PyDoc_STRVAR(load_doc,
"load(file)\n\
\n\
Read one value from the open file and return it. If no valid value is\n\
read (e.g. because the data has a different Python version’s\n\
incompatible marshal format), raise EOFError, ValueError or TypeError.\n\
The file must be an open file object opened in binary mode ('rb' or\n\
'r+b').\n\
\n\
Note: If an object containing an unsupported type was marshalled with\n\
dump(), load() will substitute None for the unmarshallable type.");


static PyObject *
marshal_loads(PyObject *self, PyObject *args)
{
    RFILE rf;
    char *s;
    Py_ssize_t n;
    PyObject* result;
    if (!PyArg_ParseTuple(args, "s#:loads", &s, &n))
        return NULL;
    rf.fp = NULL;
    rf.ptr = s;
    rf.end = s + n;
    rf.strings = PyList_New(0);
    rf.depth = 0;
    result = read_object(&rf);
    Py_DECREF(rf.strings);
    return result;
}

PyDoc_STRVAR(loads_doc,
"loads(string)\n\
\n\
Convert the string to a value. If no valid value is found, raise\n\
EOFError, ValueError or TypeError. Extra characters in the string are\n\
ignored.");

static PyMethodDef marshal_methods[] = {
    {"load",            marshal_load,   METH_O,         load_doc},
    {"loads",           marshal_loads,  METH_VARARGS,   loads_doc},
    {NULL,              NULL}           /* sentinel */
};

PyDoc_STRVAR(marshal_doc,
"This module contains functions that can read and write Python values in\n\
a binary format. The format is specific to Python, but independent of\n\
machine architecture issues.\n\
\n\
Not all Python object types are supported; in general, only objects\n\
whose value is independent from a particular invocation of Python can be\n\
written and read by this module. The following types are supported:\n\
None, integers, long integers, floating point numbers, strings, Unicode\n\
objects, tuples, lists, sets, dictionaries, and code objects, where it\n\
should be understood that tuples, lists and dictionaries are only\n\
supported as long as the values contained therein are themselves\n\
supported; and recursive lists and dictionaries should not be written\n\
(they will cause infinite loops).\n\
\n\
Variables:\n\
\n\
version -- indicates the format that the module uses. Version 0 is the\n\
    historical format, version 1 (added in Python 2.4) shares interned\n\
    strings and version 2 (added in Python 2.5) uses a binary format for\n\
    floating point numbers. (New in version 2.4)\n\
\n\
Functions:\n\
\n\
load() -- read value from a file\n\
loads() -- read value from a string");


PyMODINIT_FUNC
PyMarshal_Init(void)
{
    PyObject *mod = Py_InitModule3("marshal", marshal_methods,
        marshal_doc);
    if (mod == NULL)
        return;
    PyModule_AddIntConstant(mod, "version", Py_MARSHAL_VERSION);
}
