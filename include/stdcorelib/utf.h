// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_UTF_H
#define STDCORELIB_UTF_H

#include <string>
#include <string_view>

#include <stdcorelib/stdc_global.h>

namespace stdc {

    /// \addtogroup text
    /// @{

    /// Conversions between UTF-8, UTF-16 and UTF-32, implemented without the conversion
    /// facilities of the standard library and of the platform.
    ///
    /// Each function names its encodings explicitly rather than deriving them from the width of a
    /// type. \c std::wstring is UTF-16 on Windows and UTF-32 on other platforms. The wide-string
    /// functions are therefore built on the explicit functions rather than the reverse.
    ///
    /// \note Code pages are not covered here. Conversion to and from the local ANSI encoding
    ///       requires a Windows API call and is provided by \c str::conv<std::wstring>.
    namespace utf {

        /// The handling of input that is invalid in its encoding.
        enum error_policy {
            /// Each invalid sequence is replaced with U+FFFD and the conversion continues, so that
            /// it always produces a result. This policy is the default, because losing a whole
            /// log line or file name because of one invalid byte is worse than losing the byte.
            replace,

            /// The conversion stops and returns an empty string.
            fail,
        };

        /// The character that a \c replace conversion inserts.
        constexpr char32_t replacement_character = 0xFFFD;

        /// The largest code point that Unicode defines.
        constexpr char32_t max_code_point = 0x10FFFF;

        /// \name Conversions
        ///
        /// Each function accepts the policy for invalid input and, optionally, a flag that
        /// receives whether the input was valid. \a ok is useful only with \c fail, because an
        /// empty result does not indicate whether the input was empty or invalid.
        /// @{

        STDC_EXPORT std::u16string utf8_to_utf16(std::string_view s, error_policy policy = replace,
                                                 bool *ok = nullptr);

        STDC_EXPORT std::u32string utf8_to_utf32(std::string_view s, error_policy policy = replace,
                                                 bool *ok = nullptr);

        STDC_EXPORT std::string utf16_to_utf8(std::u16string_view s, error_policy policy = replace,
                                              bool *ok = nullptr);

        STDC_EXPORT std::u32string utf16_to_utf32(std::u16string_view s,
                                                  error_policy policy = replace,
                                                  bool *ok = nullptr);

        STDC_EXPORT std::string utf32_to_utf8(std::u32string_view s, error_policy policy = replace,
                                              bool *ok = nullptr);

        STDC_EXPORT std::u16string utf32_to_utf16(std::u32string_view s,
                                                  error_policy policy = replace,
                                                  bool *ok = nullptr);

        /// @}

        /// \name Wide strings
        ///
        /// The same conversions for the encoding of \c wchar_t on the current platform, which is
        /// UTF-16 on Windows and UTF-32 on other platforms.
        /// @{

        STDC_EXPORT std::wstring utf8_to_wide(std::string_view s, error_policy policy = replace,
                                              bool *ok = nullptr);

        STDC_EXPORT std::string wide_to_utf8(std::wstring_view s, error_policy policy = replace,
                                             bool *ok = nullptr);

        /// @}

        /// \name Validation
        ///
        /// Checks of well-formedness that do not build the converted string.
        /// @{

        STDC_EXPORT bool is_valid_utf8(std::string_view s);
        STDC_EXPORT bool is_valid_utf16(std::u16string_view s);
        STDC_EXPORT bool is_valid_utf32(std::u32string_view s);

        /// @}

        /// Returns whether \a c is a code point that may appear in text. Such a code point is
        /// within range and is not a surrogate. Surrogates exist only to encode pairs in UTF-16.
        constexpr bool is_valid_code_point(char32_t c) {
            return c <= max_code_point && (c < 0xD800 || c > 0xDFFF);
        }

    }

    /// @}
}

#endif // STDCORELIB_UTF_H
