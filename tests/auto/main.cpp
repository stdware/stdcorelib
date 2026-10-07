// SPDX-License-Identifier: MIT

#define BOOST_TEST_MAIN

#include <boost/test/unit_test.hpp>

#ifdef _WIN32
#  include <stdcorelib/platform/windows/stdc_windows.h>

namespace {

    // https://github.com/qt/qtbase/blob/v6.11.1/src/testlib/qtestcrashhandler_win.cpp#L181
    //
    // Disables Windows Error Reporting for this process and, because child processes inherit the
    // error mode, for every child process that the tests start, as the crash handler of Qt Test
    // does. Without this setting, a child that ends with a fast fail, such as the fatal logging
    // helper, waits until its report is processed. Processing takes about two seconds and
    // occasionally more than ten, which exceeds the time limits of the tests.
    struct NoErrorReporting {
        NoErrorReporting() {
            ::SetErrorMode(::SetErrorMode(0) | SEM_NOGPFAULTERRORBOX);
        }
    };

}

BOOST_TEST_GLOBAL_FIXTURE(NoErrorReporting);
#endif
