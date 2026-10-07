// SPDX-License-Identifier: MIT

/// \file commandline.h
///
/// Declaration of the command line syntax of a program, and access to the parsed values.
///
/// The API is modeled on SysCmdLine, https://github.com/SineStriker/syscmdline, which this header
/// replaces. Every declaration of this header belongs to the \ref cli module, which contains the
/// documentation.

#ifndef STDCORELIB_COMMANDLINE_H
#define STDCORELIB_COMMANDLINE_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/console.h>
#include <stdcorelib/str.h>
#include <stdcorelib/flags.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup cli Command line
///
/// Declaration of the command line syntax of a program, and access to the parsed values.
///
/// A program declares its syntax as a tree of stdc::cli::Command objects, each containing its
/// stdc::cli::Argument and stdc::cli::Option objects, and passes the tree to a stdc::cli::Parser.
/// The parser returns a stdc::cli::ParseResult. Because every accessor of the result returns
/// \c std::optional, an absent value and an empty value are distinguishable.
///
/// \code
///     using namespace stdc;
///
///     cli::Parser parser(cli::Command("prog", "What it is for")
///                            .addArgument(cli::Argument("path", "Where to work"))
///                            .addOption(cli::Option({"-j", "--jobs"}, "How many at once")
///                                           .arg(cli::Argument("n").type<int>()))
///                            .addHelpOption(true)
///                            .addVersionOption("1.0.0"));
///
///     return parser.invoke(system::command_line_arguments());
/// \endcode
///
/// \c invoke() reports a parse failure, prints the help text for \c --help and the version text
/// for \c --version, and otherwise runs the handler of the reached command. The return value of
/// \c invoke() is the return value of \c main.
///
/// \section cli_shape Command line structure
///
/// \verbatim
///     prog  remote add   --force file.txt -j 4
///           \________/   \_____________________/
///          command path    options and arguments, in any order
/// \endverbatim
///
/// Subcommand names come first, and no other token may appear between them. A command line
/// selects a command by naming every command from the root down to it. This sequence of names is
/// the command path. The first token that is not a subcommand name ends the command path. The
/// reached command is the last command named, or the root if no subcommand is named. Every later
/// token belongs to the reached command. These tokens are its own options, the options marked
/// Option::recursive() by its ancestors, and its arguments. They may appear in any order, subject
/// only to the constraints of a greedy argument.
///
/// The command line \c prog \c --plain \c sub is therefore an error rather than a way to reach
/// \c sub, even if the root declares \c --plain. This rule differs from the rule of git, which
/// places root options before the subcommand name. Option::recursive() allows an option of one
/// command to be written on a command line that reaches another command.
///
/// \subsection cli_shape_lines Accepted and rejected command lines
///
/// \code
///   cli::Command("prog")
///       .addOption(cli::Option({"--plain"}, "The root's own"))
///       .addOption(cli::Option({"--wide"}, "The root's, recursive").recursive())
///       .addCommand(cli::Command("sub")
///                       .addArgument(cli::Argument("path"))
///                       .addOption(cli::Option({"-f"}, "The subcommand's")));
/// \endcode
///
/// \verbatim
///   prog --plain                 reaches the root, and --plain is an option of the root
///   prog sub -f a                reaches sub, -f is an option of sub, a is its argument
///   prog sub --wide -f a         --wide is in scope at sub because it is recursive
///   prog sub a -f --wide         options and arguments follow the path in any order
///
///   prog --plain sub             rejected: sub is too late because an option ends the path
///   prog --wide sub              rejected for the same reason. Option::recursive() specifies
///                                where an option may be written. It does not relax the rule
///                                that the path comes first.
///   prog sub --plain             rejected: --plain belongs to the root and is not recursive
///   prog -f sub                  rejected: -f ends the path at the root, which does not declare -f
/// \endverbatim
///
/// \subsection cli_shape_greedy Greedy arguments
///
/// An Argument::Multiple argument reserves one token for each required argument after it.
/// Therefore, \c copy \c \<src\>... \c \<dest\> is valid in the arguments of a command and in the
/// arguments of an option. An Argument::Remainder argument reserves no token and ends option
/// recognition at its first token. A program defines its own terminator through such an argument.
/// The library does not reserve \c -- . A program that requires the conventional terminator
/// declares it.
///
/// \code
///   cli::Command("copy")
///       .addArgument(cli::Argument("src").multi())
///       .addArgument(cli::Argument("dest"))
///       .addOption(cli::Option({"-f"}, "Read the names from")
///                      .arg(cli::Argument("list").multi()))
///       .addOption(cli::Option({"--"}, "The rest")
///                      .arg(cli::Argument("rest").nargs(cli::Argument::Remainder).optional()));
/// \endcode
///
/// \verbatim
///   copy a b c                   src receives a and b, dest receives the token reserved for it
///   copy a b c -f x y            the arguments first, then -f up to the end of the line
///   copy a b c -f x -- y         the values of -f end at --, because -- is a declared option
///   copy a b c -- -f x           -f is a value of -- because -- recognizes no options
///
///   copy -f x y a b c            rejected: -f consumes all five tokens, <src> and <dest> none
/// \endverbatim
///
/// The values of a greedy argument end at the next declared option. If a command has arguments
/// of its own and an option with a greedy argument, the command arguments must be written first,
/// because the option otherwise consumes them.
///
/// An Argument::Remainder argument starts after the last token of the preceding argument. If it
/// is the first argument of a command, no preceding argument exists, and it starts at the first
/// token that is not written as an option. This rule allows a wrapper program to accept options of
/// its own and to pass the remaining tokens on:
///
/// \code
///   cli::Command("run")
///       .addArgument(cli::Argument("rest").nargs(cli::Argument::Remainder).optional())
///       .addOption(cli::Option({"-u"}, "As user").arg("who"));
/// \endcode
///
/// \verbatim
///   run -u root ls -u x          -u root belongs to run, ls -u x to the remainder, one spelling
///                                twice
///   run ls -u x                  every token is in the remainder because no option of run is
///                                written
///
///   run -w ls                    rejected: -w is written as an option and is not declared, and
///                                is therefore not passed on
/// \endverbatim
///
/// A value may be attached to its option, as in \c --opt=v or \c -Ov. An attached value is the
/// first value of the first argument of the option, not the complete argument. The command line
/// \c --opt=a \c b is therefore read as \c --opt \c a \c b if that argument accepts more than one
/// value. The attached spelling that Option::shortMatch() permits is an exception. Because one
/// token contains one value, this spelling is available only if the option has exactly one
/// argument and that argument accepts exactly one value.
///
/// \subsection cli_shape_trees Invalid command trees
///
/// The following conditions are errors of the program, not of the user input. In a debug build,
/// parse() asserts that the whole tree is valid. A program that builds its command tree
/// dynamically may call Parser::validate() and report the reason itself.
///
/// An argument should not
/// \li have an empty name, or share its name with another argument in the same list. The
///     arguments of a command and the arguments of an option are separate lists.
/// \li be required if a preceding argument is optional, because one token could then belong to
///     either argument
/// \li follow an Argument::Remainder argument, which consumes every remaining token
/// \li follow an Argument::Multiple argument unless it is a required Argument::Single argument,
///     because the token reservation of Argument::Multiple covers only that case
/// \li list in expect() a value that its type() cannot parse
/// \li have a defaultValue() that its type(), its expect() or its validate() rejects, or have any
///     defaultValue() while being required, because a default value replaces only an absent value
///
/// An option should not
/// \li have no spelling, a spelling shorter than two characters, or a spelling that starts with
///     neither \c - nor \c /
/// \li repeat one of its own spellings
/// \li share a spelling with another option of the same command
/// \li be required or have arguments if its prior() is Option::AutoSetWhenNoSymbols, because such
///     an option is set without being written and therefore receives no values
/// \li receive a negative maximum occurrence count from multi()
///
/// A command should not
/// \li contain a subcommand with an empty name, or two subcommands with the same name
/// \li contain both an Argument::Remainder argument and an option with a greedy argument, because
///     both consume the rest of the line and only one of them can be written first
///
/// A CommandCatalogue should not name an item that its command does not contain, or name one item
/// twice, either in one group or in two groups of the same kind.
///
/// The following conditions are detectable only on the complete tree. A command tree should not
/// \li have two options in scope at one command that match one spelling. The recursive options of
///     every ancestor command are included.
/// \li have two names in one scope that differ only in case, if the parse ignores case
/// \li have two spellings that accept an attached value if one spelling is a prefix of the other,
///     as \c -D and \c -Da are for \c -Dabc, because no rule can resolve the ambiguity
///
/// \section cli_help Help text customization
///
/// Help text customization has five levels. A program proceeds only to the level that its change
/// requires. Because each level is implemented in terms of the level below it, a single change
/// requires no reimplementation.
///
/// \verbatim
///   1  dimensions                    parser.setIndent(2)
///                                    parser.setSpacing(1)
///                                    parser.setTextWidth(100)
///
///   2  styles                        layout.setTitleStyle({console::bold})
///                                    layout.setBodyStyle(HelpBlock::Epilogue, {...})
///
///   3  blocks and their order        HelpLayout().add(HelpBlock::Usage)
///                                                .add(HelpBlock::Options)
///                                                .add(myOwnBlock)
///
///   4  block construction and        struct Mine : HelpFormatter {
///      layout, one method at a time      std::vector<HelpBlock> blocks(...) const override {
///                                            auto res = HelpFormatter::blocks(...);
///                                            ...
///                                        }
///                                    };
///
///   5  direct use of the blocks      for (auto &block : result.helpBlocks()) { ... }
/// \endverbatim
///
/// Levels 1 to 3 are settings of the Parser and require no program-defined type. Level 4 is
/// HelpFormatter, whose methods form a hierarchy of levels of their own, shown by the diagram in
/// its documentation. At level 5 the library provides the blocks, and the program processes them
/// without further involvement of the library.

namespace stdc::cli {

    /// \addtogroup cli
    /// @{

    /// The conversion of a token to \c T, and the name of \c T in the help text.
    ///
    /// Because a command line consists of text, every value is stored as text and converted when
    /// it is read. A program specializes this template to accept a type of its own:
    ///
    /// \code
    ///   template <>
    ///   struct stdc::cli::value_traits<fs::path> {
    ///       static bool parse(std::string_view token, fs::path *out) {
    ///           *out = token;
    ///           return true;
    ///       }
    ///       static const char *type_name() {
    ///           return "path";
    ///       }
    ///   };
    /// \endcode
    ///
    /// \c parse returns false for a token that the type cannot represent. This return value
    /// causes \c --count=x to produce a diagnostic instead of the value zero.
    template <class T, class Enable = void>
    struct value_traits;

    namespace detail {

        /// The check function and the name of a type, stored as pointers so that Argument can
        /// record a type without being a template.
        struct value_type_info {
            /// Returns whether the token is a valid \c T. A null pointer, which is the default,
            /// accepts every token.
            bool (*check)(std::string_view) = nullptr;
            /// The name used in diagnostics and in the help text. It must be a string literal,
            /// because the pointer is stored without copying the string.
            const char *name = nullptr;
        };

        template <class T>
        bool check_value(std::string_view token) {
            T out{};
            return value_traits<T>::parse(token, &out);
        }

        template <class T>
        value_type_info type_info_for() {
            return {&check_value<T>, value_traits<T>::type_name()};
        }

        /// The data of a ParseResult.
        class parse_data;

        STDC_EXPORT bool parse_signed(std::string_view token, int64_t *out, int64_t min,
                                      int64_t max);
        STDC_EXPORT bool parse_unsigned(std::string_view token, uint64_t *out, uint64_t max);
        STDC_EXPORT bool parse_floating(std::string_view token, float *out);
        STDC_EXPORT bool parse_floating(std::string_view token, double *out);
        STDC_EXPORT bool parse_floating(std::string_view token, long double *out);
        STDC_EXPORT bool parse_boolean(std::string_view token, bool *out);

    }

    /// Text, which requires no conversion because a command line consists of text.
    template <>
    struct value_traits<std::string> {
        static inline bool parse(std::string_view token, std::string *out) {
            out->assign(token);
            return true;
        }
        static inline const char *type_name() {
            return "string";
        }
    };

    /// A view into the storage of the result, which remains valid after the read.
    template <>
    struct value_traits<std::string_view> {
        static inline bool parse(std::string_view token, std::string_view *out) {
            *out = token;
            return true;
        }
        static inline const char *type_name() {
            return "string";
        }
    };

    /// Accepts \c true, \c false, \c yes, \c no, \c on, \c off, \c 1 and \c 0, case-insensitively.
    template <>
    struct value_traits<bool> {
        static inline bool parse(std::string_view token, bool *out) {
            return detail::parse_boolean(token, out);
        }
        static inline const char *type_name() {
            return "bool";
        }
    };

    /// Every integer type except \c bool, which has a separate specialization. The check includes
    /// the range of the target type. Therefore, \c 300 is not a valid \c uint8_t.
    template <class T>
    struct value_traits<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
        static inline bool parse(std::string_view token, T *out) {
            if constexpr (std::is_signed_v<T>) {
                int64_t v;
                if (!detail::parse_signed(token, &v, int64_t(std::numeric_limits<T>::min()),
                                          int64_t(std::numeric_limits<T>::max()))) {
                    return false;
                }
                *out = T(v);
            } else {
                uint64_t v;
                if (!detail::parse_unsigned(token, &v, uint64_t(std::numeric_limits<T>::max()))) {
                    return false;
                }
                *out = T(v);
            }
            return true;
        }
        static inline const char *type_name() {
            return std::is_signed_v<T> ? "int" : "uint";
        }
    };

    /// \c float, \c double and \c long double. The conversion uses the matching function of the
    /// \c strto* family instead of \c from_chars, because older libc++ versions do not implement
    /// \c from_chars for floating-point types.
    template <class T>
    struct value_traits<T, std::enable_if_t<std::is_floating_point_v<T>>> {
        static inline bool parse(std::string_view token, T *out) {
            return detail::parse_floating(token, out);
        }
        static inline const char *type_name() {
            return "number";
        }
    };

    class ParseResult;
    class HelpFormatter;

    /// A positional value of a command or an option.
    class Argument {
    public:
        /// The number of tokens that an argument consumes.
        enum Arity {
            /// Exactly one token.
            Single,
            /// One or more tokens. An argument of this arity is called greedy. It leaves enough
            /// tokens for the required arguments after it. Therefore,
            /// \c copy \c \<src\>... \c \<dest\> is valid.
            Multiple,
            /// Every remaining token, options included. No argument may follow an argument of
            /// this arity. Because option recognition stops at its first token, a program
            /// defines its own terminator through such an argument.
            ///
            /// It starts after the last token of the preceding argument. If it is the first
            /// argument, no preceding argument exists, and it starts at the first token that is
            /// not written as an option. This rule allows a wrapper program to accept options of
            /// its own: \c run \c -u \c root \c ls \c -l assigns \c -u to \c run and \c -l to
            /// \c ls.
            ///
            /// \note Such an argument is required like any other argument. An empty remainder
            ///       requires optional().
            Remainder,
        };

        /// Returns whether \a token is acceptable, and stores the reason in \a error if it is not.
        ///
        /// The parser may call a validator repeatedly with the same token, including during
        /// validation of the command tree. A validator should have no observable side effects,
        /// and its result should depend neither on the number of previous calls nor on mutable
        /// external state.
        using Validator = std::function<bool(std::string_view token, std::string *error)>;

        Argument() = default;

        inline Argument(std::string name, std::string desc = {}, bool required = true)
            : _name(std::move(name)), _desc(std::move(desc)), _required(required) {
        }

        /// Sets the name shown in the help text, if that name differs from name().
        inline Argument &metavar(std::string displayName) {
            _displayName = std::move(displayName);
            return *this;
        }
        inline Argument &required(bool on = true) {
            _required = on;
            return *this;
        }
        inline Argument &optional(bool on = true) {
            _required = !on;
            return *this;
        }
        /// Sets the value that the result returns if the argument is absent from the command
        /// line. The value is stored as text and converted when read.
        ///
        /// \pre The value is readable as the type declared by type<T>(), and is one of the
        ///      values allowed by expect(), if either was declared.
        inline Argument &defaultValue(std::string value) {
            _default = std::move(value);
            _hasDefault = true;
            return *this;
        }
        /// Sets the complete list of accepted values, for an argument that selects one of a few
        /// words.
        ///
        /// \pre Every value is readable as the type declared by type<T>(), if a type was
        ///      declared.
        inline Argument &expect(std::vector<std::string> values) {
            _expected = std::move(values);
            return *this;
        }
        /// Sets an additional acceptance check, applied to tokens that are readable as the type
        /// of the argument.
        ///
        /// \pre The validator accepts defaultValue(), if a default value exists. Because the
        ///      parser applies the validator only to tokens written on the command line, a
        ///      default value that the validator rejects would be returned without any check.
        inline Argument &validate(Validator validator) {
            _validator = std::move(validator);
            return *this;
        }
        inline Argument &nargs(Arity arity) {
            _arity = arity;
            return *this;
        }
        inline Argument &multi(bool on = true) {
            _arity = on ? Multiple : Single;
            return *this;
        }
        /// Declares the type. The parser checks tokens against it, and its name appears in the
        /// help text. Without a declared type, every token is accepted.
        ///
        /// \pre Every value given to expect() is readable as a \c T.
        template <class T>
        inline Argument &type() {
            _type = detail::type_info_for<T>();
            return *this;
        }

        inline const std::string &name() const {
            return _name;
        }
        inline const std::string &description() const {
            return _desc;
        }
        /// Returns the metavar if a metavar was set, and name() otherwise.
        inline const std::string &displayName() const {
            return _displayName.empty() ? _name : _displayName;
        }
        inline bool isRequired() const {
            return _required;
        }
        inline bool hasDefaultValue() const {
            return _hasDefault;
        }
        inline const std::string &defaultValue() const {
            return _default;
        }
        inline const std::vector<std::string> &expectedValues() const {
            return _expected;
        }
        inline const Validator &validator() const {
            return _validator;
        }
        inline Arity arity() const {
            return _arity;
        }
        inline const detail::value_type_info &typeInfo() const {
            return _type;
        }

    private:
        std::string _name;
        std::string _desc;
        std::string _displayName;
        std::string _default;
        std::vector<std::string> _expected;
        Validator _validator;
        detail::value_type_info _type;
        Arity _arity = Single;
        bool _required = true;
        bool _hasDefault = false;
    };

    /// A named switch with any number of arguments.
    class Option {
    public:
        /// The meaning of an option, for the two options that the library handles itself. A role
        /// provides the conventional spellings and description, and allows a caller to query an
        /// option by role instead of by spelling.
        ///
        /// The set is closed and contains exactly these two roles, because the library acts on
        /// these two only. A role for a switch for which the library supplies only the spelling
        /// would be a second way of writing
        /// Option({"-V", "--verbose"}, "Print more information").
        ///
        /// \note A role does not specify the scope, which recursive() controls, nor the position
        ///       of an option in the help text, which is the declaration order.
        enum Role {
            NoRole,
            Version,
            Help,
        };

        /// The portion of a short token that the parser may match as this option, so that
        /// \c -O2 or \c -DKEY=VALUE can be one token instead of two.
        enum ShortMatch {
            /// \c -D and its value are separate tokens.
            NoShortMatch,
            /// A single letter may be followed by the value, as in \c -O2.
            ShortMatchSingleLetter,
            /// A single character, letter or not, may be followed by the value.
            ShortMatchSingleChar,
            /// The complete rest of the token after the spelling is the value, as in
            /// \c -DKEY=VALUE.
            ShortMatchAll,
        };

        /// The priority level of an option. The highest level among the given options determines
        /// the behavior. This rule allows \c --help to take effect on a command line that lacks
        /// every required item.
        ///
        /// ###QUESTION: Should automatic activation and exclusivity be split from missing-value
        /// priority? This enum prevents combining those policies and orders unrelated values.
        enum Prior {
            NoPrior,
            /// Missing arguments of this option are not an error.
            IgnoreMissingArguments,
            /// No missing item anywhere on the command line is an error.
            IgnoreMissingSymbols,
            /// The option is set if the command line contains no other token.
            AutoSetWhenNoSymbols,
            /// If the option is given, no argument may be given.
            ExclusiveToArguments,
            /// If the option is given, no other option may be given.
            ExclusiveToOptions,
            /// If the option is given, no other token may be given.
            ExclusiveToAll,
        };

        Option() = default;

        inline Option(std::vector<std::string> tokens, std::string desc = {})
            : _tokens(std::move(tokens)), _desc(std::move(desc)) {
        }
        inline Option(std::initializer_list<std::string> tokens, std::string desc = {})
            : Option(std::vector<std::string>(tokens), std::move(desc)) {
        }
        inline Option(std::string token, std::string desc = {})
            : Option(std::vector<std::string>{std::move(token)}, std::move(desc)) {
        }
        /// Constructs an option with \a role. The constructor is not explicit, so that
        /// \c addOptions({Option::Help}) is well-formed. If \a tokens is empty, the option uses
        /// the conventional spellings of the role.
        inline Option(Role role, std::vector<std::string> tokens = {}, std::string desc = {})
            : _tokens(tokens.empty() ? defaultTokens(role) : std::move(tokens)),
              _desc(desc.empty() ? defaultDescription(role) : std::move(desc)), _role(role) {
        }

        /// Adds an argument. The argument of an option requires no description.
        inline Option &arg(std::string name, bool required = true) {
            return arg(Argument(std::move(name), {}, required));
        }
        inline Option &arg(Argument argument) {
            _args.emplace_back(std::move(argument));
            return *this;
        }
        inline Option &required(bool on = true) {
            _required = on;
            return *this;
        }
        /// Sets whether a value may be attached to the spelling, as in \c -Dfoo instead of
        /// \c -D \c foo.
        ///
        /// \note This setting permits the attached form but does not guarantee it. Because one
        ///       token contains one value, the attached form never matches an option that has no
        ///       argument, an optional argument, more than one argument, or a greedy argument,
        ///       regardless of this setting. Each of these options accepts its value as a
        ///       separate token.
        inline Option &shortMatch(ShortMatch rule) {
            _shortMatch = rule;
            return *this;
        }
        inline Option &prior(Prior level) {
            _prior = level;
            return *this;
        }
        /// Places the option in scope for every descendant command as well as for the declaring
        /// command.
        ///
        /// Without this setting, an option can be given only if the declaring command is the
        /// reached command, because every option is written after the name of its own command.
        inline Option &recursive(bool on = true) {
            _recursive = on;
            return *this;
        }
        /// Sets the maximum number of occurrences of the option. Zero means no limit.
        ///
        /// The unit of repetition is the complete occurrence. Every time the option is written,
        /// the parser reads its arguments again into a separate set, which OptionResult::at()
        /// returns. The number of values that one argument accepts within one occurrence is a
        /// separate setting, Argument::multi().
        /// \pre \a maxOccurrence is not negative. A negative value is not a smaller limit but a
        ///      limit that the count can never reach, which is equivalent to no limit.
        inline Option &multi(int maxOccurrence = 0) {
            _maxOccurrence = maxOccurrence;
            return *this;
        }

        inline const std::vector<std::string> &tokens() const {
            return _tokens;
        }
        /// Returns the first spelling, which the help text and diagnostics use.
        inline const std::string &token() const {
            assert(!_tokens.empty() && "an option with no spelling has no token");
            return _tokens.front();
        }
        inline const std::string &description() const {
            return _desc;
        }
        inline const std::vector<Argument> &arguments() const {
            return _args;
        }
        inline bool isRequired() const {
            return _required;
        }
        inline bool isRecursive() const {
            return _recursive;
        }
        inline Role role() const {
            return _role;
        }
        inline ShortMatch shortMatch() const {
            return _shortMatch;
        }
        inline Prior prior() const {
            return _prior;
        }
        inline int maxOccurrence() const {
            return _maxOccurrence;
        }

        /// Returns the help text description of \a role, used if no description was given.
        static inline std::string defaultDescription(Role role) {
            switch (role) {
                case Help:
                    return "Show this help and exit";
                case Version:
                    return "Show the version and exit";
                default:
                    return {};
            }
        }

        /// Returns the conventional spellings of \a role, used if no spellings were given.
        static inline std::vector<std::string> defaultTokens(Role role) {
            switch (role) {
                case Help:
                    return {"-h", "--help"};
                case Version:
                    return {"-v", "--version"};
                default:
                    return {};
            }
        }

    private:
        std::vector<std::string> _tokens;
        std::string _desc;
        std::vector<Argument> _args;
        Role _role = NoRole;
        ShortMatch _shortMatch = NoShortMatch;
        Prior _prior = NoPrior;
        int _maxOccurrence = 1;
        bool _required = false;
        bool _recursive = false;
    };

    /// The assignment of names to headings in the help text. Because a name absent from the
    /// catalogue is listed under the default heading, a catalogue needs to contain only the names
    /// that the program moves to another heading.
    class CommandCatalogue {
    public:
        struct Group {
            std::string name;
            std::vector<std::string> members;
        };

        inline CommandCatalogue &addCommands(std::string group, std::vector<std::string> names) {
            _commands.push_back({std::move(group), std::move(names)});
            return *this;
        }
        inline CommandCatalogue &addOptions(std::string group, std::vector<std::string> names) {
            _options.push_back({std::move(group), std::move(names)});
            return *this;
        }
        inline CommandCatalogue &addArguments(std::string group, std::vector<std::string> names) {
            _arguments.push_back({std::move(group), std::move(names)});
            return *this;
        }

        inline const std::vector<Group> &commandGroups() const {
            return _commands;
        }
        inline const std::vector<Group> &optionGroups() const {
            return _options;
        }
        inline const std::vector<Group> &argumentGroups() const {
            return _arguments;
        }
        inline bool isEmpty() const {
            return _commands.empty() && _options.empty() && _arguments.empty();
        }

    private:
        std::vector<Group> _commands;
        std::vector<Group> _options;
        std::vector<Group> _arguments;
    };

    /// A command with its arguments, its options and its subcommands, if any.
    class Command {
    public:
        /// The function run if this command is the reached command. Its return value is the
        /// return value of the program.
        using Handler = std::function<int(const ParseResult &)>;
        /// The function run before the handler. A return value of zero continues the run. Any
        /// other value ends the run and becomes the return value of the program.
        using PreHandler = std::function<int(const ParseResult &)>;
        /// The function run after the handler. It receives the current return value and returns
        /// the value that replaces it.
        using PostHandler = std::function<int(const ParseResult &, int)>;

        Command() = default;

        inline Command(std::string name, std::string desc = {})
            : _name(std::move(name)), _desc(std::move(desc)) {
        }

        inline Command &addArgument(Argument argument) {
            _args.emplace_back(std::move(argument));
            return *this;
        }
        inline Command &addArguments(std::vector<Argument> arguments) {
            for (auto &item : arguments) {
                addArgument(std::move(item));
            }
            return *this;
        }
        inline Command &addOption(Option option) {
            _options.emplace_back(std::move(option));
            return *this;
        }
        inline Command &addOptions(std::vector<Option> options) {
            for (auto &item : options) {
                addOption(std::move(item));
            }
            return *this;
        }
        inline Command &addCommand(Command command) {
            _commands.emplace_back(std::move(command));
            return *this;
        }
        inline Command &addCommands(std::vector<Command> commands) {
            for (auto &item : commands) {
                addCommand(std::move(item));
            }
            return *this;
        }

        inline Command &setHandler(Handler handler) {
            _handler = std::move(handler);
            return *this;
        }
        /// Sets the function run before the handler if the command reached is this command or a
        /// descendant of it. Pre handlers run from the root downward, and the first nonzero
        /// return value ends the run. A typical use is applying a recursive option.
        /// \sa ParseResult::invoke()
        inline Command &setPreHandler(PreHandler handler) {
            _preHandler = std::move(handler);
            return *this;
        }
        /// Sets the function run after the handler if the command reached is this command or a
        /// descendant of it, and the pre handler of this command is absent or returned zero. Post
        /// handlers run from the innermost command outward, each receiving the previous result.
        /// \sa ParseResult::invoke()
        inline Command &setPostHandler(PostHandler handler) {
            _postHandler = std::move(handler);
            return *this;
        }
        inline Command &setCatalogue(CommandCatalogue catalogue) {
            _catalogue = std::move(catalogue);
            return *this;
        }
        /// Sets the text that a Version option prints.
        inline Command &setVersion(std::string version) {
            _version = std::move(version);
            return *this;
        }

        /// Adds the version option, with the priority level that allows the option to take
        /// effect on a command line that lacks every required item.
        ///
        /// The option belongs to this command, and descendant commands inherit the version text.
        /// A subcommand therefore adds its own option and either specifies its own version text
        /// or inherits the version text. An empty \a version selects inheritance, and the option
        /// of this command prints the version text of the nearest ancestor command that has a
        /// version text. If no command on the path has a version text, invoke() leaves the
        /// Version role to the handler.
        /// \sa ParseResult::versionText()
        ///
        /// \code
        ///   root.addVersionOption("1.0");
        ///   root.findCommand("sub")->addVersionOption("");   // prog sub --version prints 1.0
        /// \endcode
        inline Command &addVersionOption(std::string version, std::vector<std::string> tokens = {},
                                         std::string desc = {}) {
            _version = std::move(version);
            return addOption(Option(Option::Version, std::move(tokens), std::move(desc))
                                 .prior(Option::IgnoreMissingSymbols));
        }

        /// Adds the help option, with a priority level that allows the option to take effect on
        /// a command line that lacks every required item.
        ///
        /// \param showIfNoArguments Whether the help text is shown for an empty command line, so
        ///        that the program name alone prints the help text.
        /// \param recursive Whether the option is also in scope for the subcommands.
        /// \param tokens The spellings. If empty, the conventional spellings are used.
        /// \param desc The description. If empty, the conventional description is used.
        inline Command &addHelpOption(bool showIfNoArguments = false, bool recursive = false,
                                      std::vector<std::string> tokens = {}, std::string desc = {}) {
            return addOption(Option(Option::Help, std::move(tokens), std::move(desc))
                                 .prior(showIfNoArguments ? Option::AutoSetWhenNoSymbols
                                                          : Option::IgnoreMissingSymbols)
                                 .recursive(recursive));
        }
        inline Command &setDescription(std::string desc) {
            _desc = std::move(desc);
            return *this;
        }

        inline const std::string &name() const {
            return _name;
        }
        inline const std::string &description() const {
            return _desc;
        }
        inline const std::string &version() const {
            return _version;
        }
        inline const std::vector<Argument> &arguments() const {
            return _args;
        }
        inline const std::vector<Option> &options() const {
            return _options;
        }
        inline const std::vector<Command> &commands() const {
            return _commands;
        }
        inline const Handler &handler() const {
            return _handler;
        }
        inline const PreHandler &preHandler() const {
            return _preHandler;
        }
        inline const PostHandler &postHandler() const {
            return _postHandler;
        }
        inline const CommandCatalogue &catalogue() const {
            return _catalogue;
        }

        /// Returns the subcommand named \a name, or null if no such subcommand exists. Only
        /// direct subcommands are searched.
        inline const Command *findCommand(std::string_view name) const {
            for (const auto &item : _commands) {
                if (item._name == name) {
                    return &item;
                }
            }
            return nullptr;
        }

        /// Returns the option that has the spelling \a token, or null if no such option exists.
        /// Every spelling of an option matches, not only the first spelling.
        inline const Option *findOption(std::string_view token) const {
            for (const auto &item : _options) {
                for (const auto &spelling : item.tokens()) {
                    if (spelling == token) {
                        return &item;
                    }
                }
            }
            return nullptr;
        }

    private:
        std::string _name;
        std::string _desc;
        std::string _version;
        std::vector<Argument> _args;
        std::vector<Option> _options;
        std::vector<Command> _commands;
        Handler _handler;
        PreHandler _preHandler;
        PostHandler _postHandler;
        CommandCatalogue _catalogue;
    };

    /// The values given to one option.
    ///
    /// An OptionResult is a view onto the ParseResult that produced it, in the same way as
    /// \c std::string_view is a view onto a string. It owns no storage and extends no lifetime.
    /// Because an OptionResult exists only for an option that was given, no empty OptionResult
    /// exists and no accessor needs to check for absence.
    ///
    /// \warning An OptionResult must not outlive its ParseResult.
    ///          \c parser.parse(args).option("-f") reads freed storage after the end of the full
    ///          expression, because the ParseResult is a temporary.
    /// \sa ParseResult::option()
    class STDC_EXPORT OptionResult {
    public:
        /// One occurrence of the option, with the same accessors as a ParseResult.
        ///
        /// The only difference between a command and an option is that a command is given once
        /// and an option may be given many times. An Occurrence therefore corresponds to the
        /// ParseResult of a command, with the same four accessors under the same four names,
        /// and an OptionResult is a sequence of occurrences.
        ///
        /// \warning Because an Occurrence is a view onto a view, it remains valid exactly as long
        ///          as the ParseResult. An Occurrence always represents an occurrence that
        ///          took place, which the precondition of OptionResult::at() guarantees.
        class STDC_EXPORT Occurrence {
        public:
            /// Returns the first value of the \a index'th argument as text, the default value
            /// if the argument is absent and has a default value, or \c std::nullopt if neither
            /// exists.
            ///
            /// An option given an empty value, as in \c --prefix= , has a value, which is the
            /// empty string. The return type is therefore an optional instead of a string,
            /// because the presence of a token and the emptiness of a token are separate
            /// properties.
            ///
            /// \warning The returned view points into the ParseResult and remains valid exactly
            ///          as long as the ParseResult. value<T>() returns a value that owns its
            ///          storage.
            std::optional<std::string_view> rawValue(int index = 0) const;

            /// Returns every value of the \a index'th argument in this occurrence. The list
            /// contains more than one value only if the argument accepts more than one value.
            ///
            /// \warning The returned views point into the ParseResult, with the same lifetime as
            ///          the view returned by rawValue().
            std::vector<std::string_view> rawValues(int index = 0) const;

            /// Returns rawValue() converted to \c T, or \c std::nullopt if no value exists or
            /// the value is not a valid \c T.
            template <class T = std::string>
            std::optional<T> value(int index = 0) const {
                auto raw = rawValue(index);
                if (!raw) {
                    return std::nullopt;
                }
                T out{};
                if (!value_traits<T>::parse(*raw, &out)) {
                    return std::nullopt;
                }
                return out;
            }

            /// Returns every value of the \a index'th argument in this occurrence converted to
            /// \c T, or \c std::nullopt if one of the values is not a valid \c T.
            template <class T = std::string>
            std::optional<std::vector<T>> values(int index = 0) const {
                std::vector<T> out;
                for (auto raw : rawValues(index)) {
                    T item{};
                    if (!value_traits<T>::parse(raw, &item)) {
                        return std::nullopt;
                    }
                    out.push_back(std::move(item));
                }
                return out;
            }

        private:
            friend class OptionResult;
            inline Occurrence(const void *data, int n) : _data(data), _n(n) {
            }
            const void *_data;
            int _n;
        };

        /// Returns the number of occurrences of the option, which is at least one.
        int count() const;

        /// Returns the declaration of the option.
        const Option *option() const;

        /// Returns the \a n'th occurrence of the option, counting from zero.
        ///
        /// \code
        ///   for (int n = 0; n < given.count(); ++n) {
        ///       take(given.at(n).values());
        ///   }
        /// \endcode
        ///
        /// \pre \a n is at least zero and less than count(). Any other value causes undefined
        ///      behavior and fails an assertion in a debug build. Because an Occurrence is a view
        ///      onto an occurrence that took place, no empty Occurrence exists to be returned
        ///      and no accessor of Occurrence checks for absence. For the same reason,
        ///      ParseResult::option() returns an optional and OptionResult does not.
        Occurrence at(int n) const;

        /// The four accessors below are shorthands for the four accessors of the first
        /// occurrence. Nearly every option is given once, and for such an option the first
        /// occurrence is the only possible interpretation.
        ///
        /// Unlike a command, an option may be given repeatedly. Repetition is exposed through
        /// at() and the two accessors below with the \c all prefix. Neither form is merged into
        /// these four accessors. An option read without an occurrence index is read from the
        /// first occurrence.
        inline std::optional<std::string_view> rawValue(int index = 0) const {
            return at(0).rawValue(index);
        }
        inline std::vector<std::string_view> rawValues(int index = 0) const {
            return at(0).rawValues(index);
        }
        /// \code
        ///   int jobs = default_jobs();
        ///   if (auto given = result.option("-j")) {
        ///       jobs = given->value<int>().value_or(jobs);
        ///   }
        /// \endcode
        template <class T = std::string>
        std::optional<T> value(int index = 0) const {
            return at(0).value<T>(index);
        }
        template <class T = std::string>
        std::optional<std::vector<T>> values(int index = 0) const {
            return at(0).values<T>(index);
        }

        /// Returns every value of the \a index'th argument across all occurrences, in the order
        /// written.
        ///
        /// This function is the usual query for a repeated option. For \c -I \c a \c -I \c b it
        /// returns both values. It is separate from the four accessors above, because those
        /// accessors would otherwise behave differently for an option given once and for the
        /// same option given twice.
        ///
        /// \warning The returned views point into the ParseResult and remain valid exactly as
        ///          long as the ParseResult. allValues<T>() returns values that own their
        ///          storage.
        std::vector<std::string_view> allRawValues(int index = 0) const;

        /// Returns allRawValues() converted to \c T, or \c std::nullopt if one of the values is
        /// not a valid \c T. An empty vector indicates that no values exist.
        template <class T = std::string>
        std::optional<std::vector<T>> allValues(int index = 0) const {
            std::vector<T> out;
            for (auto raw : allRawValues(index)) {
                T item{};
                if (!value_traits<T>::parse(raw, &item)) {
                    return std::nullopt;
                }
                out.push_back(std::move(item));
            }
            return out;
        }

    private:
        friend class ParseResult;
        inline OptionResult(const void *data) : _data(data) {
        }
        const void *_data;
    };

    /// The print style of a run of help text.
    ///
    /// ParseResult::helpText() returns plain text and ignores the style. ParseResult::showHelp()
    /// prints the text and applies the style.
    struct TextStyle {
        /// A bitwise or of console::style values.
        int style = console::nostyle;
        /// A single console::color value, not a bitwise or of several values.
        int foreground = console::nocolor;
        int background = console::nocolor;
    };

    inline bool operator==(const TextStyle &a, const TextStyle &b) {
        return a.style == b.style && a.foreground == b.foreground && a.background == b.background;
    }

    inline bool operator!=(const TextStyle &a, const TextStyle &b) {
        return !(a == b);
    }

    /// One block of the help text, consisting of a heading followed by either a paragraph or a
    /// two-column list.
    ///
    /// Help blocks are the content of the help text before layout. A program that requires an
    /// arrangement that HelpLayout cannot express obtains the blocks from
    /// ParseResult::helpBlocks() and prints them itself.
    class HelpBlock {
    public:
        /// One row of a list, with the name in the left column and the description in the right
        /// column.
        struct Entry {
            std::string left;
            std::string right;
        };

        /// The part of the help text that a block represents.
        ///
        /// \note One role produces zero or more blocks. A command without options produces no
        ///       Options block, and a CommandCatalogue distributes the options of a command
        ///       across one block per group.
        enum Role {
            /// The text above all other blocks, printed without a heading.
            Prologue,
            Description,
            Usage,
            Arguments,
            Options,
            /// The recursive options declared by the ancestors of the command, which are
            /// therefore in scope at the command.
            ///
            /// The help text titles this block "Global options", because readers of help pages
            /// know this term for an option that the command does not declare itself.
            InheritedOptions,
            Commands,
            /// The text below all other blocks, printed without a heading.
            Epilogue,
            /// A block defined by the program.
            Custom,
        };

        Role role = Custom;
        /// The heading without its trailing colon, which the layout adds. An empty title
        /// indicates that the block has no heading and that its body starts at the margin.
        std::string title;
        /// The body of a prose block. Existing newlines are preserved.
        std::string text;
        /// The rows of a list block. A block is either prose or a list, never both.
        std::vector<Entry> entries;

        TextStyle titleStyle;
        /// The style of the prose of a paragraph and of the right-hand column of a list.
        TextStyle bodyStyle;
        /// The style of the left-hand column of a list, which distinguishes the names from their
        /// descriptions.
        TextStyle entryStyle;

        inline bool isEmpty() const {
            return text.empty() && entries.empty();
        }
    };

    /// The blocks of the help text, their order, and the style of each block.
    ///
    /// HelpLayout is a plain value type. A program typically starts from defaultLayout() and
    /// modifies the required settings:
    ///
    /// \code
    ///   auto layout = cli::HelpLayout::defaultLayout();
    ///   layout.setTitleStyle({console::bold});
    ///   layout.setBodyStyle(cli::HelpBlock::Epilogue, {console::bold, console::yellow});
    ///   parser.setHelpLayout(layout);
    /// \endcode
    class HelpLayout {
    public:
        /// Returns a layout that contains every role except HelpBlock::Custom once, in the
        /// default order.
        static inline HelpLayout defaultLayout() {
            HelpLayout res;
            for (auto role : {HelpBlock::Prologue, HelpBlock::Description, HelpBlock::Usage,
                              HelpBlock::Arguments, HelpBlock::Options, HelpBlock::InheritedOptions,
                              HelpBlock::Commands, HelpBlock::Epilogue}) {
                res.add(role);
            }
            return res;
        }

        /// Appends the standard block for \a role. The contents of a role added twice appear
        /// twice.
        inline HelpLayout &add(HelpBlock::Role role) {
            HelpBlock block;
            block.role = role;
            _blocks.push_back(std::move(block));
            return *this;
        }

        /// Appends a program-defined block, which is laid out and aligned like the standard
        /// blocks.
        inline HelpLayout &add(HelpBlock block) {
            _blocks.push_back(std::move(block));
            return *this;
        }

        /// \name Styling
        ///
        /// The overloads without a role apply to every block that the layout contains at the
        /// time of the call. Program-defined blocks must therefore be added before these
        /// overloads are called.
        /// @{
        inline HelpLayout &setTitleStyle(TextStyle style) {
            for (auto &block : _blocks) {
                block.titleStyle = style;
            }
            return *this;
        }
        inline HelpLayout &setTitleStyle(HelpBlock::Role role, TextStyle style) {
            for (auto &block : _blocks) {
                if (block.role == role) {
                    block.titleStyle = style;
                }
            }
            return *this;
        }
        inline HelpLayout &setBodyStyle(TextStyle style) {
            for (auto &block : _blocks) {
                block.bodyStyle = style;
            }
            return *this;
        }
        inline HelpLayout &setBodyStyle(HelpBlock::Role role, TextStyle style) {
            for (auto &block : _blocks) {
                if (block.role == role) {
                    block.bodyStyle = style;
                }
            }
            return *this;
        }
        inline HelpLayout &setEntryStyle(TextStyle style) {
            for (auto &block : _blocks) {
                block.entryStyle = style;
            }
            return *this;
        }
        inline HelpLayout &setEntryStyle(HelpBlock::Role role, TextStyle style) {
            for (auto &block : _blocks) {
                if (block.role == role) {
                    block.entryStyle = style;
                }
            }
            return *this;
        }
        /// @}

        /// Returns the block slots in order. A slot for a standard role contains no content, only
        /// the styles applied to the blocks of that role.
        inline const std::vector<HelpBlock> &blocks() const {
            return _blocks;
        }

        inline bool isEmpty() const {
            return _blocks.empty();
        }

    private:
        std::vector<HelpBlock> _blocks;
    };

    /// The interpretation of a command line, or the reason for a parse failure.
    class STDC_EXPORT ParseResult {
    public:
        enum Error {
            NoError,
            UnknownOption,
            UnknownCommand,
            MissingOptionArgument,
            MissingCommandArgument,
            TooManyArguments,
            InvalidArgumentValue,
            MissingRequiredOption,
            OptionOccurTooMuch,
            ArgumentTypeMismatch,
            ArgumentValidateFailed,
            PriorOptionWithArguments,
            PriorOptionWithOptions,
            ErrorReadingResponseFile,
        };

        ParseResult();

        /// A ParseResult is move-only, because one parse of a command line produces one result
        /// with one owner.
        ParseResult(const ParseResult &RHS) = delete;
        ParseResult &operator=(const ParseResult &RHS) = delete;
        ParseResult(ParseResult &&RHS) noexcept;
        ParseResult &operator=(ParseResult &&RHS) noexcept;
        ~ParseResult();

        inline bool isValid() const {
            return error() == NoError;
        }
        Error error() const;
        /// Returns the error description, formatted for printing.
        const std::string &errorText() const;
        /// Returns the declared names similar to the mistyped name as suggestions, formatted for
        /// printing. Returns an empty string if no name is similar or if the failure is not
        /// caused by a mistyped name.
        std::string correctionText() const;

        /// Returns the reached command, which is the root if no subcommand was named.
        const Command *command() const;
        /// Returns the commands from the root to the command reached, in that order. The pointers
        /// remain valid for the lifetime of this result.
        const std::vector<const Command *> &commandPath() const;

        /// Returns the text that a Version option prints, which is the version text of the
        /// innermost command on the path that has a version text. A version text set on the
        /// root therefore applies to every descendant command.
        /// \sa Command::setVersion(), Command::addVersionOption()
        std::string versionText() const;

        /// Performs the complete command line handling of a \c main function and returns the
        /// value for \c main to return.
        ///
        /// \li If the parse failed, reports the error and returns \a errorCode.
        /// \li If a Help option, or a Version option with nonempty versionText(), was given,
        ///     prints the corresponding text and returns 0 without running the handler. This
        ///     rule prevents \c prog \c copy \c --help from performing the copy operation.
        /// \li Otherwise runs the handler of the reached command, or returns \a errorCode if the
        ///     command has no handler. This case includes a Version option for which the command
        ///     path supplies no text, and the handler may handle that option itself.
        ///
        /// The pre handlers of the commands on the path run before the handler, from the root
        /// downward. The first nonzero return value skips the remaining pre handlers and the
        /// handler. The post handlers then run from the innermost command outward for each
        /// command whose pre handler is absent or returned zero, and the return value of the
        /// last post handler is returned. None of these run if the handler does not.
        ///
        /// A program that handles any of these cases itself calls parse() instead. A caller of
        /// this function has no remaining step to perform.
        inline int invoke(int errorCode = -1) const {
            if (!isValid()) {
                showError();
                return errorCode;
            }
            if (isRoleSet(Option::Help)) {
                showHelp();
                return 0;
            }
            if (isRoleSet(Option::Version)) {
                auto version = versionText();
                if (!version.empty()) {
                    console::u8puts(version);
                    return 0;
                }
            }
            const Command *target = command();
            if (!target || !target->handler()) {
                return errorCode;
            }

            // Only the commands whose pre handlers succeeded receive a post handler call. These
            // commands form a prefix of the path, in the same way as objects whose constructors
            // completed.
            const auto &path = commandPath();
            size_t entered = 0;
            int code = 0;
            for (; entered < path.size(); ++entered) {
                const auto &pre = path[entered]->preHandler();
                if (pre && (code = pre(*this)) != 0) {
                    break;
                }
            }
            if (entered == path.size()) {
                code = target->handler()(*this);
            }
            while (entered > 0) {
                const auto &post = path[--entered]->postHandler();
                if (post) {
                    code = post(*this, code);
                }
            }
            return code;
        }

        /// Returns whether an option with \a role was given, in any of its spellings.
        bool isRoleSet(Option::Role role) const;

        /// Returns the values given to the option \a token, or \c std::nullopt if the option was
        /// not given. The latter case includes an undeclared option as well as a declared option
        /// that was not written.
        ///
        /// \code
        ///   if (auto force = result.option("-f")) { ... }
        /// \endcode
        ///
        /// \note Because the returned OptionResult reads from this result without copying, it
        ///       remains valid only as long as this result. OptionResult documents the resulting
        ///       restrictions.
        /// \sa OptionResult
        std::optional<OptionResult> option(std::string_view token) const;

        /// Returns the \a index'th positional argument of the reached command as text, the
        /// default value if the argument is absent and has a default value, or \c std::nullopt
        /// if neither exists.
        ///
        /// An argument given an empty string has a value, which is the empty string. The return
        /// type is therefore an optional instead of a string.
        ///
        /// \warning The returned view points into this result and remains valid exactly as long
        ///          as this result. value<T>() returns a value that owns its storage.
        std::optional<std::string_view> rawValue(int index = 0) const;
        /// Returns every token of the \a index'th positional argument.
        ///
        /// \warning The returned views point into this result, with the same lifetime as the
        ///          view returned by rawValue().
        std::vector<std::string_view> rawValues(int index = 0) const;

        /// Returns rawValue() converted to \c T, or \c std::nullopt if no value can be
        /// converted.
        ///
        /// \c std::nullopt indicates one of two cases. Either no token is present and no default
        /// value replaces it, or the token is not a valid \c T. Declaring the type on the
        /// Argument turns the second case into a parse diagnostic, and \c std::nullopt then
        /// indicates only the first case.
        ///
        /// \code
        ///   int jobs = result.value<int>(0).value_or(default_jobs());
        /// \endcode
        template <class T = std::string>
        std::optional<T> value(int index = 0) const {
            auto raw = rawValue(index);
            if (!raw) {
                return std::nullopt;
            }
            T out{};
            if (!value_traits<T>::parse(*raw, &out)) {
                return std::nullopt;
            }
            return out;
        }
        /// Returns every token of the \a index'th argument converted to \c T, or \c std::nullopt
        /// if one of the tokens is not a valid \c T. An empty vector indicates that the argument
        /// has no tokens.
        template <class T = std::string>
        std::optional<std::vector<T>> values(int index = 0) const {
            std::vector<T> out;
            for (auto raw : rawValues(index)) {
                T item{};
                if (!value_traits<T>::parse(raw, &item)) {
                    return std::nullopt;
                }
                out.push_back(std::move(item));
            }
            return out;
        }

        /// Returns the first value of the first argument in the first occurrence of the option
        /// \a token, converted to \c T. Returns \c std::nullopt if the option was not given, if
        /// the argument has no value, or if the value is not a valid \c T.
        template <class T = std::string>
        std::optional<T> valueForOption(std::string_view token) const {
            auto given = option(token);
            return given ? given->value<T>() : std::nullopt;
        }

        /// Returns the text printed above and below the help text, as set on the parser.
        const std::string &prologue() const;
        const std::string &epilogue() const;
        /// Returns the help layout, which specifies the blocks of the help text, their order,
        /// and their styles.
        const HelpLayout &helpLayout() const;
        /// Returns the recursive options declared by the ancestors of the reached command. These
        /// options are in scope at the reached command, and their requirements are enforced
        /// there. The parser collects them while traversing the command path.
        ///
        /// \warning The pointers point into the command tree and remain valid as long as the
        ///          command tree exists.
        std::vector<const Option *> inheritedOptions() const;

        /// Returns the help text of the reached command as blocks, in the order that the layout
        /// specifies and with the groups of the catalogue already split.
        ///
        /// helpText() lays out these blocks. A program uses this function if neither HelpLayout
        /// nor a HelpFormatter can express the required output, and prints the blocks in its own
        /// format.
        std::vector<HelpBlock> helpBlocks() const;

        /// Returns the help text of the reached command, including the prologue and the
        /// epilogue.
        std::string helpText() const;
        /// Writes helpText() to stdout with the styles that the layout specifies.
        void showHelp() const;
        /// Writes the error description to stderr, followed by a line that indicates how to
        /// display the help text. Does nothing if the parse succeeded.
        void showError() const;

    private:
        friend class Parser;

        using Impl = detail::parse_data;
        std::unique_ptr<Impl> _impl;
    };

    /// Parses command line arguments against a command tree into a ParseResult.
    ///
    /// \li Subcommands form the first contiguous part of the line. Once an option or argument
    ///     is read, no later token can name a subcommand. The command line
    ///     \c prog \c copy \c -f \c x therefore reaches \c copy, while
    ///     \c prog \c -V \c copy \c x does not.
    /// \li A positional token that the reached command cannot accept is an error.
    /// \li An option that requires a value does not accept as its value a token that is a
    ///     declared option of the same command. Any other token that begins with a dash is
    ///     accepted as a value at that position.
    class STDC_EXPORT Parser {
    public:
        /// Extensions of the token syntax beyond the standard syntax.
        enum ParseOption {
            Standard = 0,
            /// Subcommand names match without regard to case.
            IgnoreCommandCase = 0x1,
            /// Option tokens match without regard to case.
            IgnoreOptionCase = 0x2,
            /// \c -abc means \c -a \c -b \c -c.
            AllowUnixGroupFlags = 0x4,
            /// \c /f is another way of writing \c -f.
            AllowDosShortOptions = 0x8,
            /// A single dash does not start an option.
            DontAllowUnixShortOptions = 0x10,
            /// \c \@file is replaced by the lines of that file.
            EnableResponseFile = 0x20,
        };
        STDC_DECLARE_FLAGS(ParseOptions, ParseOption)

        /// Additional content of the help text beyond the required content. HelpLayout, not
        /// this enumeration, controls the selection and order of the blocks.
        enum DisplayOption {
            Normal = 0,
            /// Show the default value of an argument.
            ShowArgumentDefaultValue = 0x1,
            /// List the expected values of an argument that accepts a fixed set of values.
            ShowArgumentExpectedValues = 0x2,
            /// Mark the required options.
            ShowOptionIsRequired = 0x4,
            /// Align the descriptions of all groups with each other, so that a catalogue
            /// appears as one table.
            AlignAllCatalogues = 0x8,
            /// Prevent showError() from suggesting declared names similar to the mistyped name.
            SkipCorrection = 0x10,
        };
        STDC_DECLARE_FLAGS(DisplayOptions, DisplayOption)

        Parser();
        explicit Parser(Command root);
        ~Parser();

        Parser(const Parser &RHS) = delete;
        Parser &operator=(const Parser &RHS) = delete;

        /// A Parser is movable, so that a function can build and return a parser.
        Parser(Parser &&RHS) noexcept;
        Parser &operator=(Parser &&RHS) noexcept;

        /// Sets a new root command, replacing the root command given to the constructor or used
        /// by the last parse().
        void setRootCommand(Command root);
        const Command &rootCommand() const;

        /// Sets and returns the text printed above the help text, which is the prologue, and the
        /// text printed below it, which is the epilogue.
        void setPrologue(std::string text);
        const std::string &prologue() const;
        void setEpilogue(std::string text);
        const std::string &epilogue() const;

        void setDisplayOptions(DisplayOptions options);
        DisplayOptions displayOptions() const;

        /// Sets the number of columns available to the help text, which is the wrap width of its
        /// descriptions.
        ///
        /// \param width the column count, or 0 to query the terminal each time the text is
        ///        generated
        /// \note 0 is the default. If no terminal is available, as when the output is a pipe,
        ///       the width is 80 columns. The help text of a program is therefore identical
        ///       regardless of how the output is captured.
        /// \sa console::width()
        void setTextWidth(int width);
        int textWidth() const;

        /// Sets the indentation of the body of a section from the margin.
        /// \note The default is four columns.
        void setIndent(int columns);
        int indent() const;

        /// Sets the number of columns between the two columns of a list.
        /// \note The default is four columns.
        void setSpacing(int columns);
        int spacing() const;

        /// Sets the help layout, which specifies the blocks of the help text, their order, and
        /// their styles.
        /// \note A parser starts with HelpLayout::defaultLayout().
        void setHelpLayout(HelpLayout layout);
        const HelpLayout &helpLayout() const;

        /// Sets the formatter that builds and lays out the help blocks, for a program that
        /// requires output that no help layout or size setting can express.
        ///
        /// \note A parser starts with a plain HelpFormatter, and a null pointer restores it. A
        ///       formatter stores no state between calls. Therefore, one formatter may be shared.
        ///       A HelpFormatter consists of several overridable levels instead of one method.
        /// \sa HelpFormatter
        void setHelpFormatter(std::shared_ptr<HelpFormatter> formatter);
        const std::shared_ptr<HelpFormatter> &helpFormatter() const;

        /// Checks whether the command tree can be parsed under \a parseOptions.
        ///
        /// \return \c std::nullopt if the tree is valid, or a description of the first problem
        ///         found
        /// \note parse() performs this check only through an assertion. A program that obtains
        ///       a command tree from an external source should call this function explicitly
        ///       before parsing.
        std::optional<std::string> validate(ParseOptions parseOptions = Standard) const;

        /// Parses \a args against the command tree.
        ///
        /// \pre validate() returns \c std::nullopt. This is asserted in a debug build. A release
        ///      build does not traverse the tree before parsing.
        /// \note A program whose command tree is built dynamically, such as from plugins,
        ///       should call validate() explicitly before this function, even in a release
        ///       build.
        inline ParseResult parse(array_view<std::string> args,
                                 ParseOptions parseOptions = Standard) const {
            assert(!validate(parseOptions).has_value() && "the command tree is invalid");
            return parseImpl(args, parseOptions);
        }
        /// Parses \a args and performs the complete handling of a \c main function, which
        /// reports a failure and handles a Help or Version option before any handler runs.
        /// \sa ParseResult::invoke()
        inline int invoke(array_view<std::string> args, int errorCode = -1,
                          ParseOptions parseOptions = Standard) const {
            return parse(args, parseOptions).invoke(errorCode);
        }

        /// Parses the arguments passed to \c main.
        ///
        /// \warning This overload is unsuitable on Windows. The arguments of \c main are in the
        ///          system code page on Windows, while this library requires UTF-8. A non-ASCII
        ///          argument is therefore corrupted. system::command_line_arguments() returns
        ///          the same list converted to UTF-8 on every platform.
        inline ParseResult parse(int argc, char **argv,
                                 ParseOptions parseOptions = Standard) const {
            return parse(std::vector<std::string>(argv, argv + argc), parseOptions);
        }
        inline int invoke(int argc, char **argv, int errorCode = -1,
                          ParseOptions parseOptions = Standard) const {
            return parse(argc, argv, parseOptions).invoke(errorCode);
        }

    private:
        ParseResult parseImpl(array_view<std::string> args, ParseOptions parseOptions) const;

        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    STDC_DECLARE_OPERATORS_FOR_FLAGS(Parser::ParseOptions)
    STDC_DECLARE_OPERATORS_FOR_FLAGS(Parser::DisplayOptions)

    /// The dimensions of the help text and its additional content.
    struct HelpSizes {
        /// The indentation of the body of a section from the margin.
        int indent = 4;
        /// The number of columns between the two columns of a list.
        int spacing = 4;
        /// The number of columns available to the whole text.
        int textWidth = 80;
        Parser::DisplayOptions displayOptions;
    };

    /// The generation of the help text, from a command tree at the top to printable runs at the
    /// bottom. Level 4 of \ref cli_help.
    ///
    /// \verbatim
    ///     the reached command and ParseResult::helpLayout()
    ///                    |
    ///                    v
    ///    +---- blocks(result, sizes) ---------- composition of the text: the blocks, their
    ///    |               |                      titles, and the contents of their two
    ///    |               |                      columns
    ///    |               v
    ///    |      displayed(Option, bool) ------- spelling of one name, computed before any
    ///    |               |                      block exists
    ///    |               v                        "-o, --output <file>"
    ///    |      displayed(Argument) -----------   "<file>", "[<file>]", "<file>..."
    ///    |
    ///    +--> std::vector<HelpBlock> ---------> ParseResult::helpBlocks() stops here and
    ///                    |                      returns these blocks
    ///                    v
    ///    +---- render(blocks, sizes) ---------- the whole page: measures every list and
    ///    |               |                      separates consecutive blocks with a blank line
    ///    |               v
    ///    |      renderBlock(block, sizes, widest)
    ///    |                                      one block: its heading, its columns aligned
    ///    |                                      to widest, its descriptions wrapped
    ///    |
    ///    +--> std::vector<Run>
    ///                    |
    ///          +---------+---------+
    ///          v                   v
    ///   ParseResult::        ParseResult::
    ///     helpText()           showHelp()
    ///   joins the runs and   prints the runs and
    ///   discards styles      applies the styles
    /// \endverbatim
    ///
    /// A program subclasses HelpFormatter and overrides the level that produces the part to be
    /// changed. <b>Every default implementation is public and callable</b>. A change to one detail
    /// therefore requires only a call to the base implementation instead of a reimplemented
    /// renderer:
    ///
    /// \code
    ///   struct Shouty : cli::HelpFormatter {
    ///       std::vector<cli::HelpBlock> blocks(const cli::ParseResult &result,
    ///                                          const cli::HelpSizes &sizes) const override {
    ///           auto res = HelpFormatter::blocks(result, sizes);
    ///           for (auto &block : res) {
    ///               block.title = stdc::str::to_upper(block.title);
    ///           }
    ///           return res;
    ///       }
    ///   };
    ///   parser.setHelpFormatter(std::make_shared<Shouty>());
    /// \endcode
    ///
    /// Each level calls the levels below it through \c this. An override of
    /// displayed(const Argument &) alone therefore changes every metavar in the usage line and
    /// in every list, and affects nothing else.
    ///
    /// A HelpFormatter stores no state between calls. Therefore, every parser in a program can
    /// share one formatter. The only state is HelpSizes, which the parser owns and passes to the
    /// formatter.
    class STDC_EXPORT HelpFormatter {
    public:
        /// One run of help text and its style. ParseResult::helpText() joins the runs and
        /// discards the styles. ParseResult::showHelp() prints the runs and applies the styles.
        struct Run {
            TextStyle style;
            std::string text;
        };

        HelpFormatter();
        virtual ~HelpFormatter();

        /// Returns the displayed form of an argument. The form is \c \<file\>, or \c [\<file\>]
        /// if the argument is optional, followed by an ellipsis if the argument repeats.
        virtual std::string displayed(const Argument &argument) const;

        /// Returns the displayed form of an option followed by its arguments. A list shows every
        /// spelling, and the usage line shows only the first spelling.
        ///
        /// \note displayed(const Argument &) formats the arguments. An override of that function
        ///       therefore also affects this function.
        virtual std::string displayed(const Option &option, bool allSpellings) const;

        /// Returns the displayed form of a subcommand, which is its name.
        virtual std::string displayed(const Command &command) const;

        /// Returns one row of a two-column list. The left column contains the output of
        /// displayed(). The right column contains the description and the additions that the
        /// display options select: a default value, the set of expected values, or a mark for a
        /// required item.
        ///
        /// \note displayed() produces the left column. An override of displayed() therefore also
        ///       affects this function. A program overrides this function to change the right
        ///       column.
        virtual HelpBlock::Entry entry(const Argument &argument, const HelpSizes &sizes) const;

        /// \overload
        virtual HelpBlock::Entry entry(const Option &option, const HelpSizes &sizes) const;

        /// \overload
        virtual HelpBlock::Entry entry(const Command &command, const HelpSizes &sizes) const;

        /// Returns the usage line, broken across as many lines as necessary.
        ///
        /// \param command the reached command, which supplies its own options
        /// \param path the commands from the root to \a command, as ParseResult::commandPath()
        ///        returns them
        /// \param inherited the options in scope from the ancestors of \a command, which are
        ///        the only information here that \a command cannot supply
        /// \param sizes the indent and the width at which the line is broken
        /// \note A line break never splits a piece, because an option and its value appear
        ///       unrelated if a line break separates them.
        virtual std::string usageText(const Command &command,
                                      const std::vector<const Command *> &path,
                                      const std::vector<Option> &inherited,
                                      const HelpSizes &sizes) const;

        /// Returns the blocks of the help text, in the order that ParseResult::helpLayout()
        /// specifies and with the groups of the CommandCatalogue already split.
        ///
        /// \note \a sizes differs from the settings that \a result records and must not be
        ///       recomputed from \a result. A configured text width of zero requests a terminal
        ///       query, and \a sizes contains the result of that query. The width is determined
        ///       once and passed to render() as well, so that a terminal resized in between
        ///       cannot cause the usage line and the descriptions to be laid out to different
        ///       widths.
        virtual std::vector<HelpBlock> blocks(const ParseResult &result,
                                              const HelpSizes &sizes) const;

        /// Lays out one block, with its heading above it and its columns aligned to \a widest.
        ///
        /// render() inserts the blank line between consecutive blocks. The returned runs
        /// therefore contain only the block itself.
        virtual std::vector<Run> renderBlock(const HelpBlock &block, const HelpSizes &sizes,
                                             size_t widest) const;

        /// Lays out the whole page. Measures the lists, then calls renderBlock() for each block
        /// and inserts a blank line between consecutive blocks.
        virtual std::vector<Run> render(const std::vector<HelpBlock> &blocks,
                                        const HelpSizes &sizes) const;

        /// Returns the width of the left column of \a block.
        ///
        /// The width is measured in display columns instead of bytes, because a byte count
        /// misaligns the row of a metavar that contains non-ASCII characters.
        static inline size_t widestOf(const HelpBlock &block) {
            size_t res = 0;
            for (const auto &entry : block.entries) {
                res = std::max(res, size_t(console::display_width(entry.left)));
            }
            return res;
        }

        /// Returns the largest left column width across \a blocks, which is the width that
        /// AlignAllCatalogues aligns to.
        static inline size_t widestOf(const std::vector<HelpBlock> &blocks) {
            size_t res = 0;
            for (const auto &block : blocks) {
                res = std::max(res, widestOf(block));
            }
            return res;
        }

        /// Returns \a text broken into lines of at most \a columns columns. Lines break at spaces
        /// if spaces exist, and between characters otherwise. Existing newlines are preserved.
        static std::vector<std::string> wrapped(const std::string &text, int columns);
    };

    /// @}
}

#endif // STDCORELIB_COMMANDLINE_H
