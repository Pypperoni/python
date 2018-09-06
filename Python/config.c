/* Module configuration */

/* This file contains the table of built-in modules.
   See init_builtin() in import.c. */

#include "Python.h"

#ifdef WIN32
extern void initmsvcrt(void);
extern void initnt(void);
extern void init_locale(void);
extern void init_subprocess(void);
#ifndef _WIN64
extern void init_winreg(void);
#endif
#ifndef MS_WINI64
extern void initaudioop(void);
extern void initimageop(void);
#endif
#else
extern void initposix(void);
#endif
extern void initarray(void);
extern void initbinascii(void);
extern void initcmath(void);
extern void initerrno(void);
extern void initfuture_builtins(void);
extern void initgc(void);
extern void initmath(void);
extern void init_md5(void);
extern void initoperator(void);
extern void initsignal(void);
extern void init_sha(void);
extern void init_sha256(void);
extern void init_sha512(void);
extern void initstrop(void);
extern void inittime(void);
extern void initthread(void);
extern void initcStringIO(void);
extern void initcPickle(void);
extern void init_codecs(void);
extern void init_weakref(void);
extern void initxxsubtype(void);
extern void init_random(void);
extern void inititertools(void);
extern void init_collections(void);
extern void init_heapq(void);
extern void init_bisect(void);
extern void initmmap(void);
extern void init_csv(void);
extern void init_sre(void);
extern void init_struct(void);
extern void initdatetime(void);
extern void init_functools(void);
extern void init_json(void);
extern void initzlib(void);
extern void init_multibytecodec(void);
extern void init_codecs_cn(void);
extern void init_codecs_hk(void);
extern void init_codecs_iso2022(void);
extern void init_codecs_jp(void);
extern void init_codecs_kr(void);
extern void init_codecs_tw(void);
extern void init_io(void);
extern void _PyWarnings_Init(void);
extern void init_ssl(void);
extern void init_hashlib(void);
extern void init_socket(void);
extern void initselect(void);
extern void initunicodedata(void);
extern void PyMarshal_Init(void);
extern void initimp(void);

struct _inittab _PyImport_Inittab[] = {
#ifdef MS_WINDOWS
    {"_locale", init_locale},
    {"_subprocess", init_subprocess},
#ifndef _WIN64
    {"_winreg", init_winreg},
#endif
    {"msvcrt", initmsvcrt},
    {"nt", initnt},
#ifndef MS_WINI64
    {"audioop", initaudioop},
    {"imageop", initimageop},
#endif
#else
    {"posix", initposix},
#endif
    {"array", initarray},
    {"binascii", initbinascii},
    {"cmath", initcmath},
    {"errno", initerrno},
    {"future_builtins", initfuture_builtins},
    {"gc", initgc},
    {"math", initmath},
    {"_md5", init_md5},
    {"operator", initoperator},
    {"signal", initsignal},
    {"_sha", init_sha},
    {"_sha256", init_sha256},
    {"_sha512", init_sha512},
    {"strop", initstrop},
    {"time", inittime},
#ifdef WITH_THREAD
    {"thread", initthread},
#endif
    {"cStringIO", initcStringIO},
    {"cPickle", initcPickle},
    {"_codecs", init_codecs},
    {"_weakref", init_weakref},
    {"_random", init_random},
    {"_bisect", init_bisect},
    {"_heapq", init_heapq},
    {"itertools", inititertools},
    {"_collections", init_collections},
    {"mmap", initmmap},
    {"_csv", init_csv},
    {"_sre", init_sre},
    {"_struct", init_struct},
    {"datetime", initdatetime},
    {"_functools", init_functools},
    {"_json", init_json},
    {"xxsubtype", initxxsubtype},
    {"zlib", initzlib},

    /* CJK codecs */
    {"_multibytecodec", init_multibytecodec},
    {"_codecs_cn", init_codecs_cn},
    {"_codecs_hk", init_codecs_hk},
    {"_codecs_iso2022", init_codecs_iso2022},
    {"_codecs_jp", init_codecs_jp},
    {"_codecs_kr", init_codecs_kr},
    {"_codecs_tw", init_codecs_tw},

    /* This module "lives in" with marshal.c */
    {"marshal", PyMarshal_Init},

    /* These entries are here for sys.builtin_module_names */
    {"__main__", NULL},
    {"__builtin__", NULL},
    {"sys", NULL},
    {"exceptions", NULL},
    {"_warnings", _PyWarnings_Init},

    {"_io", init_io},

    {"_ssl", init_ssl},
    {"_hashlib", init_hashlib},
    {"_socket", init_socket},
    {"select", initselect},
    {"unicodedata", initunicodedata},

    /* Sentinel */
    {0, 0}
};
