#ifndef Py_CONFIG_H
#define Py_CONFIG_H

#ifndef Py_NO_ENABLE_SHARED
#define Py_NO_ENABLE_SHARED
#endif

#ifdef Py_ENABLE_SHARED
#undef Py_ENABLE_SHARED
#endif

#ifdef __linux
    #include "pyconfig_linux.h"

#elif _WIN32
    #include "pyconfig_windows.h"

#elif __APPLE__
    #include "pyconfig_mac.h"

#elif ANDROID
    #include "pyconfig_android.h"

#else
    #error "No pyconfig for your OS could be found."
#endif

#endif
