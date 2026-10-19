// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_STR_P_H
#define STDCORELIB_STR_P_H

#include <optional>
#include <string>

#include <stdcorelib/str.h>

#include <stdcorelib/adt/vlarray.h>

namespace stdc::str {

    enum class varexp_part_type {
        literal,
        literal_with_dollar,
        variable,
        nested_variable,
    };

    struct varexp_part {
        varexp_part_type type;
        const char *data;
        size_t size;
    };

    STDC_DECL_HIDDEN bool varexp_split(const std::string_view &s,
                                       vlarray_base<varexp_part> &result);

    STDC_DECL_HIDDEN std::string varexp_post(const std::string_view &s);

    // The conversions of floating-point numbers below use the C locale, in which a period is the
    // decimal separator. A host program may select another locale with setlocale, as a Qt program
    // on Unix does with setlocale(LC_ALL, ""), and strtod then reads "1.5" as 1 in a German
    // locale. The conversions select the C locale for the calling thread alone.

    /// The handling of a number outside the range of the requested type
    enum class floating_range {
        reject, ///< The conversion fails.
        accept, ///< The conversion returns the value of strtod, an infinity or zero.
    };

    /// Converts the whole of \a s to \a T, \c float, \c double or \c long double, in the forms
    /// that strtod accepts.
    ///
    /// \return the value, or \c std::nullopt if \a s is empty, contains characters after the
    ///         number, or is out of range and \a range is floating_range::reject
    template <class T>
    STDC_DECL_HIDDEN std::optional<T> parse_floating(std::string_view s, floating_range range);

    /// Returns the shortest text of \a d in the form of printf with \c %.*g that strtod reads back
    /// as \a d. Seventeen significant digits suffice for every double.
    STDC_DECL_HIDDEN std::string format_floating(double d);

}

#endif // STDCORELIB_STR_P_H
