// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_STDC_WINDOWS_H
#define STDCORELIB_STDC_WINDOWS_H

// Suppresses the min and max macros of the Windows headers.
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>

#endif // STDCORELIB_STDC_WINDOWS_H
