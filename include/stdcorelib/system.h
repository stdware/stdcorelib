// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_SYSTEM_H
#define STDCORELIB_SYSTEM_H

#include <string>
#include <vector>
#include <filesystem>
#include <map>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup platform Platform and system
///
/// Information about the program and the machine, obtained from the operating system rather than
/// from \c argv[0].
///
/// \code
///     using namespace stdc;
///
///     auto dir  = system::application_directory();
///     auto args = system::command_line_arguments();    // UTF-8, from the wide command line
///     auto env  = system::environment();               // UTF-8 regardless of the native form
///     auto text = path::to_utf8(dir / "config.json");  // path::string() can lose characters
///     auto tidy = path::clean_path(messy);             // resolves . and .. lexically
/// \endcode
///
/// On Windows, stdc::windows::RegKey and stdc::windows::RegValue read and write the registry. Every
/// operation exists in two forms: one that accepts an \c std::error_code and is \c noexcept, and
/// one without it that throws.
///
/// \code
///     using namespace stdc::windows;
///
///     std::error_code ec;
///     RegKey hklm(RegKey::RK_LocalMachine);
///     RegKey key = hklm.open(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", ec);
///     if (key.isValid()) {
///         auto name = key.value(L"ProductName", ec).toString();
///     }
/// \endcode

namespace stdc {

    /// \addtogroup platform
    /// @{

    namespace system {

        /// \name Program location
        /// @{

        /// Returns the path of the executable, obtained from the operating system rather than
        /// from \c argv[0], which the parent process can set to any value.
        STDC_EXPORT std::filesystem::path application_path();

        /// Returns the directory that contains the executable. Files distributed with the
        /// executable are usually located there.
        STDC_EXPORT std::filesystem::path application_directory();

        /// Returns the file name of the executable, including the extension.
        STDC_EXPORT std::filesystem::path application_filename();

        /// Returns the file name of the executable without its extension, as UTF-8.
        STDC_EXPORT std::string application_name();

        /// @}

        /// \name Command line
        /// @{

        /// Returns the arguments as UTF-8, including \c argv[0].
        ///
        /// \return a view of storage that exists for the lifetime of the process
        /// \note On Windows, the arguments are read from the wide command line. A path that the
        ///       narrow arguments of \c main() cannot represent is therefore preserved.
        STDC_EXPORT array_view<std::string> command_line_arguments();

        /// Splits \a command in the same way as the host platform, reversing the quoting that
        /// join_command_line() applies.
        ///
        /// \sa join_command_line()
        STDC_EXPORT std::vector<std::string> split_command_line(const std::string_view &command);

        /// Joins \a args into one command line and quotes each argument, so that the receiving
        /// program splits the line into the same arguments.
        ///
        /// \sa split_command_line()
        STDC_EXPORT std::string join_command_line(const std::vector<std::string> &args);

        /// Returns whether \a args is short enough for the system to start a program with it.
        ///
        /// Windows builds one string for \c CreateProcess and rejects it beyond 32767
        /// characters. This function therefore quotes \a args in the same way as a process
        /// launcher and measures the result rather than estimating it. POSIX counts the
        /// arguments and the environment together against \c ARG_MAX. The function therefore
        /// reserves half of that limit for the environment, and no single argument may reach the
        /// per-argument limit of 128 KiB that Linux imposes.
        ///
        /// A build system that generates a long command line calls this function first. If the
        /// function returns false, the build system writes the arguments to a response file and
        /// passes \c \@file instead.
        ///
        /// \note Windows counts the UTF-16 code units of the converted arguments, and UTF-8 is
        ///       never shorter than the UTF-16 form of the same text. Counting bytes can
        ///       therefore reject a line that the system accepts, but it never accepts a line
        ///       that the system rejects, which is the relevant direction.
        /// \note A return value of true does not guarantee that the program starts, only that
        ///       the start does not fail because of the length. Both limits are applied
        ///       conservatively.
        /// \sa cli::Parser::EnableResponseFile
        STDC_EXPORT bool command_line_fits(const std::vector<std::string> &args);

        /// @}

        /// Returns the environment of the current process as UTF-8.
        STDC_EXPORT std::map<std::string, std::string> environment();

    }

    /// @}
}

#endif // STDCORELIB_SYSTEM_H
