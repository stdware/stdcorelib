// SPDX-License-Identifier: MIT

#include "sharedlibrary.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#  include "winapi.h"
#  include "winextra.h"
#else
#  include <dlfcn.h>
#  include <limits.h>
#  include <string.h>
#endif

#include "str.h"
#include "pimpl.h"

#ifdef __APPLE__
#  define PRIOR_LIBRARY_PATH_KEY "DYLD_LIBRARY_PATH"
#else
#  define PRIOR_LIBRARY_PATH_KEY "LD_LIBRARY_PATH"
#endif

namespace fs = std::filesystem;

namespace stdc {

    class SharedLibrary::Impl {
    public:
        void *hDll = nullptr;
        fs::path path;

        bool released = false;

        // Why the last operation failed, captured where it failed. Cleared at the start of
        // every operation, so it never outlives the call it belongs to.
        //
        // The system error has to be read at the point of failure rather than later: on Windows
        // GetLastError() is thread global and any call in between overwrites it, and dlerror()
        // clears itself as soon as it is read.
        mutable std::string error;

        virtual ~Impl();

        static inline int nativeLoadHints(LoadHints loadHints);
        static void clearSysError();
        static std::string sysErrorMessage();

        bool open(LoadHints hints = {});
        bool close();
        void *resolve(const char *name) const;
    };

    SharedLibrary::Impl::~Impl() {
        // What release() is for. Impl::close() is the unload itself and asks nothing, so an
        // object destroyed after release() used to unload anyway, and the flag only did
        // anything for a caller who went on to call the public close() as well.
        if (released) {
            return;
        }
        std::ignore = close();
    }

    inline int SharedLibrary::Impl::nativeLoadHints(LoadHints loadHints) {
#ifdef _WIN32
        return 0;
#else
        int dlFlags = 0;
        if (loadHints.test_flag(ResolveAllSymbolsHint)) {
            dlFlags |= RTLD_NOW;
        } else {
            dlFlags |= RTLD_LAZY;
        }
        if (loadHints.test_flag(ExportExternalSymbolsHint)) {
            dlFlags |= RTLD_GLOBAL;
        }
#  if !defined(Q_OS_CYGWIN)
        else {
            dlFlags |= RTLD_LOCAL;
        }
#  endif
#  if defined(RTLD_DEEPBIND)
        if (loadHints.test_flag(DeepBindHint)) {
            dlFlags |= RTLD_DEEPBIND;
        }
#  endif
#  if defined(RTLD_NODELETE)
        // The POSIX half of PreventUnloadHint, which used to be dropped here. glibc has this,
        // so a Linux caller asking for it was not being told the platform had no equivalent,
        // it was being ignored.
        if (loadHints.test_flag(PreventUnloadHint)) {
            dlFlags |= RTLD_NODELETE;
        }
#  endif
        return dlFlags;
#endif
    }

    // Run before a call whose failure we want to report, so that what sysErrorMessage() reads
    // afterwards belongs to that call. dlerror() holds whatever the last dl call left and hands
    // it to whoever asks first, and GetLastError() is not cleared on success.
    void SharedLibrary::Impl::clearSysError() {
#ifdef _WIN32
        ::SetLastError(ERROR_SUCCESS);
#else
        std::ignore = dlerror();
#endif
    }

    std::string SharedLibrary::Impl::sysErrorMessage() {
#ifdef _WIN32
        return wstring_conv::to_utf8(windows::SystemError(::GetLastError(), 0));
#else
        auto err = dlerror();
        if (err) {
            return err;
        }
        return {};
#endif
    }

    bool SharedLibrary::Impl::open(LoadHints hints) {
        auto absPath = fs::absolute(path);
        clearSysError();

        auto handle =
#ifdef _WIN32
            ::LoadLibraryW(absPath.c_str())
#else
            dlopen(absPath.c_str(), Impl::nativeLoadHints(hints))
#endif
            ;
        if (!handle) {
            error = sysErrorMessage();
            return false;
        }

#ifdef _WIN32
        if (hints.test_flag(PreventUnloadHint)) {
            // prevent the unloading of this component
            HMODULE hmod;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                     GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                 reinterpret_cast<const wchar_t *>(handle), &hmod);
        }
#endif

        hDll = handle;
        return true;
    }

    bool SharedLibrary::Impl::close() {
        if (!hDll) {
            return true;
        }
        clearSysError();

        if (!
#ifdef _WIN32
            ::FreeLibrary(reinterpret_cast<HMODULE>(hDll))
#else
            (dlclose(hDll) == 0)
#endif
        ) {
            error = sysErrorMessage();
            return false;
        }

        hDll = nullptr;
        return true;
    }

    void *SharedLibrary::Impl::resolve(const char *name) const {
        if (!hDll) {
            return nullptr;
        }
        clearSysError();

        auto addr =
#ifdef _WIN32
            ::GetProcAddress(reinterpret_cast<HMODULE>(hDll), name)
#else
            dlsym(hDll, name)
#endif
            ;
        if (!addr) {
            error = sysErrorMessage();
        }
        return reinterpret_cast<void *>(addr);
    }

    SharedLibrary::SharedLibrary() : _impl(new Impl()) {
    }

    SharedLibrary::~SharedLibrary() = default;

    SharedLibrary::SharedLibrary(SharedLibrary &&other) noexcept = default;

    SharedLibrary &SharedLibrary::operator=(SharedLibrary &&other) noexcept = default;

    bool SharedLibrary::open(const fs::path &path, LoadHints hints) {
        stdc_impl_t;
        impl.error.clear();
        if (impl.hDll) {
            impl.error = "library already open";
            return false;
        }
        impl.path = path;
        if (impl.open(hints)) {
            impl.path = fs::canonical(fs::absolute(path));
            return true;
        }
        impl.path.clear();
        return false;
    }

    bool SharedLibrary::close() {
        stdc_impl_t;
        impl.error.clear();
        if (impl.released) {
            impl.released = false;
            impl.hDll = nullptr;
            impl.path.clear();
            return true;
        }
        if (impl.close()) {
            impl.path.clear();
            return true;
        }
        return false;
    }

    bool SharedLibrary::isOpen() const {
        stdc_impl_t;
        return impl.hDll != nullptr;
    }

    fs::path SharedLibrary::path() const {
        stdc_impl_t;
        return impl.path;
    }

    void *SharedLibrary::handle() const {
        stdc_impl_t;
        return impl.hDll;
    }

    void *SharedLibrary::resolve(const char *name) const {
        stdc_impl_t;
        impl.error.clear();
        if (!impl.hDll) {
            impl.error = "library not open";
            return nullptr;
        }
        return impl.resolve(name);
    }

    std::string SharedLibrary::lastError() const {
        stdc_impl_t;
        return impl.error;
    }

    void SharedLibrary::release() {
        stdc_impl_t;
        impl.released = true;
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    static bool checkVersionSuffix(const std::string_view &suffix) {
        if (suffix.empty() || suffix.front() == '.' || suffix.back() == '.') {
            return false;
        }
        size_t start = 0;
        while (start < suffix.size()) {
            size_t dotPos = suffix.find('.', start);
            std::string_view part;
            if (dotPos == std::string::npos) {
                part = suffix.substr(start);
                start = suffix.size();
            } else {
                part = suffix.substr(start, dotPos - start);
                start = dotPos + 1;
            }
            if (part.empty() || !std::all_of(part.begin(), part.end(),
                                             [](unsigned char c) { return std::isdigit(c); })) {
                return false;
            }
        }
        return true;
    }
#endif

    bool SharedLibrary::isLibrary(const fs::path &path) {
#if defined(_WIN32)
        auto fileName = path.wstring();
        return fileName.size() >= 4 &&
               std::equal(fileName.end() - 4, fileName.end(), L".dll", [](wchar_t a, wchar_t b) {
                   return ::tolower(a) == ::tolower(b); //
               });
#elif defined(__APPLE__)
        auto fileName = path.string();
        return fileName.size() >= 6 &&
               std::equal(fileName.end() - 6, fileName.end(), L".dylib", [](char a, char b) {
                   return ::tolower(a) == ::tolower(b); //
               });
#else
        auto fileName = path.string();
        size_t soPos;
        if (fileName.size() >= 3 && (soPos = fileName.rfind(".so")) != std::string::npos) {
            // Whatever follows the .so, which is either nothing or a version.
            std::string_view suffix = std::string_view(fileName).substr(soPos + 3);
            if (suffix.empty()) {
                return true; // plain .so
            }
            return suffix.front() == '.' && suffix.size() > 1 &&
                   checkVersionSuffix(suffix.substr(1));
        }
        return false;
#endif
    }

    fs::path SharedLibrary::setLibraryPath(const fs::path &path) {
#ifdef _WIN32
        std::wstring org = winapi::kernel32::GetDllDirectoryW();
        ::SetDllDirectoryW(path.c_str());
#else
        std::string org;
        if (const char *env = std::getenv(PRIOR_LIBRARY_PATH_KEY); env) {
            org = env;
        }
        if (setenv(PRIOR_LIBRARY_PATH_KEY, path.string().c_str(), 1) != 0) {
            return {};
        }
#endif
        return org;
    }

    fs::path SharedLibrary::locateLibraryPath(const void *addr) {
#ifdef _WIN32
        HMODULE hModule = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR) addr, &hModule)) {
            return {};
        }
        return winapi::kernel32::GetModuleFileNameW(hModule);
#else
        Dl_info dl_info{};
        if (!addr || dladdr(const_cast<void *>(addr), &dl_info) == 0 || !dl_info.dli_fname) {
            return {};
        }
        return dl_info.dli_fname;
#endif
    }

}
