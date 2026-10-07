// SPDX-License-Identifier: MIT

#include "commandline.h"
#include "commandline_p.h"

#include <cerrno>
#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <unordered_map>

#include "console.h"
#include "path.h"
#include "utf.h"
#include "pimpl.h"

namespace stdc::cli {

    namespace detail {

        namespace {

            /// Returns whether \c from_chars consumed all of \a token. A token with characters
            /// after the number, such as \c 12abc, is not a number.
            bool consumed_all(std::string_view token, const char *end) {
                return end == token.data() + token.size();
            }

            /// Removes a leading plus sign, which a command line may contain and \c from_chars
            /// rejects.
            std::string_view drop_leading_plus(std::string_view token) {
                if (token.size() > 1 && token.front() == '+') {
                    token.remove_prefix(1);
                }
                return token;
            }

            template <class T>
            bool parse_floating_with(std::string_view token, T *out,
                                     T (*convert)(const char *, char **)) {
                if (token.empty()) {
                    return false;
                }
                std::string buf(token);
                errno = 0;
                char *end = nullptr;
                T value = convert(buf.c_str(), &end);
                if (end != buf.c_str() + buf.size() || end == buf.c_str() || errno == ERANGE) {
                    return false;
                }
                *out = value;
                return true;
            }

        }

        bool parse_signed(std::string_view token, int64_t *out, int64_t min, int64_t max) {
            token = drop_leading_plus(token);
            if (token.empty()) {
                return false;
            }
            int64_t v = 0;
            auto res = std::from_chars(token.data(), token.data() + token.size(), v);
            if (res.ec != std::errc{} || !consumed_all(token, res.ptr)) {
                return false;
            }
            if (v < min || v > max) {
                return false;
            }
            *out = v;
            return true;
        }

        bool parse_unsigned(std::string_view token, uint64_t *out, uint64_t max) {
            token = drop_leading_plus(token);
            if (token.empty()) {
                return false;
            }
            // No explicit sign check is required, because from_chars into an unsigned type
            // rejects a minus sign. MSVC, libstdc++ and libc++ were each verified to return
            // invalid_argument and to leave the output unmodified. A test covers this as well,
            // because the code relies on a guarantee of the standard libraries rather than of
            // this library.
            uint64_t v = 0;
            auto res = std::from_chars(token.data(), token.data() + token.size(), v);
            if (res.ec != std::errc{} || !consumed_all(token, res.ptr)) {
                return false;
            }
            if (v > max) {
                return false;
            }
            *out = v;
            return true;
        }

        bool parse_floating(std::string_view token, float *out) {
            return parse_floating_with(token, out, std::strtof);
        }

        bool parse_floating(std::string_view token, double *out) {
            return parse_floating_with(token, out, std::strtod);
        }

        bool parse_floating(std::string_view token, long double *out) {
            return parse_floating_with(token, out, std::strtold);
        }

        bool parse_boolean(std::string_view token, bool *out) {
            static const std::string_view yes[] = {"true", "yes", "on", "1"};
            static const std::string_view no[] = {"false", "no", "off", "0"};
            for (auto word : yes) {
                if (str::equals_insensitive(token, word)) {
                    *out = true;
                    return true;
                }
            }
            for (auto word : no) {
                if (str::equals_insensitive(token, word)) {
                    *out = false;
                    return true;
                }
            }
            return false;
        }

    }

    // ---------------------------------------------------------------------------------------
    // Storage
    // ---------------------------------------------------------------------------------------

    namespace {

        /// The tokens assigned to one declared argument. The vector contains more than one token
        /// only if the argument accepts more than one.
        using ArgumentValues = std::vector<std::string>;

        /// One occurrence of an option, with one slot per declared argument. The type is named
        /// after its contents rather than as an occurrence, because the name Occurrence belongs
        /// to the public type OptionResult::Occurrence, which provides read access to it.
        using ArgumentSlots = std::vector<ArgumentValues>;

        struct OptionData {
            const Option *option = nullptr;
            std::vector<ArgumentSlots> occurrences;
        };

        /// The option that a token names, and the value and spelling written in the token.
        struct OptionMatch {
            OptionData *data = nullptr;
            /// The value written against the spelling, after an equals sign or joined to the
            /// spelling. Absent if the token consists of the spelling alone. An absent value is
            /// distinct from an empty value: \c --prefix= sets an empty string.
            std::string_view value;
            /// The spelling as written in the token, so that an error message names the option
            /// as the user typed it rather than as it was declared.
            std::string_view spelling;

            explicit operator bool() const {
                return data != nullptr;
            }
        };

        /// Returns how many of \a available tokens the argument at \a index in \a declared
        /// consumes. \a has_one indicates whether the argument already holds a value.
        ///
        /// One token is reserved for each required argument that follows, because a greedy
        /// argument that consumed every token would leave no token for the destination of
        /// \c copy \c \<src\>... \c \<dest\>. Because the rule is the same for the arguments of a
        /// command and of an option, it is implemented once and used by both.
        ///
        /// If too few tokens remain for every argument, a greedy argument still consumes one
        /// token, because it accepts one or more, and the required argument left without a token
        /// reports its own error. This is the only effect of \a has_one: because a value written
        /// against the option already satisfies the one-or-more requirement, no token is forced
        /// and the reservation applies.
        size_t take_for(const std::vector<Argument> &declared, size_t index, size_t available,
                        bool has_one = false) {
            const auto &argument = declared[index];
            if (argument.arity() == Argument::Single) {
                return 1;
            }
            if (argument.arity() == Argument::Remainder) {
                return available;
            }
            size_t reserved = 0;
            for (size_t j = index + 1; j < declared.size(); ++j) {
                if (declared[j].isRequired()) {
                    ++reserved;
                }
            }
            if (available > reserved) {
                return available - reserved;
            }
            return has_one ? 0 : 1;
        }

        bool same_token(std::string_view a, std::string_view b, bool ignore_case) {
            if (!ignore_case) {
                return a == b;
            }
            return str::equals_insensitive(a, b);
        }

    }

    class detail::parse_data {
    public:
        /// Shared with the parser rather than copied, so that the pointers below remain valid and
        /// the tree is traversed only once.
        std::shared_ptr<const Command> root;
        const Command *target = nullptr;
        /// The commands from the root to the target, in that order.
        std::vector<const Command *> path;

        /// The tokens of each positional argument of the target, indexed by argument.
        std::vector<ArgumentValues> arguments;
        /// Every option in scope: the target's own options and the inherited options.
        std::vector<OptionData> options;
        std::unordered_map<std::string, size_t> by_token;

        /// Copied from the parser, so that a result can print its help without access to the
        /// parser that produced it.
        std::string prologue;
        std::string epilogue;
        HelpLayout help_layout = HelpLayout::defaultLayout();
        /// Shared with the parser rather than copied, because the parser continues to use the
        /// formatter for subsequent parses and this result may outlive the parser. The same
        /// reason applies to root. A unique_ptr is not used because the formatter has no single
        /// owner.
        std::shared_ptr<HelpFormatter> formatter;
        Parser::DisplayOptions display_options;
        int text_width = 0;
        int indent = 4;
        int spacing = 4;

        ParseResult::Error error = ParseResult::NoError;
        std::string error_text;

        /// The token typed in place of a declared name, and the declared names that the token
        /// may be a misspelling of. Edit distances are computed on request rather than here, so
        /// that a program that never prints a correction does not pay for the computation.
        std::string error_token;
        std::vector<std::string> error_candidates;

        const OptionData *find(std::string_view token) const {
            auto it = by_token.find(std::string(token));
            return it == by_token.end() ? nullptr : &options[it->second];
        }

        /// Returns the same entry as find(), for writing by the parser, which fills the entries
        /// in. A separate function is preferred to a const_cast, which would imply that the const
        /// qualifier of find() never held for this data.
        OptionData *findForWriting(std::string_view token) {
            auto it = by_token.find(std::string(token));
            return it == by_token.end() ? nullptr : &options[it->second];
        }
    };

    // ---------------------------------------------------------------------------------------
    // OptionResult
    // ---------------------------------------------------------------------------------------

    // These functions dereference _data without checking it. ParseResult::option() is the only
    // function that constructs an OptionResult, and it does so only if data exists to point to.
    int OptionResult::count() const {
        return int(static_cast<const OptionData *>(_data)->occurrences.size());
    }

    const Option *OptionResult::option() const {
        return static_cast<const OptionData *>(_data)->option;
    }

    OptionResult::Occurrence OptionResult::at(int n) const {
        assert(n >= 0 && n < count() && "there was no such occurrence of this option");
        return {_data, n};
    }

    namespace {

        /// Returns the slots of occurrence \a n. Because the validity of \a n is a precondition of
        /// at(), no check is required here and no result is defined for a nonexistent
        /// occurrence.
        const ArgumentSlots &slots_of(const void *data, int n) {
            const auto &occurrences = static_cast<const OptionData *>(data)->occurrences;
            assert(n >= 0 && size_t(n) < occurrences.size());
            return occurrences[size_t(n)];
        }

    }

    // The slot is a vector of the tokens assigned to the argument. An empty vector and an empty
    // token in the vector are different conditions. Therefore, an absent value is never reported
    // as empty text.
    std::optional<std::string_view> OptionResult::Occurrence::rawValue(int index) const {
        const auto &slots = slots_of(_data, _n);
        if (index < 0 || size_t(index) >= slots.size() || slots[size_t(index)].empty()) {
            return std::nullopt;
        }
        return std::string_view(slots[size_t(index)].front());
    }

    std::vector<std::string_view> OptionResult::Occurrence::rawValues(int index) const {
        std::vector<std::string_view> res;
        const auto &slots = slots_of(_data, _n);
        if (index < 0 || size_t(index) >= slots.size()) {
            return res;
        }
        for (const auto &item : slots[size_t(index)]) {
            res.emplace_back(item);
        }
        return res;
    }

    std::vector<std::string_view> OptionResult::allRawValues(int index) const {
        std::vector<std::string_view> res;
        if (index < 0) {
            return res;
        }
        auto data = static_cast<const OptionData *>(_data);
        for (const auto &slots : data->occurrences) {
            if (size_t(index) >= slots.size()) {
                continue;
            }
            for (const auto &item : slots[size_t(index)]) {
                res.emplace_back(item);
            }
        }
        return res;
    }

    // ---------------------------------------------------------------------------------------
    // ParseResult
    // ---------------------------------------------------------------------------------------

    ParseResult::ParseResult() : _impl(std::make_unique<detail::parse_data>()) {
    }

    ParseResult::ParseResult(ParseResult &&RHS) noexcept = default;
    ParseResult &ParseResult::operator=(ParseResult &&RHS) noexcept = default;
    ParseResult::~ParseResult() = default;

    ParseResult::Error ParseResult::error() const {
        stdc_impl_t;
        return impl.error;
    }

    const std::string &ParseResult::errorText() const {
        stdc_impl_t;
        return impl.error_text;
    }

    namespace {

        /// Returns the number of single-character insertions, deletions and substitutions that
        /// transform \a a into \a b. Only one row of the table is kept, together with one
        /// diagonal value, because each cell reads only the cell to its left, the cell above it
        /// and the cell diagonally above-left. The row is overwritten in place from left to
        /// right. \c diagonal preserves the value of the cell above-left, because the row entry
        /// at that position already holds the current row.
        size_t edit_distance(const std::string &a, const std::string &b) {
            std::vector<size_t> row(b.size() + 1);
            for (size_t j = 0; j <= b.size(); ++j) {
                row[j] = j;
            }
            for (size_t i = 1; i <= a.size(); ++i) {
                size_t diagonal = row[0];
                row[0] = i;
                for (size_t j = 1; j <= b.size(); ++j) {
                    size_t above = row[j];
                    row[j] = std::min(
                        {row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                    diagonal = above;
                }
            }
            return row[b.size()];
        }

    }

    std::string ParseResult::correctionText() const {
        stdc_impl_t;
        const auto &input = impl.error_token;
        if (input.empty() || impl.error_candidates.empty()) {
            return {};
        }

        // The threshold is half the length of the typed token. A looser threshold makes every
        // short name a candidate for every short typo, which is worse than no suggestion.
        const size_t threshold = input.size() / 2;

        // Indented by the same amount as a section body of the help text, because the
        // suggestions form a list under an introductory line and read as a section body.
        const std::string margin(size_t(impl.indent < 0 ? 0 : impl.indent), ' ');
        std::string suggestions;
        for (const auto &item : impl.error_candidates) {
            if (edit_distance(input, item) <= threshold) {
                suggestions += "\n" + margin + item;
            }
        }
        if (suggestions.empty()) {
            return {};
        }
        return "\"" + input + "\" is not matched. Do you mean one of the following?" + suggestions;
    }

    const Command *ParseResult::command() const {
        stdc_impl_t;
        return impl.target;
    }

    const std::vector<const Command *> &ParseResult::commandPath() const {
        stdc_impl_t;
        return impl.path;
    }

    // Returns the version of the innermost command on the path that specifies a version. A
    // subcommand may therefore specify its own version, and every command under a root that
    // specifies a version inherits it.
    std::string ParseResult::versionText() const {
        stdc_impl_t;
        // A parse that fails while expanding a response file ends before the root is added to
        // the path.
        if (impl.path.empty()) {
            return impl.root ? impl.root->version() : std::string();
        }
        std::string res;
        for (const Command *at : impl.path) {
            if (!at->version().empty()) {
                res = at->version();
            }
        }
        return res;
    }


    bool ParseResult::isRoleSet(Option::Role role) const {
        if (role == Option::NoRole) {
            return false;
        }
        stdc_impl_t;
        for (const auto &item : impl.options) {
            if (item.option->role() == role && !item.occurrences.empty()) {
                return true;
            }
        }
        return false;
    }

    // Returns std::nullopt rather than an empty result, so that there is one way to test whether
    // an option was given and one kind of OptionResult to handle.
    std::optional<OptionResult> ParseResult::option(std::string_view token) const {
        stdc_impl_t;
        auto data = impl.find(token);
        if (!data || data->occurrences.empty()) {
            return std::nullopt;
        }
        return OptionResult(data);
    }

    std::optional<std::string_view> ParseResult::rawValue(int index) const {
        stdc_impl_t;
        if (index < 0 || size_t(index) >= impl.arguments.size() ||
            impl.arguments[size_t(index)].empty()) {
            return std::nullopt;
        }
        return std::string_view(impl.arguments[size_t(index)].front());
    }

    std::vector<std::string_view> ParseResult::rawValues(int index) const {
        stdc_impl_t;
        std::vector<std::string_view> res;
        if (index < 0 || size_t(index) >= impl.arguments.size()) {
            return res;
        }
        for (const auto &item : impl.arguments[size_t(index)]) {
            res.emplace_back(item);
        }
        return res;
    }

    const std::string &ParseResult::prologue() const {
        stdc_impl_t;
        return impl.prologue;
    }

    const std::string &ParseResult::epilogue() const {
        stdc_impl_t;
        return impl.epilogue;
    }

    const HelpLayout &ParseResult::helpLayout() const {
        stdc_impl_t;
        return impl.help_layout;
    }

    // Collected by traversing the path in the same way as the parser. Because the options that
    // the parser collects during the traversal are the options it checks at the end, this
    // function is the point at which the help text must agree with the parser.
    std::vector<const Option *> ParseResult::inheritedOptions() const {
        stdc_impl_t;
        std::vector<const Option *> res;
        // Iterates over the commands above the target. The target is the last element of the
        // path.
        for (size_t i = 0; i + 1 < impl.path.size(); ++i) {
            for (const auto &option : impl.path[i]->options()) {
                if (option.isRecursive()) {
                    res.push_back(&option);
                }
            }
        }
        return res;
    }

    namespace {

        HelpSizes sizesOf(const detail::parse_data *data) {
            HelpSizes res;
            res.indent = data->indent;
            res.spacing = data->spacing;
            // Zero selects the width of stdout: the width of a terminal, or 80 columns for a pipe
            // or a file, so that help captured into a file is identical on every system.
            res.textWidth = data->text_width > 0 ? data->text_width : console::width(stdout);
            res.displayOptions = data->display_options;
            return res;
        }

        /// Returns the whole help text as runs, laid out but not yet concatenated. The sizes are
        /// computed once and used for both stages, so that a terminal resized between the stages
        /// cannot cause the usage line to be laid out at one width and the descriptions at
        /// another.
        std::vector<HelpFormatter::Run> helpRuns(const ParseResult &result,
                                                 const detail::parse_data *data) {
            if (!data->target || !data->formatter) {
                return {};
            }
            HelpSizes sizes = sizesOf(data);
            return data->formatter->render(data->formatter->blocks(result, sizes), sizes);
        }

    }

    std::vector<HelpBlock> ParseResult::helpBlocks() const {
        stdc_impl_t;
        if (!impl.target || !impl.formatter) {
            return {};
        }
        return impl.formatter->blocks(*this, sizesOf(&impl));
    }

    std::string ParseResult::helpText() const {
        stdc_impl_t;
        std::string out;
        for (const auto &run : helpRuns(*this, &impl)) {
            out += run.text;
        }
        return out;
    }

    void ParseResult::showHelp() const {
        stdc_impl_t;
        // Writes through the library's console rather than fwrite, so that one program does not
        // write to the terminal in two different ways, and so that a Windows console receives the
        // required transcoding.
        for (const auto &run : helpRuns(*this, &impl)) {
            console::fputs(run.style.style, run.style.foreground, run.style.background, run.text,
                           stdout);
        }
    }

    void ParseResult::showError() const {
        if (isValid()) {
            return;
        }
        stdc_impl_t;
        // The error text is printed in color if color is available. console determines whether
        // stderr is a destination that accepts escape sequences.
        console::fputs(console::bold, console::red, console::nocolor, impl.error_text + "\n",
                       stderr);
        if (!impl.display_options.test_flag(Parser::SkipCorrection)) {
            auto correction = correctionText();
            if (!correction.empty()) {
                console::fputs(console::nostyle, console::nocolor, console::nocolor,
                               correction + "\n", stderr);
            }
        }
        // Uses the help spelling that this program declares rather than a common convention. A
        // tree that declares no help option receives no hint line, because a reference to an
        // undeclared option is worse than no hint.
        std::string help;
        for (const auto &item : impl.options) {
            if (item.option->role() != Option::Help) {
                continue;
            }
            for (const auto &token : item.option->tokens()) {
                // Prefers a long spelling if the option declares a long spelling, because a long
                // spelling is the most readable.
                if (help.empty() || (help.rfind("--", 0) != 0 && token.rfind("--", 0) == 0)) {
                    help = token;
                }
            }
            break;
        }
        if (impl.target && !help.empty()) {
            std::string name;
            for (size_t i = 0; i < impl.path.size(); ++i) {
                name += (i ? " " : "") + impl.path[i]->name();
            }
            console::fputs(console::nostyle, console::nocolor, console::nocolor,
                           "Try \"" + name + " " + help + "\" for more information.\n", stderr);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Help
    // ---------------------------------------------------------------------------------------

    namespace {

        /// The minimum width of a description, regardless of the terminal width. A narrower column
        /// is too thin to read, which is worse than exceeding the terminal width.
        constexpr int min_description = 20;

    }

    HelpFormatter::HelpFormatter() = default;

    HelpFormatter::~HelpFormatter() = default;

    // Breaks at spaces if the text contains any, and between characters otherwise, as required by
    // languages written without spaces. Widths are measured in columns rather than in bytes or
    // characters, so that a CJK description breaks at the visually expected position.
    std::vector<std::string> HelpFormatter::wrapped(const std::string &text, int columns) {
        std::vector<std::string> lines;
        if (columns < 1) {
            lines.push_back(text);
            return lines;
        }

        auto points = utf::utf8_to_utf32(text);
        std::u32string line;
        int width = 0;
        const auto emit = [&lines](std::u32string piece) {
            while (!piece.empty() && piece.back() == U' ') {
                piece.pop_back();
            }
            lines.push_back(utf::utf32_to_utf8(piece));
        };
        const auto measure = [](const std::u32string &piece) {
            int res = 0;
            for (char32_t c : piece) {
                res += console::display_width(c);
            }
            return res;
        };

        for (char32_t c : points) {
            if (c == U'\n') {
                emit(std::move(line));
                line.clear();
                width = 0;
                continue;
            }
            int w = console::display_width(c);
            if (width + w > columns && !line.empty()) {
                // Backs up to the last space, so that a word is not split. A word longer than the
                // whole column contains no space to back up to and is broken at the column edge.
                auto space = line.find_last_of(U' ');
                if (space == std::u32string::npos) {
                    emit(line);
                    line.clear();
                } else {
                    auto tail = line.substr(space + 1);
                    emit(line.substr(0, space));
                    line = tail;
                }
                width = measure(line);
            }
            line.push_back(c);
            width += w;
        }
        emit(std::move(line));
        return lines;
    }

    std::string HelpFormatter::displayed(const Argument &argument) const {
        std::string res = "<" + argument.displayName() + ">";
        if (argument.arity() != Argument::Single) {
            res += "...";
        }
        return argument.isRequired() ? res : "[" + res + "]";
    }

    std::string HelpFormatter::displayed(const Option &option, bool allSpellings) const {
        std::string res;
        if (allSpellings) {
            for (size_t i = 0; i < option.tokens().size(); ++i) {
                res += (i ? ", " : "") + option.tokens()[i];
            }
        } else {
            res = option.token();
        }
        // Calls the virtual displayed() rather than the base implementation above, so that a
        // formatter overriding only the argument level affects every metavar.
        for (const auto &argument : option.arguments()) {
            res += " " + displayed(argument);
        }
        return res;
    }

    namespace {

        /// Returns whether \a option can be printed and typed.
        ///
        /// An option without a spelling has no text to display and no key to look up, and
        /// token() calls front() on an empty vector. Command::addOption() asserts on such an
        /// option. Therefore, this function returns false only in a release build, in which the
        /// assertion is removed and the option remains in the tree. Every place that reads the
        /// name of an option calls this function, so that these places cannot disagree about
        /// which options qualify.
        inline bool spelled(const Option &option) {
            return !option.tokens().empty();
        }

        /// Returns an empty block with the role and styles of \a slot and the title \a title.
        HelpBlock blockLike(const HelpBlock &slot, std::string title) {
            HelpBlock res;
            res.role = slot.role;
            res.title = std::move(title);
            res.titleStyle = slot.titleStyle;
            res.bodyStyle = slot.bodyStyle;
            res.entryStyle = slot.entryStyle;
            return res;
        }

        /// Distributes \a items into the groups specified by \a groups, in the specified order,
        /// and places the items that no group lists under \a fallback at the end. Returns one
        /// block per group, each with the styles of \a slot.
        template <class T, class Name, class Line>
        std::vector<HelpBlock>
            grouped(const std::vector<T> &items, const std::vector<CommandCatalogue::Group> &groups,
                    const HelpBlock &slot, const std::string &fallback, Name name, Line line) {
            std::vector<HelpBlock> res;
            std::vector<bool> taken(items.size(), false);

            for (const auto &group : groups) {
                HelpBlock block = blockLike(slot, group.name);
                for (const auto &wanted : group.members) {
                    for (size_t i = 0; i < items.size(); ++i) {
                        if (!taken[i] && name(items[i]) == wanted) {
                            block.entries.push_back(line(items[i]));
                            taken[i] = true;
                            break;
                        }
                    }
                }
                if (!block.entries.empty()) {
                    res.push_back(std::move(block));
                }
            }

            HelpBlock rest = blockLike(slot, fallback);
            for (size_t i = 0; i < items.size(); ++i) {
                if (!taken[i]) {
                    rest.entries.push_back(line(items[i]));
                }
            }
            if (!rest.entries.empty()) {
                res.push_back(std::move(rest));
            }
            return res;
        }

        /// Appends \a text to \a out, merging it into the last run if the styles are equal, so
        /// that showHelp() writes a plain help text in a single write rather than in one write
        /// per column.
        void appendRun(std::vector<HelpFormatter::Run> &out, const TextStyle &style,
                       const std::string &text) {
            if (text.empty()) {
                return;
            }
            if (!out.empty() && out.back().style == style) {
                out.back().text += text;
                return;
            }
            out.push_back({style, text});
        }

        void appendRuns(std::vector<HelpFormatter::Run> &out,
                        const std::vector<HelpFormatter::Run> &more) {
            for (const auto &run : more) {
                appendRun(out, run.style, run.text);
            }
        }

        // Returns the usage text broken into as many lines as the width of a section body
        // requires, with the line breaks written into the text.
        std::string usage_text(const HelpFormatter &formatter, const Command &command,
                               const std::vector<const Command *> &path,
                               const std::vector<Option> &inherited, int indent, int text_width) {
            std::string head;
            for (size_t i = 0; i < path.size(); ++i) {
                head += (i ? " " : "") + path[i]->name();
            }

            // Because a required option is not optional information, it is spelled out where a
            // reader looks first rather than hidden inside "[options]". The "[options]" hint
            // covers the other options and is omitted if there are no other options.
            // Because a subcommand name either comes first or does not appear, "[commands]" is
            // written first. An option before a subcommand name ends the command path. Therefore,
            // the reverse order is the only arrangement that the parser rejects.
            std::vector<std::string> parts;
            if (!command.commands().empty()) {
                parts.push_back("[commands]");
            }
            size_t optional_count = 0;
            const auto &take = [&](const Option &option) {
                if (!spelled(option)) {
                    return;
                }
                if (option.isRequired()) {
                    parts.push_back(formatter.displayed(option, false));
                } else {
                    optional_count++;
                }
            };
            for (const auto &option : command.options()) {
                take(option);
            }
            for (const auto &option : inherited) {
                take(option);
            }
            if (optional_count > 0) {
                parts.push_back("[options]");
            }
            for (const auto &argument : command.arguments()) {
                parts.push_back(formatter.displayed(argument));
            }

            int room = std::max(text_width - indent, min_description);
            std::string res = head;
            int line_width = console::display_width(head);
            for (const auto &part : parts) {
                int part_width = console::display_width(part);
                if (line_width > 0 && line_width + 1 + part_width > room) {
                    res += "\n";
                    line_width = 0;
                }
                // At the margin, a part is placed directly below the part above it. Elsewhere a
                // space separates the part from the previous part.
                res += line_width == 0 ? part : " " + part;
                line_width += line_width == 0 ? part_width : 1 + part_width;
            }
            return res;
        }

    }

    std::string HelpFormatter::displayed(const Command &command) const {
        return command.name();
    }

    // Returns the text that an argument adds to the right-hand column after its description. The
    // text is the same for an argument of a command and an argument of an option, because a
    // default value is equally useful in both places.
    static std::string argument_extras(const Argument &argument, const HelpSizes &sizes) {
        auto flags = sizes.displayOptions;
        std::string res;
        if (flags.test_flag(Parser::ShowArgumentExpectedValues) &&
            !argument.expectedValues().empty()) {
            std::string words;
            for (const auto &item : argument.expectedValues()) {
                words += (words.empty() ? "" : ", ") + item;
            }
            res += " [" + words + "]";
        }
        if (flags.test_flag(Parser::ShowArgumentDefaultValue) && argument.hasDefaultValue()) {
            res += " (default: " + argument.defaultValue() + ")";
        }
        return res;
    }

    HelpBlock::Entry HelpFormatter::entry(const Argument &argument, const HelpSizes &sizes) const {
        return {displayed(argument), argument.description() + argument_extras(argument, sizes)};
    }

    HelpBlock::Entry HelpFormatter::entry(const Option &option, const HelpSizes &sizes) const {
        std::string right = option.description();
        for (const auto &argument : option.arguments()) {
            right += argument_extras(argument, sizes);
        }
        if (sizes.displayOptions.test_flag(Parser::ShowOptionIsRequired) && option.isRequired()) {
            right += " (required)";
        }
        return {displayed(option, true), right};
    }

    HelpBlock::Entry HelpFormatter::entry(const Command &command, const HelpSizes &) const {
        return {displayed(command), command.description()};
    }

    std::string HelpFormatter::usageText(const Command &command,
                                         const std::vector<const Command *> &path,
                                         const std::vector<Option> &inherited,
                                         const HelpSizes &sizes) const {
        return usage_text(*this, command, path, inherited, sizes.indent, sizes.textWidth);
    }

    std::vector<HelpBlock> HelpFormatter::blocks(const ParseResult &result,
                                                 const HelpSizes &sizes) const {
        if (!result.command()) {
            return {};
        }
        const Command &command = *result.command();
        const auto &catalogue = command.catalogue();
        auto flags = sizes.displayOptions;

        // Each row is built through the level that produces a row, so that a formatter changing
        // the content of a row does not have to override this whole function.
        auto argument_line = [this, &sizes](const Argument &item) { return entry(item, sizes); };
        auto option_line = [this, &sizes](const Option &item) { return entry(item, sizes); };
        auto command_line = [this, &sizes](const Command &item) { return entry(item, sizes); };

        // Lists only the options that can be printed, as defined and explained at spelled().
        // The help text is not the place to reveal that a tree contains an option that cannot
        // be typed.
        std::vector<Option> own;
        for (const auto &item : command.options()) {
            if (spelled(item)) {
                own.push_back(item);
            }
        }
        // The options that the commands above declared recursive are in scope here and are
        // checked here. Therefore, they are listed here. They are listed under a separate
        // heading, because they belong to the program rather than to this command, and because
        // the groups of the catalogue were written for this command's own options and do not
        // cover these options.
        std::vector<Option> inherited_options;
        for (const auto *option : result.inheritedOptions()) {
            if (spelled(*option)) {
                inherited_options.push_back(*option);
            }
        }

        std::vector<HelpBlock> out;
        // An empty block is neither printed nor returned, so that the help of a command without
        // subcommands omits the subcommand section rather than showing an empty heading.
        const auto push = [&out](HelpBlock block) {
            if (!block.isEmpty()) {
                out.push_back(std::move(block));
            }
        };
        const auto pushAll = [&out](std::vector<HelpBlock> blocks) {
            for (auto &block : blocks) {
                out.push_back(std::move(block));
            }
        };

        for (const auto &slot : result.helpLayout().blocks()) {
            switch (slot.role) {
                case HelpBlock::Prologue: {
                    auto block = blockLike(slot, {});
                    block.text = result.prologue();
                    push(std::move(block));
                    break;
                }
                case HelpBlock::Description: {
                    auto block = blockLike(slot, "Description");
                    block.text = command.description();
                    push(std::move(block));
                    break;
                }
                case HelpBlock::Usage: {
                    auto block = blockLike(slot, "Usage");
                    block.text = usageText(command, result.commandPath(), inherited_options, sizes);
                    push(std::move(block));
                    break;
                }
                case HelpBlock::Arguments: {
                    pushAll(grouped(
                        command.arguments(), catalogue.argumentGroups(), slot, "Arguments",
                        [](const Argument &item) { return item.name(); }, argument_line));
                    break;
                }
                // ###QUESTION: A recursive option is listed here on the page of the declaring
                // command and under "Global options" on the pages below it. The same spelling
                // therefore appears under two headings depending on the page. Cobra handles a
                // persistent flag in the same way. The two alternatives are listing the option
                // under "Global options" everywhere, which splits the list of the declaring
                // command by a distinction that a reader of that page cannot act on, and
                // omitting the second block entirely, which is the System.CommandLine behavior
                // and loses the only useful statement on the page of a subcommand: the option
                // belongs to the program and is documented in the help of the program. It is
                // undecided whether both pages should use the same heading. That change would
                // require four lines here, which filter the recursive options out of own and add
                // them to the block below.
                case HelpBlock::Options: {
                    pushAll(grouped(
                        own, catalogue.optionGroups(), slot, "Options",
                        [](const Option &item) { return item.token(); }, option_line));
                    break;
                }
                case HelpBlock::InheritedOptions: {
                    auto block = blockLike(slot, "Global options");
                    for (const auto &option : inherited_options) {
                        block.entries.push_back(option_line(option));
                    }
                    push(std::move(block));
                    break;
                }
                case HelpBlock::Commands: {
                    pushAll(grouped(
                        command.commands(), catalogue.commandGroups(), slot, "Commands",
                        [](const Command &item) { return item.name(); }, command_line));
                    break;
                }
                case HelpBlock::Epilogue: {
                    auto block = blockLike(slot, {});
                    block.text = result.epilogue();
                    push(std::move(block));
                    break;
                }
                case HelpBlock::Custom: {
                    push(slot);
                    break;
                }
            }
        }
        return out;
    }

    std::vector<HelpFormatter::Run> HelpFormatter::renderBlock(const HelpBlock &block,
                                                               const HelpSizes &sizes,
                                                               size_t widest) const {
        std::vector<Run> out;
        if (!block.title.empty()) {
            // The newline is outside the styled run, so that the reset escape sequence is the
            // last item on the title line rather than the first item on the next line. A
            // background color painted to the end of the line makes the difference visible.
            appendRun(out, block.titleStyle, block.title + ":");
            appendRun(out, {}, "\n");
        }

        if (!block.entries.empty()) {
            // The column at which a description starts, which is therefore also the indentation
            // of its continuation lines, so that a wrapped entry remains one block instead of
            // drifting left.
            size_t column = size_t(sizes.indent) + widest + size_t(sizes.spacing);
            int room = std::max(sizes.textWidth - int(column), min_description);
            for (const auto &entry : block.entries) {
                appendRun(out, {}, std::string(size_t(sizes.indent), ' '));
                appendRun(out, block.entryStyle, entry.left);
                if (!entry.right.empty()) {
                    auto lines = wrapped(entry.right, room);
                    size_t padding =
                        widest - size_t(console::display_width(entry.left)) + size_t(sizes.spacing);
                    appendRun(out, {}, std::string(padding, ' '));
                    appendRun(out, block.bodyStyle, lines.front());
                    for (size_t i = 1; i < lines.size(); ++i) {
                        appendRun(out, {}, "\n" + std::string(column, ' '));
                        appendRun(out, block.bodyStyle, lines[i]);
                    }
                }
                appendRun(out, {}, "\n");
            }
            return out;
        }

        // Prose under a heading is indented under the heading. Prose without a heading starts at
        // the margin, as required for a prologue and an epilogue.
        size_t margin = block.title.empty() ? 0 : size_t(sizes.indent);
        int room = std::max(sizes.textWidth - int(margin), min_description);
        for (const auto &line : wrapped(block.text, room)) {
            appendRun(out, {}, std::string(margin, ' '));
            appendRun(out, block.bodyStyle, line);
            appendRun(out, {}, "\n");
        }
        return out;
    }

    std::vector<HelpFormatter::Run> HelpFormatter::render(const std::vector<HelpBlock> &blocks,
                                                          const HelpSizes &sizes) const {
        // Measured across every list rather than across each list separately, so that a
        // catalogue reads as one table instead of several.
        bool align_all = sizes.displayOptions.test_flag(Parser::AlignAllCatalogues);
        size_t shared = align_all ? widestOf(blocks) : 0;

        std::vector<Run> out;
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (i > 0) {
                appendRun(out, {}, "\n");
            }
            appendRuns(out,
                       renderBlock(blocks[i], sizes, align_all ? shared : widestOf(blocks[i])));
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------
    // Parsing
    //
    // The command path is a prefix. The run of subcommand names at the front of the line forms
    // the whole path, and the first token that is not a subcommand name ends it and fixes the
    // target. Every later token belongs to the target: its own options, the options its ancestors
    // declared recursive, and its arguments.
    //
    // There is therefore exactly one scope, and it is known before any option is read. An option
    // written between two command names is not read against the command that the traversal had
    // reached at that point. This is intentional: the queries that a ParseResult supports
    // correspond exactly to the content that a command line can express.
    //
    // Two further rules reject command lines that a lenient parser accepts without a report.
    //
    // Positional tokens that the target cannot accept are an error. If such tokens were
    // discarded, a mistyped subcommand would succeed without a report.
    //
    // An option that requires a value does not accept a token that is a declared option of the
    // same command. Reporting the error is preferable to consuming --force and leaving the user
    // to find where it went. Only a declared option counts. Therefore, a negative number or an
    // undeclared name is still a value.
    // ---------------------------------------------------------------------------------------

    namespace {

        /// The state of one call to parse, held in one object so that the steps share it without
        /// passing a dozen parameters.
        class ParserCore {
        public:
            ParserCore(detail::parse_data *out, Parser::ParseOptions flags) : r(out), flags(flags) {
            }

            void run(array_view<std::string> args);

        private:
            using Error = ParseResult::Error;

            detail::parse_data *r;
            Parser::ParseOptions flags;
            std::vector<std::string> tokens;
            size_t pos = 0;
            /// The positional tokens, collected first and assigned to the arguments afterwards,
            /// because the number of tokens each argument consumes depends on the total number.
            std::vector<std::string> positional;
            /// The given option with the highest priority, which determines whether the final
            /// checks are performed.
            const Option *prior_option = nullptr;
            /// The options that the commands on the traversed path declared recursive, which are
            /// in scope below the declaring command.
            std::vector<const Option *> inherited;
            /// The index of the Remainder argument among the arguments of the target, if the
            /// target declares a Remainder argument. Once that many positional tokens have been
            /// collected, the rest of the line belongs to the Remainder argument, including
            /// options. A program uses this to specify where option reading stops.
            size_t remainder_at = size_t(-1);
            /// Whether the Remainder argument has received its first token. This matters only if
            /// the Remainder argument is the first argument and the options in front of it are
            /// still being read.
            bool remainder_started = false;

            bool on(Parser::ParseOption flag) const {
                return flags.test_flag(flag);
            }
            bool failed() const {
                return r->error != ParseResult::NoError;
            }
            /// Returns the options that can be written at the target: its own options first, then
            /// the inherited options. r->options contains exactly these options, because the path
            /// was settled before any option was collected. Therefore, the writable options and
            /// the readable options form the same list.
            std::vector<const Option *> inScope() const {
                std::vector<const Option *> res;
                for (const auto &option : r->target->options()) {
                    res.push_back(&option);
                }
                res.insert(res.end(), inherited.begin(), inherited.end());
                return res;
            }
            /// Returns whether any option in scope was written. Used together with an empty
            /// positional list to distinguish a bare command from a command with input.
            bool anythingGiven() const {
                for (const auto &item : r->options) {
                    if (!item.occurrences.empty()) {
                        return true;
                    }
                }
                return false;
            }
            void fail(Error error, std::string text) {
                if (!failed()) {
                    r->error = error;
                    r->error_text = std::move(text);
                }
            }
            /// Records a failure like fail(), for a misspelled name, together with the declared
            /// names to suggest as corrections.
            void failFor(Error error, std::string text, std::string token,
                         std::vector<std::string> candidates) {
                if (failed()) {
                    return;
                }
                fail(error, std::move(text));
                r->error_token = std::move(token);
                r->error_candidates = std::move(candidates);
            }

            void expandResponseFiles();
            const Command *subcommandFor(const std::string &token) const;
            void enter(const Command *next);
            void collectOptions();
            void readTokens();
            bool readOption(const std::string &token);
            bool readOneOption(OptionData *data, std::string_view inline_value);
            OptionData *lookup(std::string_view token) const;
            /// Returns the option that a token names and the value written against it, for every
            /// spelling that readOption() accepts. Grouped flags are excluded because they name a
            /// list of options rather than one option. groupedFlagsFor() handles grouped flags.
            OptionMatch optionFor(const std::string &token) const;
            bool groupedFlagsFor(const std::string &token, std::vector<OptionData *> *found) const;
            /// Returns whether the token names an option, which is the condition that ends a run
            /// of values. Because this query and the reading of the token must agree, both use
            /// the two functions above. A run that stopped at --out but not at --out=x would
            /// consume --out=x as a value of the option being read.
            bool namesAnOption(const std::string &token) const;
            bool readGroupedFlags(const std::string &token);
            void assignPositional();
            void applyDefaults();
            void checkRequired();

            bool looksLikeOption(std::string_view token) const;
            bool accepts(const Argument &argument, const std::string &token,
                         const std::string &where);
            void notePrior(const Option *option);
        };

        bool ParserCore::looksLikeOption(std::string_view token) const {
            if (token.size() < 2) {
                return false;
            }
            if (token[0] == '-') {
                return token[1] == '-' || !on(Parser::DontAllowUnixShortOptions);
            }
            return token[0] == '/' && on(Parser::AllowDosShortOptions);
        }

        void ParserCore::notePrior(const Option *option) {
            if (!prior_option || option->prior() > prior_option->prior()) {
                if (option->prior() != Option::NoPrior) {
                    prior_option = option;
                }
            }
        }

        /// Returns \a text without leading and trailing whitespace. Because the carriage return of
        /// a CRLF line is whitespace, a file written on Windows requires no separate step.
        std::string_view trimmed(std::string_view text) {
            const auto space = [](char c) {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
            };
            while (!text.empty() && space(text.front())) {
                text.remove_prefix(1);
            }
            while (!text.empty() && space(text.back())) {
                text.remove_suffix(1);
            }
            return text;
        }

        // A line of a response file becomes a token only after three items are removed from it.
        // These files are written by a build system rather than by a shell, and a shell would
        // not have left any of the three items in place.
        //
        //  - blanks at either end, because a generator aligns its arguments
        //  - one pair of quotes, because a path containing a space requires quoting, and CMake
        //    quotes every path
        //  - a byte order mark, because a Windows editor writes a byte order mark at the start
        //    of the first line
        //
        // A line that consists only of a pair of quotes is an empty argument and is retained. A
        // line that is empty or contains only blanks is not an argument and is dropped.
        void ParserCore::expandResponseFiles() {
            std::vector<std::string> out;
            for (const auto &token : tokens) {
                if (token.size() < 2 || token.front() != '@') {
                    out.push_back(token);
                    continue;
                }
                // The name is UTF-8 like every other string here. Because ifstream on Windows
                // interprets a narrow string in the system code page, the name is converted with
                // path::from_utf8().
                //
                // The file is read in binary mode, so that no content is interpreted. A Windows
                // text stream treats a Ctrl-Z as the end of the file and discards the remainder.
                // The carriage returns of a CRLF file are not the reason: they are whitespace and
                // are removed by the trim below, on every platform and in either mode.
                std::ifstream file(path::from_utf8(std::string_view(token).substr(1)),
                                   std::ios::binary);
                if (!file) {
                    fail(ParseResult::ErrorReadingResponseFile,
                         "cannot read response file \"" + token.substr(1) + "\"");
                    return;
                }
                bool first = true;
                std::string line;
                while (std::getline(file, line)) {
                    std::string_view text = line;
                    if (first) {
                        first = false;
                        // Only the first line can carry a byte order mark, and only the complete
                        // three-byte sequence is removed. Two of the three bytes are not a mark.
                        if (text.substr(0, 3) == "\xEF\xBB\xBF") {
                            text.remove_prefix(3);
                        }
                    }
                    text = trimmed(text);
                    if (text.empty()) {
                        continue;
                    }
                    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                        text = text.substr(1, text.size() - 2);
                    }
                    out.emplace_back(text);
                }
            }
            tokens = std::move(out);
        }

        /// Returns the subcommand that \a token names, or null if there is none. Returns null
        /// after the first positional token, because a name in that position is a value.
        const Command *ParserCore::subcommandFor(const std::string &token) const {
            if (!positional.empty()) {
                return nullptr;
            }
            for (const auto &candidate : r->target->commands()) {
                if (same_token(candidate.name(), token, on(Parser::IgnoreCommandCase))) {
                    return &candidate;
                }
            }
            return nullptr;
        }

        /// Moves into \a next and adds the recursive options of the command being left to the
        /// inherited options.
        ///
        /// The options are not collected here. Because no option may be written before the path
        /// ends, there is one scope to collect, which is the scope of the target. Collecting
        /// at every level would leave the own options of each ancestor in the table for the rest
        /// of the parse.
        void ParserCore::enter(const Command *next) {
            for (const auto &option : r->target->options()) {
                if (option.isRecursive()) {
                    inherited.push_back(&option);
                }
            }
            r->target = next;
            r->path.push_back(next);
        }

        /// Collects the options that can be written at the target: its own options and the
        /// recursive options of every command above it. Called once, after the path is settled.
        /// Therefore, this is the only scope, and the options that a result can be queried for
        /// are exactly the options that could be written.
        void ParserCore::collectOptions() {
            r->options.clear();
            r->by_token.clear();
            auto add = [this](const Option *option) {
                size_t index = r->options.size();
                for (size_t i = 0; i < r->options.size(); ++i) {
                    if (r->options[i].option == option) {
                        index = i;
                        break;
                    }
                }
                if (index == r->options.size()) {
                    r->options.push_back({option, {}});
                }
                for (const auto &spelling : option->tokens()) {
                    r->by_token[spelling] = index;
                }
            };
            for (const auto &option : r->target->options()) {
                add(&option);
            }
            for (auto option : inherited) {
                add(option);
            }
        }

        OptionData *ParserCore::lookup(std::string_view token) const {
            if (!on(Parser::IgnoreOptionCase)) {
                return r->findForWriting(token);
            }
            for (auto &item : r->options) {
                for (const auto &spelling : item.option->tokens()) {
                    if (str::equals_insensitive(spelling, token)) {
                        return &item;
                    }
                }
            }
            return nullptr;
        }

        bool ParserCore::accepts(const Argument &argument, const std::string &token,
                                 const std::string &where) {
            const auto &expected = argument.expectedValues();
            if (!expected.empty() &&
                std::find(expected.begin(), expected.end(), token) == expected.end()) {
                failFor(ParseResult::InvalidArgumentValue,
                        "\"" + token + "\" is not one of the values " + where + " accepts", token,
                        expected);
                return false;
            }
            if (argument.typeInfo().check && !argument.typeInfo().check(token)) {
                fail(ParseResult::ArgumentTypeMismatch, "\"" + token + "\" is not a " +
                                                            argument.typeInfo().name +
                                                            ", which is what " + where + " takes");
                return false;
            }
            if (argument.validator()) {
                std::string reason;
                if (!argument.validator()(token, &reason)) {
                    fail(ParseResult::ArgumentValidateFailed,
                         reason.empty() ? "\"" + token + "\" is not acceptable to " + where
                                        : reason);
                    return false;
                }
            }
            return true;
        }

        /// Reads the arguments of one option occurrence, starting at \c pos. \a inline_value is
        /// the text after an equals sign or joined to a short token, and supplies the first
        /// value of the first argument if present.
        bool ParserCore::readOneOption(OptionData *data, std::string_view inline_value) {
            const Option *option = data->option;

            int limit = option->maxOccurrence();
            if (limit > 0 && int(data->occurrences.size()) >= limit) {
                fail(ParseResult::OptionOccurTooMuch,
                     "option \"" + option->token() + "\" was given more than " +
                         std::to_string(limit) + (limit == 1 ? " time" : " times"));
                return false;
            }

            ArgumentSlots slots(option->arguments().size());
            bool have_inline = inline_value.data() != nullptr;

            for (size_t i = 0; i < option->arguments().size(); ++i) {
                const auto &argument = option->arguments()[i];
                const std::string where = "\"" + option->token() + "\"";

                // The text after the equals sign or joined to the spelling is the first value of
                // the first argument, not the whole argument. A Single argument is complete with
                // this value, and an argument that accepts more values continues reading after
                // the token, so that --opt=a b and --opt a b are equivalent.
                if (i == 0 && have_inline) {
                    std::string token(inline_value);
                    if (!accepts(argument, token, where)) {
                        return false;
                    }
                    slots[i].push_back(std::move(token));
                    if (argument.arity() == Argument::Single) {
                        continue;
                    }
                }

                // Counts the available tokens, up to the next token that names a declared option,
                // and then the number of those tokens that this argument consumes.
                // A token that names a declared option is never silently consumed as a value, not
                // even by a required argument: reporting "-o needs a value" is preferable to
                // consuming --force and leaving the user to find where it went. Only a declared
                // option counts. Therefore, a negative number is an ordinary value rather than an
                // unknown option.
                //
                // A Remainder argument is the exception, and this exception is its entire
                // meaning: it consumes the rest of the line unchanged. A program uses a Remainder
                // argument to specify where option reading stops, and the token at which reading
                // stops is the choice of the program.
                bool remainder = argument.arity() == Argument::Remainder;
                const auto &ends_the_run = [this](const std::string &token) {
                    return namesAnOption(token);
                };

                size_t available = tokens.size() - pos;
                if (!remainder) {
                    available = 0;
                    while (pos + available < tokens.size() &&
                           !ends_the_run(tokens[pos + available])) {
                        ++available;
                    }
                }
                size_t take = take_for(option->arguments(), i, available, !slots[i].empty());

                bool took_any = !slots[i].empty();
                for (size_t count = 0; count < take && pos < tokens.size(); ++count) {
                    const auto &token = tokens[pos];
                    if (!remainder && ends_the_run(token)) {
                        break;
                    }
                    if (!accepts(argument, token, where)) {
                        return false;
                    }
                    slots[i].push_back(token);
                    ++pos;
                    took_any = true;
                }

                if (!took_any && argument.isRequired() &&
                    option->prior() < Option::IgnoreMissingArguments) {
                    fail(ParseResult::MissingOptionArgument, "option \"" + option->token() +
                                                                 "\" needs a value for <" +
                                                                 argument.displayName() + ">");
                    return false;
                }
            }

            data->occurrences.push_back(std::move(slots));
            notePrior(option);
            return true;
        }

        OptionMatch ParserCore::optionFor(const std::string &token) const {
            if (token.empty()) {
                return {};
            }
            // The whole token is a spelling, which is the ordinary case.
            if (auto data = lookup(token)) {
                return {data, {}, token};
            }

            // The token contains a value joined by an equals sign. The value is empty rather than
            // absent if nothing follows the sign, because --prefix= sets an empty string.
            auto equals = token.find('=');
            if (equals != std::string::npos) {
                if (auto data = lookup(token.substr(0, equals))) {
                    return {data, std::string_view(token).substr(equals + 1),
                            std::string_view(token).substr(0, equals)};
                }
            }

            // The token is a short option with its value joined to it. The spellings that qualify
            // are returned by detail::sticky_spellings(), which the whole-tree check also uses,
            // so that a configuration rejected as ambiguous is rejected for the spellings that
            // would actually be tried. The prefix is compared in the same way as a whole token,
            // because otherwise IgnoreOptionCase would apply to -d foo but not to -dfoo.
            for (auto &item : r->options) {
                for (auto spelling : detail::sticky_spellings(*item.option)) {
                    if (spelling.size() >= token.size()) {
                        continue;
                    }
                    auto head = std::string_view(token).substr(0, spelling.size());
                    if (!detail::same_name(spelling, head, on(Parser::IgnoreOptionCase))) {
                        continue;
                    }
                    return {&item, std::string_view(token).substr(spelling.size()), head};
                }
            }

            // The token is a DOS short option, looked up under its Unix spelling.
            if (token[0] == '/' && on(Parser::AllowDosShortOptions)) {
                if (auto data = lookup("-" + token.substr(1))) {
                    return {data, {}, token};
                }
            }
            return {};
        }

        bool ParserCore::groupedFlagsFor(const std::string &token,
                                         std::vector<OptionData *> *found) const {
            if (!on(Parser::AllowUnixGroupFlags) || token.size() < 3 || token[0] != '-' ||
                token[1] == '-') {
                return false;
            }
            // Every letter must be a separate option that accepts no value. Otherwise the token
            // is not a group of flags and is not handled here.
            for (size_t i = 1; i < token.size(); ++i) {
                auto data = lookup(std::string("-") + token[i]);
                if (!data || !data->option->arguments().empty()) {
                    return false;
                }
                found->push_back(data);
            }
            return true;
        }

        bool ParserCore::namesAnOption(const std::string &token) const {
            if (!looksLikeOption(token)) {
                return false;
            }
            if (optionFor(token)) {
                return true;
            }
            std::vector<OptionData *> flags;
            return groupedFlagsFor(token, &flags);
        }

        bool ParserCore::readGroupedFlags(const std::string &token) {
            std::vector<OptionData *> found;
            if (!groupedFlagsFor(token, &found)) {
                return false;
            }
            for (auto data : found) {
                if (!readOneOption(data, {})) {
                    return false;
                }
            }
            return true;
        }

        bool ParserCore::readOption(const std::string &token) {
            if (auto match = optionFor(token)) {
                if (match.value.data() && match.data->option->arguments().empty()) {
                    fail(ParseResult::UnknownOption,
                         "option \"" + std::string(match.spelling) + "\" takes no value");
                    return false;
                }
                return readOneOption(match.data, match.value);
            }

            if (readGroupedFlags(token)) {
                return true;
            }

            std::vector<std::string> declared;
            for (const auto &item : r->options) {
                for (const auto &spelling : item.option->tokens()) {
                    declared.push_back(spelling);
                }
            }
            failFor(ParseResult::UnknownOption, "unknown option \"" + token + "\"", token,
                    std::move(declared));
            return false;
        }

        void ParserCore::readTokens() {
            // Reads the path first and nothing else. A command line names its target by naming
            // each command down to the target with nothing in between. Therefore, the run of
            // subcommand names at the front forms the whole path, and the first token that is not
            // a subcommand name ends the path.
            //
            // This makes the scope that of the target rather than that of the command at the
            // current read position. An option belongs to one command and is written after that
            // command, and recursive() is the only way for the option to reach a command below.
            while (pos < tokens.size()) {
                const auto &token = tokens[pos];
                if (looksLikeOption(token)) {
                    break;
                }
                auto next = subcommandFor(token);
                if (!next) {
                    break;
                }
                ++pos;
                enter(next);
            }
            collectOptions();

            // Locates the Remainder argument of the target, if the target declares a Remainder
            // argument. Every argument before it consumes one token, because no greedy argument may
            // precede a Remainder argument. Therefore, the number of positional tokens collected
            // indicates when the Remainder argument has started.
            const auto &declared = r->target->arguments();
            for (size_t i = 0; i < declared.size(); ++i) {
                if (declared[i].arity() == Argument::Remainder) {
                    remainder_at = i;
                    break;
                }
            }

            while (pos < tokens.size() && !failed()) {
                const auto &token = tokens[pos];
                // Once the Remainder argument has started, no token is an option. This is the
                // meaning of the arity, and it allows a program to choose the token at which
                // option reading stops, rather than every program stopping at a single token
                // chosen by the parser.
                if (remainder_at != size_t(-1) && positional.size() >= remainder_at) {
                    // Filling the argument before the Remainder argument ends option reading. If
                    // the Remainder argument is the first argument, no such preceding argument
                    // exists. Therefore, option reading ends at the first token that is not
                    // written as an option. This matches sudo: sudo -u root ls -l reads -u root
                    // and runs ls -l. A token that looks like an option but is not a declared
                    // option is still reported, because silently passing on misspelled flags of
                    // the wrapper itself does not improve the wrapper.
                    if (remainder_at == 0 && !remainder_started && looksLikeOption(token)) {
                        ++pos;
                        readOption(token);
                        continue;
                    }
                    remainder_started = true;
                    positional.push_back(token);
                    ++pos;
                    continue;
                }
                if (looksLikeOption(token)) {
                    ++pos;
                    readOption(token);
                    continue;
                }
                // Because the path has ended, a subcommand name at this position was written too
                // late. The error reports exactly that rather than a stray value, because
                // "unknown argument" for a word that the user can see is a subcommand is the
                // least useful report.
                //
                // This applies only if the target accepts no arguments of its own. Otherwise this
                // token is a value of those arguments: a program that has a subcommand named
                // \c copy can still receive a file named copy, and treating the token otherwise
                // would make the name unusable as a value.
                if (r->target->arguments().empty() && subcommandFor(token)) {
                    failFor(ParseResult::UnknownCommand,
                            "command \"" + token + "\" has to come before the options of \"" +
                                r->target->name() + "\"",
                            token, {});
                    return;
                }
                positional.push_back(token);
                ++pos;
            }
        }

        void ParserCore::assignPositional() {
            const auto &declared = r->target->arguments();
            r->arguments.resize(declared.size());

            size_t taken = 0;
            for (size_t i = 0; i < declared.size() && taken < positional.size(); ++i) {
                const auto &argument = declared[i];
                size_t take = std::min(take_for(declared, i, positional.size() - taken),
                                       positional.size() - taken);

                const std::string where = "<" + argument.displayName() + ">";
                for (size_t k = 0; k < take; ++k) {
                    const auto &token = positional[taken + k];
                    if (!accepts(argument, token, where)) {
                        return;
                    }
                    r->arguments[i].push_back(token);
                }
                taken += take;
            }

            if (taken < positional.size()) {
                // If no token was assigned and the target has subcommands, the first token
                // occupied the position of a subcommand. Reporting an unknown command is more
                // useful than an argument count error for a user who mistyped a name.
                if (taken == 0 && !r->target->commands().empty()) {
                    std::vector<std::string> declared;
                    for (const auto &command : r->target->commands()) {
                        declared.push_back(command.name());
                    }
                    failFor(ParseResult::UnknownCommand,
                            "\"" + positional[0] + "\" is not a command of \"" + r->target->name() +
                                "\"",
                            positional[0], std::move(declared));
                    return;
                }
                fail(ParseResult::TooManyArguments, "\"" + positional[taken] +
                                                        "\" is one argument more than \"" +
                                                        r->target->name() + "\" takes");
            }
        }

        void ParserCore::applyDefaults() {
            const auto &declared = r->target->arguments();
            for (size_t i = 0; i < declared.size() && i < r->arguments.size(); ++i) {
                if (r->arguments[i].empty() && declared[i].hasDefaultValue()) {
                    r->arguments[i].push_back(declared[i].defaultValue());
                }
            }
            for (auto &item : r->options) {
                for (auto &occurrence : item.occurrences) {
                    for (size_t i = 0; i < occurrence.size(); ++i) {
                        if (occurrence[i].empty() &&
                            item.option->arguments()[i].hasDefaultValue()) {
                            occurrence[i].push_back(item.option->arguments()[i].defaultValue());
                        }
                    }
                }
            }
        }

        void ParserCore::checkRequired() {
            // Because an option with a sufficiently high priority level satisfies the command line
            // by itself, missing items elsewhere are not reported.
            auto level = prior_option ? prior_option->prior() : Option::NoPrior;

            // Each of the three exclusive levels forbids its own category rather than everything
            // below it. Only the lower levels are cumulative.
            bool forbids_arguments =
                level == Option::ExclusiveToArguments || level == Option::ExclusiveToAll;
            bool forbids_options =
                level == Option::ExclusiveToOptions || level == Option::ExclusiveToAll;

            // Checks the given tokens rather than the stored values. r->arguments already
            // contains the applied defaults, and a default substitutes for an omitted value.
            // Therefore, checking r->arguments here would make an option that forbids arguments
            // reject a line that contains no arguments.
            if (forbids_arguments && !positional.empty()) {
                fail(ParseResult::PriorOptionWithArguments,
                     "option \"" + prior_option->token() + "\" takes no arguments beside it");
                return;
            }
            if (forbids_options) {
                for (const auto &item : r->options) {
                    if (item.option != prior_option && !item.occurrences.empty()) {
                        fail(ParseResult::PriorOptionWithOptions,
                             "option \"" + prior_option->token() + "\" takes no options beside it");
                        return;
                    }
                }
            }
            if (level >= Option::IgnoreMissingSymbols) {
                return;
            }

            const auto &declared = r->target->arguments();
            for (size_t i = 0; i < declared.size(); ++i) {
                if (declared[i].isRequired() && r->arguments[i].empty()) {
                    fail(ParseResult::MissingCommandArgument, "\"" + r->target->name() +
                                                                  "\" needs a value for <" +
                                                                  declared[i].displayName() + ">");
                    return;
                }
            }
            for (const auto &item : r->options) {
                if (item.option->isRequired() && item.occurrences.empty()) {
                    fail(ParseResult::MissingRequiredOption,
                         "option \"" + item.option->token() + "\" is required");
                    return;
                }
            }
        }

        void ParserCore::run(array_view<std::string> args) {
            // The first argument is the program name and is not parsed.
            tokens.assign(args.begin() + (args.empty() ? 0 : 1), args.end());
            if (on(Parser::EnableResponseFile)) {
                expandResponseFiles();
                if (failed()) {
                    return;
                }
            }

            r->path.push_back(r->target);
            readTokens();
            if (failed()) {
                return;
            }

            // Applies an option that substitutes for an empty command line, which makes a bare
            // command print its help rather than report an error.
            //
            // The condition is whether the target was given any input, not the number of
            // tokens. Because a subcommand name is a token, counting tokens would treat
            // "prog build" as a nonempty line, and the AutoSetWhenNoSymbols option of a
            // subcommand would never apply.
            //
            // The option is searched for among the options in scope at the target rather than
            // among every option collected during the traversal, because an option of a command
            // already left cannot be written here and must not substitute for a line written
            // here. Because the own options of the target precede the inherited options, the
            // innermost option takes precedence.
            if (!prior_option && positional.empty() && !anythingGiven()) {
                for (const auto *option : inScope()) {
                    if (option->prior() != Option::AutoSetWhenNoSymbols ||
                        option->tokens().empty()) {
                        continue;
                    }
                    auto data = r->findForWriting(option->token());
                    if (!data) {
                        continue;
                    }
                    data->occurrences.emplace_back(option->arguments().size());
                    prior_option = option;
                    break;
                }
            }

            assignPositional();
            if (failed()) {
                return;
            }
            applyDefaults();
            checkRequired();
        }

    }

    // ---------------------------------------------------------------------------------------
    // Parser
    // ---------------------------------------------------------------------------------------

    namespace {

        /// Returns the initial formatter of a parser. One instance is shared rather than one per
        /// parser, because a plain formatter holds no state between calls and all its methods
        /// are const.
        const std::shared_ptr<HelpFormatter> &defaultFormatter() {
            static const std::shared_ptr<HelpFormatter> instance =
                std::make_shared<HelpFormatter>();
            return instance;
        }

    }

    class Parser::Impl {
    public:
        std::shared_ptr<Command> root = std::make_shared<Command>();
        std::string prologue;
        std::string epilogue;
        HelpLayout help_layout = HelpLayout::defaultLayout();
        std::shared_ptr<HelpFormatter> formatter = defaultFormatter();
        Parser::DisplayOptions display_options;
        int text_width = 0;
        int indent = 4;
        int spacing = 4;
    };

    Parser::Parser() : _impl(std::make_unique<Impl>()) {
    }

    Parser::Parser(Command root) : _impl(std::make_unique<Impl>()) {
        stdc_impl_t;
        impl.root = std::make_shared<Command>(std::move(root));
    }

    Parser::~Parser() = default;

    Parser::Parser(Parser &&RHS) noexcept = default;

    Parser &Parser::operator=(Parser &&RHS) noexcept = default;

    void Parser::setRootCommand(Command root) {
        stdc_impl_t;

        // Creates a new tree rather than assigning new contents to the old tree. Every
        // ParseResult already returned shares this pointer and holds raw pointers into the tree.
        // Therefore, assigning through the pointer would leave every such result reading freed
        // vectors. Assigning through the pointer was verified to fail under ASAN in
        // test_a_parser_is_reusable_and_its_tree_can_be_replaced.
        impl.root = std::make_shared<Command>(std::move(root));
    }

    const Command &Parser::rootCommand() const {
        stdc_impl_t;
        return *impl.root;
    }

    void Parser::setPrologue(std::string text) {
        stdc_impl_t;
        impl.prologue = std::move(text);
    }

    const std::string &Parser::prologue() const {
        stdc_impl_t;
        return impl.prologue;
    }

    void Parser::setEpilogue(std::string text) {
        stdc_impl_t;
        impl.epilogue = std::move(text);
    }

    const std::string &Parser::epilogue() const {
        stdc_impl_t;
        return impl.epilogue;
    }

    void Parser::setDisplayOptions(DisplayOptions options) {
        stdc_impl_t;
        impl.display_options = options;
    }

    Parser::DisplayOptions Parser::displayOptions() const {
        stdc_impl_t;
        return impl.display_options;
    }

    void Parser::setTextWidth(int width) {
        stdc_impl_t;
        impl.text_width = width;
    }

    int Parser::textWidth() const {
        stdc_impl_t;
        return impl.text_width;
    }

    void Parser::setIndent(int columns) {
        stdc_impl_t;
        impl.indent = columns;
    }

    int Parser::indent() const {
        stdc_impl_t;
        return impl.indent;
    }

    void Parser::setSpacing(int columns) {
        stdc_impl_t;
        impl.spacing = columns;
    }

    int Parser::spacing() const {
        stdc_impl_t;
        return impl.spacing;
    }

    void Parser::setHelpLayout(HelpLayout layout) {
        stdc_impl_t;
        impl.help_layout = std::move(layout);
    }

    const HelpLayout &Parser::helpLayout() const {
        stdc_impl_t;
        return impl.help_layout;
    }

    // Null restores the default formatter rather than leaving the parser without a formatter for
    // its help text, because every higher level still calls the formatter.
    void Parser::setHelpFormatter(std::shared_ptr<HelpFormatter> formatter) {
        stdc_impl_t;
        impl.formatter = formatter ? std::move(formatter) : defaultFormatter();
    }

    const std::shared_ptr<HelpFormatter> &Parser::helpFormatter() const {
        stdc_impl_t;
        return impl.formatter;
    }

    std::optional<std::string> Parser::validate(ParseOptions parseOptions) const {
        stdc_impl_t;
        return detail::validate_tree(*impl.root, parseOptions.test_flag(IgnoreOptionCase),
                                     parseOptions.test_flag(IgnoreCommandCase));
    }

    ParseResult Parser::parseImpl(array_view<std::string> args, ParseOptions parseOptions) const {
        stdc_impl_t;
        ParseResult result;
        auto &out = *result._impl;
        out.root = impl.root;
        out.target = impl.root.get();
        out.prologue = impl.prologue;
        out.epilogue = impl.epilogue;
        out.help_layout = impl.help_layout;
        out.formatter = impl.formatter;
        out.display_options = impl.display_options;
        out.text_width = impl.text_width;
        out.indent = impl.indent;
        out.spacing = impl.spacing;
        ParserCore(&out, parseOptions).run(args);
        return result;
    }

}
