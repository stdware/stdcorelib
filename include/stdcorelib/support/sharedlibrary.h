// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_SHAREDLIBRARY_H
#define STDCORELIB_SHAREDLIBRARY_H

#include <memory>
#include <filesystem>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/flags.h>

namespace stdc {

    /// \addtogroup process
    /// @{

    /// Loads a shared library at run time and resolves symbols from it. \c LoadLibraryEx on
    /// Windows, \c dlopen elsewhere.
    ///
    /// The library is unloaded when the object goes away, unless release() has given up
    /// ownership of the handle.
    ///
    /// \code
    ///   SharedLibrary lib;
    ///   if (!lib.open("/usr/lib/x86_64-linux-gnu/libc.so.6")) {
    ///       return lib.lastError();
    ///   }
    ///   auto fn = reinterpret_cast<void *(*) (size_t)>(lib.resolve("malloc"));
    /// \endcode
    class STDC_EXPORT SharedLibrary {
    public:
        SharedLibrary();
        ~SharedLibrary();

        SharedLibrary(SharedLibrary &&other) noexcept;
        SharedLibrary &operator=(SharedLibrary &&other) noexcept;

    public:
        /// Passed to open(). These map onto the \c dlopen flags and are ignored where the
        /// platform has no equivalent.
        enum LoadHint {
            ResolveAllSymbolsHint = 0x01,
            ExportExternalSymbolsHint = 0x02,
            LoadArchiveMemberHint = 0x04, // Unused
            /// The library stays for the life of the process, whatever anyone does with the
            /// object that loaded it. \c RTLD_NODELETE where there is one, and the module
            /// pinned on Windows.
            ///
            /// \note A wish rather than a promise. Where the loader refuses to pin, open()
            ///       still succeeds with the library loaded and unloadable as usual, so a
            ///       caller that has to know cannot learn it from here.
            PreventUnloadHint = 0x08,
            DeepBindHint = 0x10,
        };
        STDC_DECLARE_FLAGS(LoadHints, LoadHint)

        /// Loads \a path.
        ///
        /// \param path the library to load
        /// \param hints what to ask the loader for
        /// \retval false nothing was loaded, with the reason in lastError()
        /// \note An object that is already open is left alone and this fails, so close() first
        ///       to swap one library for another.
        bool open(const std::filesystem::path &path, LoadHints hints = {});

        /// Unloads the library.
        ///
        /// \note The OS only really unloads it once the last user has let go, so code and
        ///       statics from it may well still be mapped afterwards.
        bool close();

        bool isOpen() const;

        /// The path this was opened with, empty if it is not open.
        std::filesystem::path path() const;

        /// The underlying \c HMODULE or \c dlopen handle, for the platform calls this class does
        /// not wrap.
        void *handle() const;

        /// The address exported under \a name.
        ///
        /// \return the symbol's address, or null with the reason in lastError()
        /// \pre The library is open.
        void *resolve(const char *name) const;

        /// The reason the last operation on this object failed, or empty if it succeeded.
        ///
        /// Captured where the failure happened rather than read from the system on demand, so
        /// nothing that ran in between can have overwritten it.
        std::string lastError() const;

        /// Gives up ownership, so the destructor will not unload.
        ///
        /// \note The handle stays valid and nobody will close it, which is the point and also
        ///       the leak if that was not intended.
        void release();

        /// Whether \a path has a suffix this platform can load: \c .dll on Windows, \c .dylib on
        /// macOS, and \c .so with an optional numeric version behind it elsewhere.
        ///
        /// \note This reads the name only. It says nothing about whether the file exists or is
        ///       loadable.
        static bool isLibrary(const std::filesystem::path &path);

        /// Where the system should look for a library's own dependencies, as far as each
        /// system lets that be said at run time. This is not one behavior with three spellings,
        /// so what it is worth is written out per platform.
        ///
        /// \li <b>Windows</b>: \c SetDllDirectoryW. Every \c LoadLibrary this process makes
        ///     afterwards looks here, which is what lets a plugin outside the usual directories
        ///     find the libraries next to it. This is the case the function was written for.
        /// \li <b>Linux</b>: \c LD_LIBRARY_PATH is set, and the loader read it once when the
        ///     process started. So this <b>does not change where this process looks</b>. What
        ///     it changes is the environment a child inherits, which is worth something and is
        ///     not what the name suggests. Measured, not assumed: a library whose dependency
        ///     sits only in the new directory still fails to load after this returns, and the
        ///     same binary loads it when the variable was set before it started.
        /// \li <b>macOS</b>: \c DYLD_LIBRARY_PATH is set. Whether that changes where the current
        ///     process looks has not been measured, so this function makes no such promise.
        ///
        /// A plugin that has to find its own neighbors on every platform wants an rpath of
        /// \c $ORIGIN or <tt>\@loader_path</tt>, which is a property of the plugin rather than
        /// anything a caller can set from here.
        ///
        /// \return what was set before, so it can be put back. An empty path unsets the
        ///         variable rather than setting it to nothing, so putting back what a process
        ///         that never had one hands out leaves it without one.
        static std::filesystem::path setLibraryPath(const std::filesystem::path &path);

        /// The path of the library that \a addr falls inside, which answers where a given
        /// function came from.
        ///
        /// \sa resolve()
        static std::filesystem::path locateLibraryPath(const void *addr);

    protected:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    STDC_DECLARE_OPERATORS_FOR_FLAGS(SharedLibrary::LoadHints)

    /// @}
}

#endif // STDCORELIB_SHAREDLIBRARY_H
