// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_SHAREDLIBRARY_H
#define STDCORELIB_SHAREDLIBRARY_H

#include <memory>
#include <filesystem>
#include <system_error>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/flags.h>

namespace stdc {

    /// \addtogroup process
    /// @{

    /// Loads a shared library at run time and resolves symbols from it, through \c LoadLibraryEx
    /// on Windows and \c dlopen on other platforms.
    ///
    /// The library is unloaded when the object is destroyed, unless release() has released the
    /// ownership of the handle.
    ///
    /// \code
    ///   SharedLibrary lib;
    ///   if (!lib.open("/usr/lib/x86_64-linux-gnu/libc.so.6")) {
    ///       return lib.errorMessage();
    ///   }
    ///   auto fn = reinterpret_cast<void *(*) (size_t)>(lib.resolve("malloc"));
    /// \endcode
    class STDC_EXPORT SharedLibrary {
    public:
        SharedLibrary();
        ~SharedLibrary();

        SharedLibrary(SharedLibrary &&RHS) noexcept;
        SharedLibrary &operator=(SharedLibrary &&RHS) noexcept;

    public:
        /// The hints for open(). Most hints correspond to a \c dlopen flag, and some correspond to
        /// a request to the native loader of a platform. A platform ignores a hint for which it
        /// has no corresponding request.
        enum LoadHint {
            /// Resolves all undefined symbols while loading the library. Maps to \c RTLD_NOW
            /// on POSIX.
            ResolveAllSymbolsHint = 0x01,
            /// Makes the symbols of the library available to libraries loaded later. Maps to
            /// \c RTLD_GLOBAL on POSIX.
            ExportExternalSymbolsHint = 0x02,
            /// Reserved for loading a member of an archive library. Currently ignored.
            LoadArchiveMemberHint = 0x04,
            /// Keeps the library loaded for the lifetime of the process. Uses \c RTLD_NODELETE on
            /// POSIX and pins the module on Windows. If no safe loader flag exists, as on macOS,
            /// open() calls release() after a successful load. A failure to pin does not make
            /// open() fail.
            PreventUnloadHint = 0x08,
            /// Prefers the symbols of this library over previously loaded global symbols when
            /// resolving references. Maps to \c RTLD_DEEPBIND if available.
            DeepBindHint = 0x10,
            /// Searches the directory of the library while resolving its dependencies, as
            /// required by a plugin that keeps its private libraries in its own directory.
            /// The hint applies only on Windows and corresponds to
            /// \c LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR.
            SearchLibraryLoadDirectoryHint = 0x20,
        };
        STDC_DECLARE_FLAGS(LoadHints, LoadHint)

    public:
        /// \name Loading
        /// @{

        /// Loads \a path.
        ///
        /// \param path the library to load
        /// \param hints the requests to the loader
        /// \retval false no library was loaded, and errorMessage() contains the reason
        /// \note If the object is already open, it remains unchanged and the function fails.
        ///       Replacing one library with another therefore requires close() first.
        bool open(const std::filesystem::path &path, LoadHints hints = {});

        /// Unloads the library.
        ///
        /// \note The operating system unloads a library only after its last user releases it.
        ///       Code and static data of the library can therefore remain mapped afterwards.
        bool close();

        /// Returns the address exported under \a name.
        ///
        /// \return the address of the symbol, or null with the reason in errorMessage()
        /// \pre The library is open.
        void *resolve(const char *name) const;

        /// Releases ownership of the handle, so that the destructor does not unload the library.
        ///
        /// \note The handle remains valid and is never closed. This is the purpose of the
        ///       function, and also a leak if the release was not intended.
        void release();

        /// @}

    public:
        /// \name Failures
        ///
        /// Both functions describe the last operation, and both are cleared when the next
        /// operation begins.
        /// @{

        /// Returns the reason for the failure of the last operation on this object, or an empty
        /// string if the operation succeeded.
        ///
        /// The reason is recorded when the failure occurs rather than read from the system on
        /// request, so that no intervening call can overwrite it.
        std::string errorMessage() const;

        /// Returns the same failure as an error code.
        ///
        /// \note The \c dl functions report no error code. For a failed open(), the code is
        ///       therefore derived from the path. A missing symbol has no path to examine and is
        ///       reported only as text.
        std::error_code errorCode() const;

        /// @}

    public:
        /// \name Properties
        /// @{

        bool isOpen() const;

        /// Returns the path with which the library was opened, or an empty path if no library
        /// is open.
        std::filesystem::path path() const;

        /// Returns the underlying \c HMODULE or \c dlopen handle, for the platform calls that
        /// this class does not wrap.
        void *handle() const;

        /// @}

    public:
        /// \name Paths
        ///
        /// Queries and settings of library locations that require no open library.
        /// @{

        /// Returns whether \a path has a suffix that this platform can load: \c .dll on Windows,
        /// \c .dylib on macOS, and \c .so with an optional numeric version on other platforms.
        ///
        /// \note The function examines only the name. It does not determine whether the file
        ///       exists or is loadable.
        static bool isLibrary(const std::filesystem::path &path);

        /// Sets the directory in which the system searches for the dependencies of a library, to
        /// the extent that each system allows this setting at run time. The effect differs
        /// between platforms and is therefore described per platform.
        ///
        /// \li <b>Windows</b>: \c SetDllDirectoryW. Every subsequent \c LoadLibrary of this
        ///     process searches the directory, so that a plugin outside the usual directories
        ///     finds the libraries beside it. This is the intended use of the function.
        /// \li <b>Linux</b>: \c LD_LIBRARY_PATH is set, but the loader read it once at process
        ///     start. The function therefore <b>does not change the search path of this
        ///     process</b>. It changes only the environment that a child process inherits, which
        ///     is useful but differs from what the name suggests. A measurement confirmed this:
        ///     a library whose dependency is located only in the new directory still fails to
        ///     load after the call, and the same binary loads it if the variable was set before
        ///     the process started.
        /// \li <b>macOS</b>: \c DYLD_LIBRARY_PATH is set. The function <b>does not change the
        ///     search path of this process</b>. As on Linux, it changes only the environment that
        ///     a child process inherits. A measurement confirmed this with a dependent library
        ///     that failed to load after a call at run time and loaded if the same path was set
        ///     at process start.
        ///
        /// A plugin that must find its neighboring libraries on every platform requires an rpath
        /// of \c $ORIGIN or <tt>\@loader_path</tt>. The rpath is a property of the plugin, which
        /// a caller cannot set through this function.
        ///
        /// \return the previous value, so that it can be restored. An empty path unsets the
        ///         variable rather than setting it to an empty value. Restoring the value that a
        ///         process without the variable receives therefore leaves the variable unset.
        static std::filesystem::path setLibraryPath(const std::filesystem::path &path);

        /// Returns the path of the library that contains \a addr. The result identifies the
        /// library from which a given function was loaded.
        ///
        /// \sa resolve()
        static std::filesystem::path locateLibraryPath(const void *addr);

        /// @}

    protected:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    STDC_DECLARE_OPERATORS_FOR_FLAGS(SharedLibrary::LoadHints)

    /// @}
}

#endif // STDCORELIB_SHAREDLIBRARY_H
