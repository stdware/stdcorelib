// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_PATH_H
#define STDCORELIB_PATH_H

#include <string>
#include <filesystem>

#include <stdcorelib/str.h>

namespace stdc {

    /// \addtogroup platform
    /// @{

    namespace path {

        /// Returns the path that the UTF-8 string \a s denotes.
        ///
        /// \note On Windows, \c std::filesystem::path interprets a narrow string in the ANSI code
        ///       page and corrupts every character outside it. This function avoids that
        ///       conversion.
        inline std::filesystem::path from_utf8(const std::string_view &s) {
#ifdef _WIN32
            return wstring_conv::from_utf8(s);
#else
            return s;
#endif
        }

        /// Returns \a path as UTF-8. On Windows, \c path::string() loses characters for the same
        /// reason.
        inline std::string to_utf8(const std::filesystem::path &path) {
#ifdef _WIN32
            return wstring_conv::to_utf8(path.wstring());
#else
            return path.string();
#endif
        }

        inline std::string to_utf8(const std::filesystem::path::string_type &path) {
#ifdef _WIN32
            return wstring_conv::to_utf8(path);
#else
            return path;
#endif
        }

        /// Returns the canonical form of \a path like \c std::filesystem::canonical, without
        /// throwing. The function reads the file system and resolves symbolic links.
        ///
        /// \return the resolved path, or an empty path on failure
        /// \pre \a path exists.
        /// \sa clean_path()
        inline std::filesystem::path canonical(const std::filesystem::path &path) {
            std::error_code ec;
            return std::filesystem::canonical(path, ec);
        }

        /// Resolves <tt>\.</tt> and <tt>\.\.</tt> lexically, without reading the file system. The
        /// function therefore also accepts a path that does not exist.
        ///
        /// \warning For a symbolic link followed by \c .., the result can differ from the result
        ///          of canonical(), because the lexical resolution ignores the target of the link.
        STDC_EXPORT std::filesystem::path clean_path(const std::filesystem::path &path);

        /// Returns \a path with every separator written as \c /, or as the native separator if
        /// \a native is true.
        inline std::string normalize_separators(const std::filesystem::path &path,
                                                bool native = false) {
            return str::conv<std::filesystem::path>::normalize_separators(to_utf8(path), native);
        }

    }

    using path::clean_path;
    using path::normalize_separators;

    /// @}
}

#endif // STDCORELIB_PATH_H
