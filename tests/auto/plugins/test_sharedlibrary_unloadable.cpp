// SPDX-License-Identifier: MIT

// A module for the SharedLibrary tests and nothing else, so that "is it still loaded" is a
// question about the case asking rather than about what another suite left behind. It links
// nothing, so loading it pulls in no dependency that could keep it alive.

#if defined(_WIN32)
#  define TEST_SHAREDLIBRARY_UNLOADABLE_EXPORT __declspec(dllexport)
#else
#  define TEST_SHAREDLIBRARY_UNLOADABLE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" TEST_SHAREDLIBRARY_UNLOADABLE_EXPORT int test_sharedlibrary_unloadable_answer() {
    return 42;
}
