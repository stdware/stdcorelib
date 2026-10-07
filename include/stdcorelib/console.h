// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_CONSOLE_H
#define STDCORELIB_CONSOLE_H

#include <cstdarg>
#include <cstdio>

#include <stdcorelib/str.h>

namespace stdc {

    /// \addtogroup text
    /// @{

    namespace console {

        /// Text attributes, combined with \c |.
        enum style {
            nostyle = 0x0, ///< no attribute at all
            bold = 0x1,
            italic = 0x2,
            underline = 0x4,
            strikethrough = 0x8,
        };

        /// The eight base colors, each with a brighter variant.
        ///
        /// Unlike \ref style, colors do not combine. A parameter accepts one color, not a bitwise
        /// or of several colors. \c intensified is the exception, because it is the bit that the
        /// \c light names already contain.
        enum color {
            nocolor = 0,        ///< the current color of the terminal, unchanged
            intensified = 0x10, ///< the brightness bit alone

            red = 0x1,
            green = 0x2,
            blue = 0x4,
            yellow = red | green,
            purple = red | blue,
            cyan = green | blue,
            white = red | green | blue,
            black = 0x8,
            lightred = intensified | red,
            lightgreen = intensified | green,
            lightblue = intensified | blue,
            lightyellow = intensified | yellow,
            lightpurple = intensified | purple,
            lightcyan = intensified | cyan,
            lightwhite = intensified | white,
            lightblack = intensified | black,
        };

        /// \name Color mode
        /// @{

        /// The method by which styling is applied to the target.
        enum color_mode {
            automatic,      ///< determined per target, with styling only for a terminal
            never,          ///< no styling for any target
            vt,             ///< ANSI escape sequences for every target
            windows_legacy, ///< the Windows console API, equivalent to \c never elsewhere
        };

        /// Returns the mode set for the process.
        /// \sa set_color_mode()
        STDC_EXPORT color_mode get_color_mode();

        /// Overrides the mode for the whole process.
        ///
        /// A program applies a \c --color=always option or the \c NO_COLOR variable here.
        ///
        /// \param mode the mode to force, or \c automatic to restore detection per target
        /// \note The function also discards the detection results for the targets seen so far.
        ///       After a \c freopen(), calling it again with the current mode forces the targets
        ///       to be detected anew.
        STDC_EXPORT void set_color_mode(color_mode mode);

        /// Returns the mode used for \a file.
        ///
        /// \param file the target, which is examined on the first call and cached afterwards
        /// \return one of \c never, \c vt or \c windows_legacy, never \c automatic
        STDC_EXPORT color_mode resolve_color_mode(FILE *file);

        /// @}

        /// \name Geometry
        /// @{

        /// Returns the width in columns of the terminal behind \a file.
        ///
        /// \param file the target
        /// \param fallback the result if no terminal is behind \a file, as for a pipe or a file
        /// \note The width is queried on every call rather than cached, because a terminal can be
        ///       resized while the program runs.
        /// \note \c COLUMNS takes precedence if it is set. It is the variable through which a
        ///       shell reports the width, and the only means for a caller to specify the width
        ///       if the output is not a terminal.
        STDC_EXPORT int width(FILE *file = stdout, int fallback = 80);

        /// Returns the number of columns that \a utf8 occupies on a terminal.
        ///
        /// The result is neither the length in bytes nor the length in characters. A CJK
        /// ideograph occupies two columns, and a combining mark occupies none.
        STDC_EXPORT int display_width(const std::string_view &utf8);

        /// \overload
        ///
        /// This overload measures one code point, so that text can be measured during a traversal
        /// rather than one character at a time through the string form.
        STDC_EXPORT int display_width(char32_t c);

        /// @}

        /// \name General output
        /// @{

        /// Writes \a buf to \a file with the given attributes and then restores the previous
        /// attributes.
        ///
        /// The string is interpreted as UTF-8 and transcoded for a Windows console.
        ///
        /// \param style a bitwise or of \ref style values, or \c nostyle
        /// \param fg one \ref color value, or \c nocolor
        /// \param bg one \ref color value for the background, or \c nocolor
        /// \param buf the text, which is written whether or not the attributes are applied
        /// \param file the target
        /// \return the number of bytes of \a buf written, excluding escape sequences
        /// \note resolve_color_mode() for \a file determines whether the attributes are emitted.
        ///       A redirected stream therefore receives only the text.
        STDC_EXPORT int fputs(int style, int fg, int bg, const char *buf, FILE *file);

        /// \overload
        STDC_EXPORT int fputs(int style, int fg, int bg, const std::string_view &buf, FILE *file);

        /// Writes \a buf to \c stdout like fputs() and appends a newline.
        STDC_EXPORT int puts(int style, int fg, int bg, const char *buf);

        /// \overload
        STDC_EXPORT int puts(int style, int fg, int bg, const std::string_view &buf);

        /// Writes like fputs(), with printf-style formatting.
        STDC_EXPORT int fprintf(int style, int fg, int bg, FILE *file, const char *fmt, ...)
            STDC_PRINTF_FORMAT(5, 6);

        STDC_EXPORT int vfprintf(int style, int fg, int bg, FILE *file, const char *fmt,
                                 va_list args);

        STDC_EXPORT int printf(int style, int fg, int bg, const char *fmt, ...)
            STDC_PRINTF_FORMAT(4, 5);

        STDC_EXPORT int vprintf(int style, int fg, int bg, const char *fmt, va_list args);

        /// Writes like fputs(), with formatN() placeholders (\c %1, \c %2, ...) rather than printf
        /// conversions.
        /// \sa formatN()
        template <class... Args>
        inline int print(int style, int fg, int bg, const std::string_view &format,
                         Args &&...args) {
            return console::fputs(style, fg, bg, formatN(format, args...), stdout);
        }

        template <class... Args>
        inline int println(int style, int fg, int bg, const std::string_view &format,
                           Args &&...args) {
            return console::puts(style, fg, bg, formatN(format, std::forward<Args>(args)...));
        }

        /// \overload
        inline int println() {
            return std::putchar('\n');
        }

        /// @}

        /// \name Plain output
        /// @{

        /// Writes \a buf to \a file without attributes.
        ///
        /// \note The function is preferable to \c std::fputs on Windows, because the console
        ///       renders UTF-8 text correctly only after transcoding.
        inline int u8fputs(const char *buf, FILE *file) {
            return console::fputs(nostyle, nocolor, nocolor, buf, file);
        }

        /// \overload
        inline int u8fputs(const std::string_view &buf, FILE *file) {
            return console::fputs(nostyle, nocolor, nocolor, buf, file);
        }

        inline int u8puts(const char *buf) {
            return console::puts(nostyle, nocolor, nocolor, buf);
        }

        /// \overload
        inline int u8puts(const std::string_view &buf) {
            return console::puts(nostyle, nocolor, nocolor, buf);
        }

        STDC_EXPORT int u8fprintf(FILE *file, const char *fmt, ...) STDC_PRINTF_FORMAT(2, 3);

        STDC_EXPORT int u8vfprintf(FILE *file, const char *fmt, va_list args);

        STDC_EXPORT int u8printf(const char *fmt, ...) STDC_PRINTF_FORMAT(1, 2);

        STDC_EXPORT int u8vprintf(const char *fmt, va_list args);

        template <class... Args>
        inline int u8print(const std::string_view &format, Args &&...args) {
            return u8fputs(formatN(format, std::forward<Args>(args)...), stdout);
        }

        template <class... Args>
        inline int u8println(const std::string_view &format, Args &&...args) {
            return u8puts(formatN(format, std::forward<Args>(args)...));
        }

        /// \overload
        inline int u8println() {
            return std::putchar('\n');
        }

        /// @}

        /// \name Messages
        /// @{

        /// Writes one line in the conventional color of a severity, for a program that requires
        /// the four usual severities without choosing colors itself.
        ///
        /// All four functions write to \c stdout. Severities with separate destinations and
        /// filtering by category are provided by the logging facility.
        ///
        /// \sa formatN(), stdc::Logger
        template <class... Args>
        inline int debug(const std::string_view &format, Args &&...args) {
            return println(nostyle, lightblue, nocolor, format, std::forward<Args>(args)...);
        }

        template <class... Args>
        inline int success(const std::string_view &format, Args &&...args) {
            return println(nostyle, lightgreen, nocolor, format, std::forward<Args>(args)...);
        }

        template <class... Args>
        inline int warning(const std::string_view &format, Args &&...args) {
            return println(nostyle, yellow, nocolor, format, std::forward<Args>(args)...);
        }

        template <class... Args>
        inline int critical(const std::string_view &format, Args &&...args) {
            return println(nostyle, red, nocolor, format, std::forward<Args>(args)...);
        }

        /// @}
    }

    using console::u8printf;
    using console::u8vprintf;
    using console::u8print;
    using console::u8println;

    namespace console {

        /// \name Inline color markup
        /// @{

        /// Writes \a buf and interprets \c ${...} as attribute changes rather than as text.
        ///
        /// The markup is an alternative to passing \a style, \a fg and \a bg to every call. A group
        /// contains one or more names separated by spaces, and \c $$ writes a literal \c $.
        /// Nested \c ${...} groups are not supported and are ignored.
        /// The names are:
        ///   \li a color: \c red, \c green, \c blue, \c yellow, \c purple, \c cyan, \c white,
        ///       \c black or \c nocolor, each also available with a \c light prefix
        ///   \li a color preceded by \c @, which sets the background instead of the foreground
        ///   \li a style: \c bold, \c italic, \c underline, \c strikethrough or \c nostyle
        ///   \li \c intensified or \c \@intensified, which brightens the current color
        ///   \li \c reset or \c clear, which restores plain text
        ///
        /// \param buf the text and its markup
        /// \param file the target
        /// \return the number of bytes written, excluding the markup and any escape sequences
        /// \note An unknown name is discarded and has no effect. A misspelled name therefore
        ///       loses the styling rather than the text.
        /// \note The attributes are plain at the start of every call and are restored when the
        ///       call returns. They therefore never affect subsequent output.
        ///
        /// \code
        ///   cprintln("${lightgreen}ok ${@blue bold}on blue ${reset}plain, 50$$ off");
        /// \endcode
        ///
        /// fputs() accepts the same attributes as arguments.
        ///
        /// \sa fputs()
        STDC_EXPORT int cfputs(const char *buf, FILE *file);

        /// \overload
        STDC_EXPORT int cfputs(const std::string_view &buf, FILE *file);

        /// Writes \a buf to \c stdout like cfputs() and appends a newline.
        STDC_EXPORT int cputs(const char *buf);

        /// \overload
        STDC_EXPORT int cputs(const std::string_view &buf);

        /// Writes like cfputs(), with printf-style formatting.
        ///
        /// \warning The markup is interpreted after the formatting. If a \c %s expands to text
        ///          that contains \c ${ or \c $$, that text is interpreted as markup rather than
        ///          printed. Text from an untrusted or unknown source must therefore be written
        ///          with u8fprintf().
        STDC_EXPORT int cfprintf(FILE *file, const char *fmt, ...) STDC_PRINTF_FORMAT(2, 3);

        STDC_EXPORT int cvfprintf(FILE *file, const char *fmt, va_list args);

        STDC_EXPORT int cprintf(const char *fmt, ...) STDC_PRINTF_FORMAT(1, 2);

        STDC_EXPORT int cvprintf(const char *fmt, va_list args);

        /// Writes like cfputs(), with formatN() placeholders (\c %1, \c %2, ...).
        template <class... Args>
        inline int cprint(const std::string_view &format, Args &&...args) {
            return cfputs(formatN(format, std::forward<Args>(args)...), stdout);
        }

        template <class... Args>
        inline int cprintln(const std::string_view &format, Args &&...args) {
            return cputs(formatN(format, std::forward<Args>(args)...));
        }

        /// @}
    }

    using console::cprintf;
    using console::cvprintf;
    using console::cprint;
    using console::cprintln;

    /// @}
}

#endif // STDCORELIB_CONSOLE_H
