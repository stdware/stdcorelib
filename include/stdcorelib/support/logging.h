// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_LOGGING_H
#define STDCORELIB_LOGGING_H

#include <stdcorelib/str.h>

/// \defgroup logging Logging
///
/// Named categories with per-level switches and Qt-style filter rules.
///
/// \code
///     static stdc::LogCategory lc("app.io");
///
///     lc.stdcWarning("cannot read %1", path);
///     lc.stdcDebugF("offset=%zu", off);   // the printf-style variant
///     stdcInfo("logged to the default category");
///
///     lc.setFilterRules("*.debug = false\n"        // disables debug in every category
///                       "app.io = false\n"         // disables this category
///                       "app.io.warning = true");  // re-enables its warnings
/// \endcode
///
/// \c Logger::setLogCallback() replaces the sink. A program uses it to direct records to a file or
/// to a user interface instead of the terminal.

namespace stdc {

    /// \addtogroup logging
    /// @{

    class LogContext {
    public:
        inline LogContext() noexcept = default;
        inline LogContext(const char *fileName, int lineNumber, const char *functionName,
                          const char *categoryName) noexcept
            : line(lineNumber), file(fileName), function(functionName), category(categoryName) {
        }

        int line = 0;
        const char *file = nullptr;
        const char *function = nullptr;
        const char *category = nullptr;
    };

    class STDC_EXPORT Logger {
    public:
        enum Level {
            Trace = 1,
            Debug,
            Success,
            Information,
            Warning,
            Critical,
            Fatal,
        };

        inline Logger(LogContext context) : _context(std::move(context)) {
        }

        inline Logger(const char *file, int line, const char *function, const char *category)
            : _context(file, line, function, category) {
        }

        template <class... Args>
        inline void trace(const std::string_view &format, Args &&...args) {
            print(Trace, stdc::formatN(format, std::forward<Args>(args)...));
        }

        template <class... Args>
        inline void debug(const std::string_view &format, Args &&...args) {
            print(Debug, stdc::formatN(format, std::forward<Args>(args)...));
        }

        template <class... Args>
        inline void success(const std::string_view &format, Args &&...args) {
            print(Success, stdc::formatN(format, std::forward<Args>(args)...));
        }

        template <class... Args>
        inline void info(const std::string_view &format, Args &&...args) {
            print(Information, stdc::formatN(format, std::forward<Args>(args)...));
        }

        template <class... Args>
        inline void warning(const std::string_view &format, Args &&...args) {
            print(Warning, stdc::formatN(format, std::forward<Args>(args)...));
        }

        template <class... Args>
        inline void critical(const std::string_view &format, Args &&...args) {
            print(Critical, stdc::formatN(format, std::forward<Args>(args)...));
        }

        template <class... Args>
        [[noreturn]] inline void fatal(const std::string_view &format, Args &&...args) {
            print(Fatal, stdc::formatN(format, std::forward<Args>(args)...));
            abort();
        }

        template <class... Args>
        inline void log(int level, const std::string_view &format, Args &&...args) {
            print(level, stdc::formatN(format, std::forward<Args>(args)...));
        }

        void print(int level, const std::string_view &message);

        void printf(int level, const char *fmt, ...);

        [[noreturn]] static void abort();

    public:
        using LogCallback = void (*)(int, const LogContext &, const std::string_view &);

        static LogCallback logCallback();

        /// Replaces the sink that receives every record.
        ///
        /// \param callback the new sink, or \c nullptr to restore the built-in sink
        static void setLogCallback(LogCallback callback);

    protected:
        LogContext _context;
    };

    /// A named channel with independently switchable levels, modeled on \c QLoggingCategory of Qt.
    ///
    /// Each category registers itself on construction and applies the filter rules that are
    /// already in effect.
    ///
    /// Disabling the fatal level suppresses its record but does not suppress process termination.
    ///
    /// \sa setFilterRules()
    class STDC_EXPORT LogCategory {
    public:
        explicit LogCategory(const char *name);
        ~LogCategory();

        inline const char *name() const {
            return _name;
        }
        inline bool isLevelEnabled(int level) const {
            return levelEnabled[level];
        }
        inline void setLevelEnabled(int level, bool enabled) {
            levelEnabled[level] = enabled;
        }

        using LogCategoryFilter = void (*)(LogCategory *);

        /// Returns the filter in effect, which is the default filter if none was installed.
        ///
        /// \return never \c nullptr. setLogFilter() interprets \c nullptr as the default filter
        ///         rather than as no filter. A state without a filter therefore does not exist.
        static LogCategoryFilter logFilter();

        /// Replaces the category filter and applies it again to every registered category.
        ///
        /// \param filter the new filter, or \c nullptr to restore the default filter
        /// \note A custom filter replaces the default filter entirely. setFilterRules() therefore
        ///       has no effect unless the custom filter reads the rules itself.
        static void setLogFilter(LogCategoryFilter filter);

        static std::string filterRules();

        /// Installs Qt-style filter rules that determine which levels each category emits.
        ///
        /// Rules are separated by newlines or \c ;, and a \c # starts a comment line. Each rule
        /// has the form <tt>category[.level] = true|false</tt>, in which:
        ///   \li category may have a single leading or trailing \c * wildcard, or both, and
        ///       otherwise matches exactly
        ///   \li level is one of \c trace, \c debug, \c success, \c info, \c warning,
        ///       \c critical or \c fatal. A rule without a level applies to every level.
        ///
        /// Rules are applied in order, starting from a state in which every level is enabled.
        /// A later matching rule therefore takes precedence.
        ///
        /// \code
        ///   *.debug = false          // disables debug in every category
        ///   stdc.io = false          // disables the stdc.io category
        ///   stdc.io.warning = true   // re-enables its warnings
        /// \endcode
        ///
        /// \note Although the function is a member, it affects every category in the process,
        ///       not only this category. A malformed rule is skipped without a report.
        void setFilterRules(std::string rules);

        static LogCategory &defaultCategory();

        template <int Level, class... Args>
        void log(const char *fileName, int lineNumber, const char *functionName,
                 const std::string_view &format, Args &&...args) const {
            if (isLevelEnabled(Level)) {
                Logger(fileName, lineNumber, functionName, _name)
                    .log(Level, format, std::forward<Args>(args)...);
            }
            if constexpr (Level == stdc::Logger::Fatal) {
                Logger::abort();
            }
        }

        template <int Level, class... Args>
        void logf(const char *fileName, int lineNumber, const char *functionName, const char *fmt,
                  Args &&...args) const {
            if (isLevelEnabled(Level)) {
                Logger(fileName, lineNumber, functionName, _name)
                    .printf(Level, fmt, std::forward<Args>(args)...);
            }
            if constexpr (Level == stdc::Logger::Fatal) {
                Logger::abort();
            }
        }

        inline const LogCategory &stdcGetLogCategory() const {
            return *this;
        }

    protected:
        const char *_name;

        union {
            bool levelEnabled[8];
            uint64_t enabled;
        };
    };

    /// @}
}

/// Returns the category that the macros below use if no LogCategory is in scope. A category
/// provides a member of the same name, which unqualified lookup finds first.
///
/// \internal
static inline const stdc::LogCategory &stdcGetLogCategory() {
    return stdc::LogCategory::defaultCategory();
}

/// Logs one record at \a LEVEL, together with the file, line and function of the call.
///
/// Called on a category, the macro logs to that category. Called without a category, it logs to
/// the default category. The message uses formatN() placeholders (\c %1, \c %2, ...), and the
/// \c F variants below use printf conversions instead.
///
/// \warning The macro expands to an ordinary call. The arguments are therefore evaluated whether
///          or not the level is enabled. Unlike \c qCDebug of Qt, the macro does not short
///          circuit, and an expensive argument requires a preceding isLevelEnabled() check.
///
/// \code
///   stdc::LogCategory lc("app.io");
///   lc.stdcWarning("cannot read %1", path);
///   lc.stdcWarningF("cannot read %s", path.c_str());
///
///   stdcWarning("logged to the default category");
/// \endcode
#define stdcLog(LEVEL, ...)                                                                        \
    stdcGetLogCategory().log<stdc::Logger::LEVEL>(__FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define stdcTrace(...)    stdcLog(Trace, __VA_ARGS__)
#define stdcDebug(...)    stdcLog(Debug, __VA_ARGS__)
#define stdcSuccess(...)  stdcLog(Success, __VA_ARGS__)
#define stdcInfo(...)     stdcLog(Information, __VA_ARGS__)
#define stdcWarning(...)  stdcLog(Warning, __VA_ARGS__)
#define stdcCritical(...) stdcLog(Critical, __VA_ARGS__)
#define stdcFatal(...)    stdcLog(Fatal, __VA_ARGS__)

#define stdcLogF(LEVEL, ...)                                                                       \
    stdcGetLogCategory().logf<stdc::Logger::LEVEL>(__FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define stdcTraceF(...)    stdcLogF(Trace, __VA_ARGS__)
#define stdcDebugF(...)    stdcLogF(Debug, __VA_ARGS__)
#define stdcSuccessF(...)  stdcLogF(Success, __VA_ARGS__)
#define stdcInfoF(...)     stdcLogF(Information, __VA_ARGS__)
#define stdcWarningF(...)  stdcLogF(Warning, __VA_ARGS__)
#define stdcCriticalF(...) stdcLogF(Critical, __VA_ARGS__)
#define stdcFatalF(...)    stdcLogF(Fatal, __VA_ARGS__)

#endif // STDCORELIB_LOGGING_H
