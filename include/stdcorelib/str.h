// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_STR_H
#define STDCORELIB_STR_H

#include <string>
#include <string_view>
#include <sstream>
#include <vector>
#include <type_traits>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <algorithm>
#include <functional>
#include <system_error>
#include <tuple>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup text Text
///
/// Strings, formatting, console output and UTF conversion.
///
/// \code
///     using namespace stdc;
///
///     auto msg  = formatN("%1 took %2 ms", name, elapsed);  // each argument keeps its type
///     auto head = str::trim(str::split(line, ",").front());
///     auto path = str::join({"usr", "local", "bin"}, "/");
///     auto full = str::varexp("${HOME}/config", env);       // ${VAR}, nesting, $$ escapes
/// \endcode
///
/// \c formatN accepts every type that \c str::to_string supports, including
/// \c std::filesystem::path and wide strings. No conversion is therefore required at the call
/// site.
///
/// The console functions write attributes and write UTF-8 that a Windows console displays
/// correctly. Whether escape sequences are emitted is determined per target file. Output
/// redirected to a file therefore contains only the text.
///
/// \code
///     console::printf(console::bold, console::lightgreen, console::nocolor, "%d passed\n", n);
///     console::warning("%1 is deprecated, use %2", old_name, new_name);
///     u8println("plain UTF-8, transcoded for the console if required");
///
///     // Alternatively, the attributes are written inside the string.
///     cprintln("${lightgreen}ok ${@blue bold}on blue ${reset}plain, 50$$ off");
/// \endcode
///
/// \c console::set_color_mode() applies a \c --color=always option or the \c NO_COLOR variable,
/// and \c console::width() returns the width of the terminal.
///
/// The conversions in \ref utf.h are the basis of this module. They follow the Unicode
/// substitution rule: one replacement character per maximal subpart of an ill-formed sequence, not
/// one per byte.

namespace stdc {

    /// \addtogroup text
    /// @{

    namespace str {

        template <class T>
        struct conv;

        template <>
        struct conv<std::string> {
            inline std::string operator()(const std::string &s) const {
                return s;
            }

            // ##FIXME: remove?
            inline std::string operator()(std::string &&s) const {
                return s;
            }
        };

        template <>
        struct conv<std::string_view> {
            inline std::string operator()(const std::string_view &s) const {
                return {s.data(), s.size()};
            }
        };

        template <>
        struct conv<char *> {
            inline std::string operator()(const char *s) const {
                return s;
            }
        };

        template <>
        struct conv<std::wstring> {
            inline std::string operator()(const std::wstring &s) const {
                return to_utf8(s);
            }

            /// \name UTF-8
            ///
            /// Text that is invalid in its encoding converts to an empty string. \a size specifies
            /// the length of text that is not null-terminated.
            ///
            /// utf::utf8_to_wide() and utf::wide_to_utf8() can instead replace each invalid
            /// sequence with U+FFFD rather than reject the whole string.
            ///
            /// \sa utf::utf8_to_wide(), utf::wide_to_utf8()
            /// @{

            STDC_EXPORT static std::wstring from_utf8(const char *s, int size = -1);

            static inline std::wstring from_utf8(const std::string_view &s) {
                return from_utf8(s.data(), int(s.size()));
            }

            STDC_EXPORT static std::string to_utf8(const wchar_t *s, int size = -1);

            static inline std::string to_utf8(const std::wstring_view &s) {
                return to_utf8(s.data(), int(s.size()));
            }

            /// @}

#ifdef _WIN32
            /// \name ANSI
            ///
            /// The same conversions for the process code page instead of UTF-8. The code page is
            /// a Windows concept that requires a Windows API call. These functions therefore
            /// exist only on Windows.
            /// @{

            STDC_EXPORT static std::wstring from_ansi(const char *s, int size = -1);

            static inline std::wstring from_ansi(const std::string_view &s) {
                return from_ansi(s.data(), int(s.size()));
            }

            STDC_EXPORT static std::string to_ansi(const wchar_t *s, int size = -1);

            static inline std::string to_ansi(const std::wstring_view &s) {
                return to_ansi(s.data(), int(s.size()));
            }

            /// @}
#endif
        };

        template <>
        struct conv<std::wstring_view> {
            inline std::string operator()(const std::wstring_view &s) const {
                return conv<std::wstring>::to_utf8(s.data(), int(s.size()));
            }
        };

        template <>
        struct conv<wchar_t *> {
            inline std::string operator()(const wchar_t *s) const {
                return conv<std::wstring>::to_utf8(s);
            }
        };

        template <>
        struct conv<std::filesystem::path> {
            inline std::string operator()(const std::filesystem::path &path) const {
#ifdef _WIN32
                return normalize_separators(conv<std::wstring>::to_utf8(path.wstring()), true);
#else
                return normalize_separators(path.string(), true);
#endif
            }

            STDC_EXPORT static std::string normalize_separators(const std::string &utf8_path,
                                                                bool native);
        };

        /// Returns \a t as UTF-8 text.
        ///
        /// The function supports the arithmetic types, \c char, \c wchar_t, and every type for
        /// which \c str::conv has a specialization. formatN() therefore accepts a path or a wide
        /// string without a conversion by the caller.
        template <class T>
        std::string to_string(T &&t) {
            using T1 = std::remove_reference_t<T>;
            if constexpr (std::is_pointer_v<T1>) {
                using T2 =
                    std::add_pointer_t<std::decay_t<std::remove_cv_t<std::remove_pointer_t<T1>>>>;
                return str::conv<T2>()(t);
            } else {
                using T2 = std::decay_t<std::remove_cv_t<T1>>;
                if constexpr (std::is_same_v<T2, bool>) {
                    return t ? "true" : "false";
                } else if constexpr (std::is_same_v<T2, char>) {
                    return std::string(1, t);
                } else if constexpr (std::is_same_v<T2, wchar_t>) {
                    return conv<std::wstring>::to_utf8(&t, 1);
                } else if constexpr (std::is_integral_v<T2>) {
                    return std::to_string(t);
                } else if constexpr (std::is_floating_point_v<T2>) {
                    std::ostringstream oss;
                    oss << std::noshowpoint << t;
                    return oss.str();
                } else {
                    return str::conv<T2>()(std::forward<T>(t));
                }
            }
        }

        /// Concatenates the elements of \a v with \a delimiter between them.
        STDC_EXPORT std::string join(const array_view<std::string> &v,
                                     const std::string_view &delimiter);

        /// \overload
        STDC_EXPORT std::string join(const array_view<std::string_view> &v,
                                     const std::string_view &delimiter);

        /// \overload
        inline std::string join(std::initializer_list<std::string_view> v,
                                const std::string_view &delimiter) {
            return join(array_view<std::string_view>(v.begin(), v.size()), delimiter);
        }

        /// Splits \a s at every occurrence of \a delimiter and keeps empty fields.
        ///
        /// \return the fields, at least one. An empty \a s produces one empty field.
        /// \warning The views point into \a s, which must therefore outlive them. The overload
        ///          that accepts an rvalue \c std::string returns copies instead, because its
        ///          argument does not outlive the call.
        STDC_EXPORT std::vector<std::string_view> split(const std::string_view &s,
                                                        const std::string_view &delimiter);

        /// \overload
        STDC_EXPORT std::vector<std::string> split(std::string &&s,
                                                   const std::string_view &delimiter);

        /// \overload
        inline std::vector<std::string_view> split(const char *s,
                                                   const std::string_view &delimiter) {
            return split(std::string_view(s), delimiter);
        }

        /// Replaces \c %1, \c %2, ... in \a fmt with the elements of \a args, counting from one.
        ///
        /// \note A placeholder without a corresponding argument remains unchanged.
        STDC_EXPORT std::string format(const std::string_view &fmt,
                                       const array_view<std::string> &args);

        /// Calls format() with the given arguments, each converted by to_string() first.
        ///
        /// The placeholders are \c %1, \c %2 rather than printf conversions. Each argument
        /// therefore keeps its own type, and no conversion specifier can mismatch it.
        ///
        /// \code
        ///   formatN("%1 took %2 ms", name, elapsed);
        /// \endcode
        ///
        /// \sa format(), to_string()
        template <class Arg1, class... Args>
        std::string formatN(const std::string_view &fmt, Arg1 &&arg1, Args &&...args) {
            return format(fmt, {
                                   to_string(std::forward<Arg1>(arg1)),
                                   to_string(std::forward<Args>(args))...,
                               });
        }

        /// \overload
        inline std::string formatN(const std::string_view &fmt) {
            return std::string(fmt);
        }

        /// \overload
        inline std::string formatN(std::string &&fmt) {
            return fmt;
        }

        /// \overload
        inline std::string formatN(const char *fmt) {
            return fmt;
        }

        /// Expands every \c ${name} in \a s with the value that \a find returns for the name.
        ///
        /// \c $$ writes one literal \c $, so that \c $${A} remains \c ${A}. A \c $ without a
        /// following brace is literal. Names can be nested, and inner names are expanded first.
        /// \c ${${A}_${B}} therefore looks up the name that the two expansions form.
        ///
        /// \param s the text to expand
        /// \param find the function that returns the value of each name. An empty value removes
        ///        the reference from the text.
        /// \return the expanded text, or an empty string if a brace is unbalanced
        STDC_EXPORT std::string
            varexp(const std::string_view &s,
                   const std::function<std::string(const std::string_view &)> &find);

        /// \overload
        template <template <class, class, class...> class MAP, class K, class V, class... MODS>
        inline std::string varexp(const std::string_view &s, const MAP<K, V, MODS...> &vars) {
            return varexp(s, [&vars](const std::string_view &name) -> std::string {
                auto it = vars.find(std::string(name));
                if (it == vars.end())
                    return std::string();
                return it->second;
            });
        }

    }

    using str::to_string;
    using str::join;
    using str::split;
    using str::format;
    using str::formatN;

    using wstring_conv = str::conv<std::wstring>;

    namespace str {

        /// \defgroup ascii ASCII
        /// \ingroup text
        ///
        /// Classification and case folding, restricted to the ASCII range.
        ///
        /// The results of the C library depend on the current locale. The input that a program
        /// accepts would then depend on the machine on which it runs, and the functions are
        /// undefined for a negative plain \c char. This library reads machine syntax rather than
        /// human text, and it reads it as UTF-8, in which every byte of a non-ASCII character is
        /// negative. Folding these bytes individually cannot produce a correct result. These
        /// functions therefore leave them unchanged.
        /// @{

        constexpr bool is_digit(char c) noexcept {
            return c >= '0' && c <= '9';
        }

        /// \overload
        constexpr bool is_digit(wchar_t c) noexcept {
            return c >= L'0' && c <= L'9';
        }

        constexpr bool is_hex_digit(char c) noexcept {
            return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        /// \overload
        constexpr bool is_hex_digit(wchar_t c) noexcept {
            return is_digit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        }

        constexpr bool is_lower(char c) noexcept {
            return c >= 'a' && c <= 'z';
        }

        /// \overload
        constexpr bool is_lower(wchar_t c) noexcept {
            return c >= L'a' && c <= L'z';
        }

        constexpr bool is_upper(char c) noexcept {
            return c >= 'A' && c <= 'Z';
        }

        /// \overload
        constexpr bool is_upper(wchar_t c) noexcept {
            return c >= L'A' && c <= L'Z';
        }

        constexpr bool is_alpha(char c) noexcept {
            return is_lower(c) || is_upper(c);
        }

        /// \overload
        constexpr bool is_alpha(wchar_t c) noexcept {
            return is_lower(c) || is_upper(c);
        }

        constexpr bool is_alnum(char c) noexcept {
            return is_alpha(c) || is_digit(c);
        }

        /// \overload
        constexpr bool is_alnum(wchar_t c) noexcept {
            return is_alpha(c) || is_digit(c);
        }

        /// Returns whether \a c is a space, tab, newline, vertical tab, form feed or carriage
        /// return.
        constexpr bool is_space(char c) noexcept {
            return c == ' ' || (c >= '\t' && c <= '\r');
        }

        /// \overload
        constexpr bool is_space(wchar_t c) noexcept {
            return c == L' ' || (c >= L'\t' && c <= L'\r');
        }

        /// Returns whether \a c occupies a position of its own on the screen, including the
        /// space.
        constexpr bool is_print(char c) noexcept {
            return c >= ' ' && c < '\x7F';
        }

        /// \overload
        constexpr bool is_print(wchar_t c) noexcept {
            return c >= L' ' && c < L'\x7F';
        }

        /// Returns whether \a c is printable and is neither a letter, a digit, nor the space.
        constexpr bool is_punct(char c) noexcept {
            return is_print(c) && c != ' ' && !is_alnum(c);
        }

        /// \overload
        constexpr bool is_punct(wchar_t c) noexcept {
            return is_print(c) && c != L' ' && !is_alnum(c);
        }

        constexpr char to_lower(char c) noexcept {
            return is_upper(c) ? char(c - 'A' + 'a') : c;
        }

        /// \overload
        constexpr wchar_t to_lower(wchar_t c) noexcept {
            return is_upper(c) ? wchar_t(c - L'A' + L'a') : c;
        }

        constexpr char to_upper(char c) noexcept {
            return is_lower(c) ? char(c - 'a' + 'A') : c;
        }

        /// \overload
        constexpr wchar_t to_upper(wchar_t c) noexcept {
            return is_lower(c) ? wchar_t(c - L'a' + L'A') : c;
        }

        /// Returns the value of the hexadecimal digit \a c, or -1 if \a c is not a hexadecimal
        /// digit.
        constexpr int hex_value(char c) noexcept {
            if (is_digit(c)) {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        }

        /// Compares two views like \c strcasecmp(), folding only the ASCII letters.
        ///
        /// \return a negative number if \a LHS sorts before \a RHS, zero if both sort equally, or
        ///         a positive number if \a LHS sorts after \a RHS
        STDC_EXPORT int compare_insensitive(const std::string_view &LHS,
                                            const std::string_view &RHS);

        inline bool equals_insensitive(const std::string_view &LHS, const std::string_view &RHS) {
            if (LHS.size() != RHS.size()) {
                return false;
            }
            for (size_t i = 0; i < LHS.size(); ++i) {
                if (to_lower(LHS[i]) != to_lower(RHS[i])) {
                    return false;
                }
            }
            return true;
        }

        /// \overload
        inline bool equals_insensitive(const std::wstring_view &LHS, const std::wstring_view &RHS) {
            if (LHS.size() != RHS.size()) {
                return false;
            }
            for (size_t i = 0; i < LHS.size(); ++i) {
                if (to_lower(LHS[i]) != to_lower(RHS[i])) {
                    return false;
                }
            }
            return true;
        }

        inline std::string to_upper(std::string s) {
            std::ignore =
                std::transform(s.begin(), s.end(), s.begin(), [](char c) { return to_upper(c); });
            return s;
        }

        /// \overload
        inline std::wstring to_upper(std::wstring s) {
            std::ignore = std::transform(s.begin(), s.end(), s.begin(),
                                         [](wchar_t c) { return to_upper(c); });
            return s;
        }

        inline std::string to_lower(std::string s) {
            std::ignore =
                std::transform(s.begin(), s.end(), s.begin(), [](char c) { return to_lower(c); });
            return s;
        }

        /// \overload
        inline std::wstring to_lower(std::wstring s) {
            std::ignore = std::transform(s.begin(), s.end(), s.begin(),
                                         [](wchar_t c) { return to_lower(c); });
            return s;
        }

        /// @}
    }

    using str::compare_insensitive;
    using str::equals_insensitive;
    using str::to_lower;
    using str::to_upper;

    namespace str {

        /// Returns whether \a s begins with \a prefix.
        ///
        /// If \a case_insensitive is true, the ASCII letters of both strings are folded before the
        /// comparison, and every other byte is compared unchanged.
        inline bool starts_with(const std::string_view &s, const std::string_view &prefix,
                                bool case_insensitive = false) {
            if (case_insensitive) {
                return s.size() >= prefix.size() &&
                       equals_insensitive(s.substr(0, prefix.size()), prefix);
            }
#if __cplusplus >= 202002L
            return s.starts_with(prefix);
#else
            return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
#endif
        }

        /// \overload
        inline bool starts_with(const std::string_view &s, char prefix,
                                bool case_insensitive = false) {
            if (s.empty()) {
                return false;
            }
            return case_insensitive ? to_lower(s.front()) == to_lower(prefix) : s.front() == prefix;
        }

        /// \overload
        inline bool starts_with(const std::wstring_view &s, const std::wstring_view &prefix,
                                bool case_insensitive = false) {
            if (case_insensitive) {
                return s.size() >= prefix.size() &&
                       equals_insensitive(s.substr(0, prefix.size()), prefix);
            }
#if __cplusplus >= 202002L
            return s.starts_with(prefix);
#else
            return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
#endif
        }

        /// \overload
        inline bool starts_with(const std::wstring_view &s, wchar_t prefix,
                                bool case_insensitive = false) {
            if (s.empty()) {
                return false;
            }
            return case_insensitive ? to_lower(s.front()) == to_lower(prefix) : s.front() == prefix;
        }

        /// Returns whether \a s ends with \a suffix.
        ///
        /// If \a case_insensitive is true, the ASCII letters of both strings are folded before the
        /// comparison, and every other byte is compared unchanged.
        inline bool ends_with(const std::string_view &s, const std::string_view &suffix,
                              bool case_insensitive = false) {
            if (case_insensitive) {
                return s.size() >= suffix.size() &&
                       equals_insensitive(s.substr(s.size() - suffix.size()), suffix);
            }
#if __cplusplus >= 202002L
            return s.ends_with(suffix);
#else
            return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
#endif
        }

        /// \overload
        inline bool ends_with(const std::string_view &s, char suffix,
                              bool case_insensitive = false) {
            if (s.empty()) {
                return false;
            }
            return case_insensitive ? to_lower(s.back()) == to_lower(suffix) : s.back() == suffix;
        }

        /// \overload
        inline bool ends_with(const std::wstring_view &s, const std::wstring_view &suffix,
                              bool case_insensitive = false) {
            if (case_insensitive) {
                return s.size() >= suffix.size() &&
                       equals_insensitive(s.substr(s.size() - suffix.size()), suffix);
            }
#if __cplusplus >= 202002L
            return s.ends_with(suffix);
#else
            return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
#endif
        }

        /// \overload
        inline bool ends_with(const std::wstring_view &s, wchar_t suffix,
                              bool case_insensitive = false) {
            if (s.empty()) {
                return false;
            }
            return case_insensitive ? to_lower(s.back()) == to_lower(suffix) : s.back() == suffix;
        }

        inline std::string_view drop_front(const std::string_view &s, size_t n = 1) {
            return s.substr(n);
        }

        /// \overload
        inline std::string drop_front(std::string &&s, size_t n = 1) {
            return s.substr(n);
        }

        /// \overload
        inline std::string_view drop_front(const char *s, size_t n = 1) {
            return drop_front(std::string_view(s), n);
        }

        inline std::string_view drop_back(const std::string_view &s, size_t n = 1) {
            return s.substr(0, s.size() - n);
        }

        /// \overload
        inline std::string drop_back(std::string &&s, size_t n = 1) {
            return s.substr(0, s.size() - n);
        }

        /// \overload
        inline std::string_view drop_back(const char *s, size_t n = 1) {
            return drop_back(std::string_view(s), n);
        }

        inline std::string_view ltrim(const std::string_view &s, char c) {
            return drop_front(s, std::min(s.size(), s.find_first_not_of(c)));
        }

        /// \overload
        inline std::string ltrim(std::string &&s, char c) {
            return std::string(
                drop_front(std::string_view(s), std::min(s.size(), s.find_first_not_of(c))));
        }

        /// \overload
        inline std::string_view ltrim(const std::string_view &s,
                                      const std::string_view &chars = " \t\n\v\f\r") {
            return drop_front(s, std::min(s.size(), s.find_first_not_of(chars)));
        }

        /// \overload
        inline std::string ltrim(std::string &&s, const std::string_view &chars = " \t\n\v\f\r") {
            return std::string(
                drop_front(std::string_view(s), std::min(s.size(), s.find_first_not_of(chars))));
        }

        /// \overload
        inline std::string_view ltrim(const char *s, char c) {
            return ltrim(std::string_view(s), c);
        }

        /// \overload
        inline std::string_view ltrim(const char *s,
                                      const std::string_view &chars = " \t\n\v\f\r") {
            return ltrim(std::string_view(s), chars);
        }

        inline std::string_view rtrim(const std::string_view &s, char c) {
            return drop_back(s, s.size() - std::min(s.size(), s.find_last_not_of(c) + 1));
        }

        /// \overload
        inline std::string rtrim(std::string &&s, char c) {
            return std::string(drop_back(std::string_view(s),
                                         s.size() - std::min(s.size(), s.find_last_not_of(c) + 1)));
        }

        /// \overload
        inline std::string_view rtrim(const std::string_view &s,
                                      const std::string_view &chars = " \t\n\v\f\r") {
            return drop_back(s, s.size() - std::min(s.size(), s.find_last_not_of(chars) + 1));
        }

        /// \overload
        inline std::string rtrim(std::string &&s, const std::string_view &chars = " \t\n\v\f\r") {
            return std::string(drop_back(
                std::string_view(s), s.size() - std::min(s.size(), s.find_last_not_of(chars) + 1)));
        }

        /// \overload
        inline std::string_view rtrim(const char *s, char c) {
            return rtrim(std::string_view(s), c);
        }

        /// \overload
        inline std::string_view rtrim(const char *s,
                                      const std::string_view &chars = " \t\n\v\f\r") {
            return rtrim(std::string_view(s), chars);
        }

        inline std::string_view trim(const std::string_view &s, char c) {
            return rtrim(ltrim(s, c), c);
        }

        /// \overload
        inline std::string trim(std::string &&s, char c) {
            return std::string(rtrim(ltrim(std::string_view(s), c), c));
        }

        /// \overload
        inline std::string_view trim(const std::string_view &s,
                                     std::string_view chars = " \t\n\v\f\r") {
            return rtrim(ltrim(s, chars), chars);
        }

        /// \overload
        inline std::string trim(std::string &&s, std::string_view chars = " \t\n\v\f\r") {
            return std::string(rtrim(ltrim(std::string_view(s), chars), chars));
        }

        /// \overload
        inline std::string_view trim(const char *s, char c) {
            return trim(std::string_view(s), c);
        }

        /// \overload
        inline std::string_view trim(const char *s, std::string_view chars = " \t\n\v\f\r") {
            return trim(std::string_view(s), chars);
        }

        /// Returns whether \a sub occurs anywhere in \a s.
        ///
        /// If \a case_insensitive is true, the ASCII letters of both strings are folded before the
        /// comparison, and every other byte is compared unchanged.
        inline bool contains(const std::string_view &s, const std::string_view &sub,
                             bool case_insensitive = false) {
            if (!case_insensitive) {
                return s.find(sub) != std::string_view::npos;
            }
            if (sub.size() > s.size()) {
                return false;
            }
            for (size_t i = 0; i + sub.size() <= s.size(); ++i) {
                if (equals_insensitive(s.substr(i, sub.size()), sub)) {
                    return true;
                }
            }
            return false;
        }

        /// \overload
        inline bool contains(const std::string_view &s, char c, bool case_insensitive = false) {
            if (!case_insensitive) {
                return s.find(c) != std::string_view::npos;
            }
            for (char item : s) {
                if (to_lower(item) == to_lower(c)) {
                    return true;
                }
            }
            return false;
        }

    }

    using str::starts_with;
    using str::ends_with;
    using str::ltrim;
    using str::rtrim;
    using str::trim;

    namespace str {

        STDC_EXPORT std::string asprintf(const char *fmt, ...) STDC_PRINTF_FORMAT(1, 2);

        STDC_EXPORT std::string vasprintf(const char *fmt, va_list args);

    }

    using str::asprintf;
    using str::vasprintf;

#ifdef _WIN32
    STDC_EXPORT const std::error_category &windows_utf8_category() noexcept;
#endif

    /// @}
}

#endif // STDCORELIB_STR_H
