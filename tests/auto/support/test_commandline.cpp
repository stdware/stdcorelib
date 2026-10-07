// SPDX-License-Identifier: MIT

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <stdcorelib/console.h>
#include <stdcorelib/path.h>
#include <stdcorelib/support/commandline.h>

// Private header. The individual checks that compose whole-tree validation are shared with their
// dedicated tests but are not part of the public API.
#include "support/commandline_p.h"

#include <boost/test/unit_test.hpp>

// Required by the single case that reads back the output of showError() on stderr.
#ifdef _WIN32
#  include <io.h>
#  define STDC_TEST_DUP    _dup
#  define STDC_TEST_DUP2   _dup2
#  define STDC_TEST_CLOSE  _close
#  define STDC_TEST_FILENO _fileno
#else
#  include <unistd.h>
#  define STDC_TEST_DUP    dup
#  define STDC_TEST_DUP2   dup2
#  define STDC_TEST_CLOSE  close
#  define STDC_TEST_FILENO fileno
#endif

using namespace stdc::cli;

// Contents, in order. Each line is the heading of a section below, spelled identically, so that a
// search for a line finds its section. Line numbers are omitted because every commit can
// invalidate them.
//
//     Conversion of a token to a type
//     The builders
//     Parsing
//     Edge cases from the CLI11, argparse and argtable3 test suites
//     The help text
//     Forms required by complete programs
//     Degenerate trees and misuse
//     Program flow from argv to a handler
//     Error reporting: corrections and printed output
//     Line wrapping and wrap width
//     Subcommands and inherited options
//     Layout: block selection and order
//     HelpFormatter levels and their overrides
//     Validity of a command tree
//     Reuse, ownership and lifetimes

namespace {

    /// Returns the value that a case requires to be present. Because dereferencing an empty
    /// optional is undefined behavior, an absent value fails the assertion that requested it.
    template <class T>
    T must(const std::optional<T> &value) {
        BOOST_REQUIRE_MESSAGE(value.has_value(), "expected a value, got nothing");
        return *value;
    }

    /// Reads \a token as a \c T and returns whether the conversion succeeded.
    template <class T>
    bool reads(std::string_view token, T *out) {
        return value_traits<T>::parse(token, out);
    }

    /// Reads \a token as a \c T and requires the conversion to succeed. Used by cases that verify
    /// only the resulting value.
    template <class T>
    T read(std::string_view token) {
        T out{};
        BOOST_REQUIRE_MESSAGE(reads(token, &out), "could not read \"" << token << "\"");
        return out;
    }

    struct Fraction {
        int numerator = 0;
        int denominator = 1;
    };

    /// Returns the names of the commands on \a path. The cases compare paths by these names.
    std::vector<std::string> names(const std::vector<const Command *> &path) {
        std::vector<std::string> res;
        for (const Command *command : path) {
            res.push_back(command->name());
        }
        return res;
    }

}

/// A caller-defined type. Verifies that the customization point is accessible from outside the
/// library and that a type with its own token syntax is supported.
template <>
struct stdc::cli::value_traits<Fraction> {
    static bool parse(std::string_view token, Fraction *out) {
        auto slash = token.find('/');
        if (slash == std::string_view::npos) {
            return false;
        }
        return value_traits<int>::parse(token.substr(0, slash), &out->numerator) &&
               value_traits<int>::parse(token.substr(slash + 1), &out->denominator);
    }
    static const char *type_name() {
        return "fraction";
    }
};

BOOST_AUTO_TEST_SUITE(test_commandline)

// ---------------------------------------------------------------------------------------------
// Conversion of a token to a type
// ---------------------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(test_string_takes_anything) {
    BOOST_CHECK_EQUAL(read<std::string>(""), "");
    BOOST_CHECK_EQUAL(read<std::string>("--not-an-option"), "--not-an-option");
    BOOST_CHECK_EQUAL(read<std::string>(" spaces kept "), " spaces kept ");

    // The string_view alternative refers to the original bytes rather than to a copy.
    std::string_view token = "borrowed";
    std::string_view view;
    BOOST_REQUIRE(reads(token, &view));
    BOOST_CHECK(view.data() == token.data());
}

BOOST_AUTO_TEST_CASE(test_integers) {
    BOOST_CHECK_EQUAL(read<int>("0"), 0);
    BOOST_CHECK_EQUAL(read<int>("42"), 42);
    BOOST_CHECK_EQUAL(read<int>("-42"), -42);
    BOOST_CHECK_EQUAL(read<int>("+42"), 42);

    int out;
    // A number must occupy the entire token. A token with a numeric prefix is rejected.
    BOOST_CHECK(!reads("12abc", &out));
    BOOST_CHECK(!reads("", &out));
    BOOST_CHECK(!reads(" 12", &out));
    BOOST_CHECK(!reads("12 ", &out));
    BOOST_CHECK(!reads("1.5", &out));
    BOOST_CHECK(!reads("0x10", &out));
    BOOST_CHECK(!reads("--", &out));
}

BOOST_AUTO_TEST_CASE(test_integer_range_belongs_to_the_target_type) {
    // The range check uses the requested type rather than int64_t. A value outside the range of a
    // narrower requested type is therefore rejected.
    BOOST_CHECK_EQUAL(read<uint8_t>("255"), 255);
    uint8_t small;
    BOOST_CHECK(!reads("256", &small));

    BOOST_CHECK_EQUAL(read<int8_t>("-128"), -128);
    int8_t signed_small;
    BOOST_CHECK(!reads("-129", &signed_small));

    BOOST_CHECK_EQUAL(read<int64_t>("9223372036854775807"), INT64_MAX);
    int64_t big;
    BOOST_CHECK(!reads("9223372036854775808", &big));

    BOOST_CHECK_EQUAL(read<uint64_t>("18446744073709551615"), UINT64_MAX);
}

// This case verifies a guarantee of the standard library rather than of the library under test.
// On MSVC, libstdc++ and libc++, from_chars into an unsigned type rejects a minus sign. Therefore,
// the library does not reject the sign explicitly. A standard library that accepts the minus sign
// fails this case.
BOOST_AUTO_TEST_CASE(test_negative_is_not_an_unsigned) {
    unsigned out;
    BOOST_CHECK(!reads("-1", &out));
    BOOST_CHECK(!reads("-0", &out));
    BOOST_CHECK(!reads("-", &out));

    // Because from_chars also rejects a plus sign, the sign is removed before the call.
    BOOST_CHECK_EQUAL(read<unsigned>("+7"), 7u);
}

BOOST_AUTO_TEST_CASE(test_floating_point) {
    BOOST_CHECK_CLOSE(read<double>("1.5"), 1.5, 1e-9);
    BOOST_CHECK_CLOSE(read<double>("-2"), -2.0, 1e-9);
    BOOST_CHECK_CLOSE(read<double>("1e3"), 1000.0, 1e-9);
    BOOST_CHECK_CLOSE(read<float>("0.25"), 0.25f, 1e-6f);

    double out;
    BOOST_CHECK(!reads("1.5.5", &out));
    BOOST_CHECK(!reads("", &out));
    BOOST_CHECK(!reads("abc", &out));
    BOOST_CHECK(!reads("1.5x", &out));
    // A value outside the range of double is rejected rather than converted to infinity.
    BOOST_CHECK(!reads("1e400", &out));

    // The range check uses the requested type. Without this, conversion through double accepts
    // this value and narrows it to infinity.
    float narrow;
    BOOST_CHECK(!reads("1e39", &narrow));

    // On a platform on which long double is wider than double, conversion must not pass through
    // double and lose the wider range. On MSVC and Apple arm64 the two types are identical.
    // Therefore, no wider value exists to test there.
    if constexpr (std::numeric_limits<long double>::max_exponent10 >
                  std::numeric_limits<double>::max_exponent10) {
        long double wide;
        BOOST_REQUIRE(reads("1e400", &wide));
        BOOST_CHECK(std::isfinite(wide));
    }
}

BOOST_AUTO_TEST_CASE(test_booleans_spell_themselves_several_ways) {
    for (auto token : {"true", "TRUE", "True", "yes", "on", "1"}) {
        BOOST_CHECK_MESSAGE(read<bool>(token), token);
    }
    for (auto token : {"false", "FALSE", "no", "off", "0"}) {
        BOOST_CHECK_MESSAGE(!read<bool>(token), token);
    }

    bool out;
    BOOST_CHECK(!reads("", &out));
    BOOST_CHECK(!reads("2", &out));
    BOOST_CHECK(!reads("maybe", &out));
}

BOOST_AUTO_TEST_CASE(test_a_caller_can_add_a_type) {
    auto half = read<Fraction>("1/2");
    BOOST_CHECK_EQUAL(half.numerator, 1);
    BOOST_CHECK_EQUAL(half.denominator, 2);

    Fraction out;
    BOOST_CHECK(!reads("1", &out));
    BOOST_CHECK(!reads("1/x", &out));

    BOOST_CHECK_EQUAL(std::string(value_traits<Fraction>::type_name()), "fraction");
}

BOOST_AUTO_TEST_CASE(test_type_info_carries_the_check_without_a_template) {
    // Argument stores this structure, which records a type without making Argument a template.
    auto info = detail::type_info_for<int>();
    BOOST_REQUIRE(info.check != nullptr);
    BOOST_CHECK(info.check("42"));
    BOOST_CHECK(!info.check("x"));
    BOOST_CHECK_EQUAL(std::string(info.name), "int");

    auto text = detail::type_info_for<std::string>();
    BOOST_CHECK(text.check("anything at all"));
}

// ---------------------------------------------------------------------------------------------
// The builders
// ---------------------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(test_argument_defaults) {
    Argument arg("file", "The file to read");
    BOOST_CHECK_EQUAL(arg.name(), "file");
    BOOST_CHECK_EQUAL(arg.description(), "The file to read");
    BOOST_CHECK(arg.isRequired());
    BOOST_CHECK(arg.arity() == Argument::Single);
    BOOST_CHECK(!arg.hasDefaultValue());
    BOOST_CHECK(arg.expectedValues().empty());
    BOOST_CHECK(!arg.validator());
    // Without a requested type there is no check. Therefore, the argument accepts any token.
    BOOST_CHECK(arg.typeInfo().check == nullptr);
    // Without a metavar the display name is the argument name.
    BOOST_CHECK_EQUAL(arg.displayName(), "file");
}

BOOST_AUTO_TEST_CASE(test_argument_setters_chain) {
    auto arg = Argument("count", "How many").metavar("N").optional().defaultValue("1").type<int>();

    BOOST_CHECK_EQUAL(arg.displayName(), "N");
    BOOST_CHECK(!arg.isRequired());
    BOOST_REQUIRE(arg.hasDefaultValue());
    BOOST_CHECK_EQUAL(arg.defaultValue(), "1");

    BOOST_REQUIRE(arg.typeInfo().check != nullptr);
    BOOST_CHECK(arg.typeInfo().check("7"));
    BOOST_CHECK(!arg.typeInfo().check("seven"));
    BOOST_CHECK_EQUAL(std::string(arg.typeInfo().name), "int");
}

BOOST_AUTO_TEST_CASE(test_argument_arity) {
    BOOST_CHECK(Argument("x").arity() == Argument::Single);
    BOOST_CHECK(Argument("x").multi().arity() == Argument::Multiple);
    BOOST_CHECK(Argument("x").multi(false).arity() == Argument::Single);
    BOOST_CHECK(Argument("x").nargs(Argument::Remainder).arity() == Argument::Remainder);
}

BOOST_AUTO_TEST_CASE(test_argument_carries_a_validator_and_a_set_of_words) {
    auto arg = Argument("mode")
                   .expect({"fast", "slow"})
                   .validate([](std::string_view token, std::string *error) {
                       if (token == "slow") {
                           *error = "too slow";
                           return false;
                       }
                       return true;
                   });

    BOOST_CHECK(arg.expectedValues() == std::vector<std::string>({"fast", "slow"}));

    std::string error;
    BOOST_REQUIRE(arg.validator());
    BOOST_CHECK(arg.validator()("fast", &error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK(!arg.validator()("slow", &error));
    BOOST_CHECK_EQUAL(error, "too slow");
}

BOOST_AUTO_TEST_CASE(test_option_is_built_several_ways) {
    Option from_list({"-f", "--force"}, "Force it");
    BOOST_CHECK(from_list.tokens() == std::vector<std::string>({"-f", "--force"}));
    BOOST_CHECK_EQUAL(from_list.token(), "-f");
    BOOST_CHECK_EQUAL(from_list.description(), "Force it");

    Option from_one("--force");
    BOOST_CHECK(from_one.tokens() == std::vector<std::string>({"--force"}));

    Option from_vector(std::vector<std::string>{"-e", "--exclude"});
    BOOST_CHECK_EQUAL(from_vector.tokens().size(), 2u);

    // Default values. These checks record the definition of an option with no settings.
    BOOST_CHECK(!from_list.isRequired());
    BOOST_CHECK(!from_list.isRecursive());
    BOOST_CHECK(from_list.role() == Option::NoRole);
    BOOST_CHECK(from_list.prior() == Option::NoPrior);
    BOOST_CHECK(from_list.shortMatch() == Option::NoShortMatch);
    BOOST_CHECK_EQUAL(from_list.maxOccurrence(), 1);
    BOOST_CHECK(from_list.arguments().empty());
}

BOOST_AUTO_TEST_CASE(test_option_roles_bring_their_own_spelling) {
    Option help = Option::Help;
    BOOST_CHECK(help.role() == Option::Help);
    BOOST_CHECK(help.tokens() == std::vector<std::string>({"-h", "--help"}));

    BOOST_CHECK(Option(Option::Version).tokens() == std::vector<std::string>({"-v", "--version"}));
    BOOST_CHECK_EQUAL(Option(Option::Version).description(), "Show the version and exit");

    // Explicit tokens retain the role and replace the default tokens.
    Option renamed(Option::Help, {"--usage"}, "Print usage");
    BOOST_CHECK(renamed.role() == Option::Help);
    BOOST_CHECK(renamed.tokens() == std::vector<std::string>({"--usage"}));
    BOOST_CHECK_EQUAL(renamed.description(), "Print usage");

    // A role without default tokens produces an option without tokens.
    BOOST_CHECK(Option(Option::NoRole).tokens().empty());
}

BOOST_AUTO_TEST_CASE(test_option_setters_chain) {
    auto opt = Option({"-D", "--define"}, "Define a variable")
                   .arg("expr")
                   .multi()
                   .shortMatch(Option::ShortMatchSingleChar)
                   .prior(Option::IgnoreMissingArguments)
                   .required()
                   .recursive();

    BOOST_REQUIRE_EQUAL(opt.arguments().size(), 1u);
    BOOST_CHECK_EQUAL(opt.arguments().front().name(), "expr");
    BOOST_CHECK(opt.arguments().front().isRequired());
    // An option argument requires no description because the option description applies to it.
    BOOST_CHECK(opt.arguments().front().description().empty());

    BOOST_CHECK_EQUAL(opt.maxOccurrence(), 0);
    BOOST_CHECK(opt.shortMatch() == Option::ShortMatchSingleChar);
    BOOST_CHECK(opt.prior() == Option::IgnoreMissingArguments);
    BOOST_CHECK(opt.isRequired());
    BOOST_CHECK(opt.isRecursive());

    // An optional argument, and an argument constructed separately.
    auto with_optional = Option("-w").arg("file", false);
    BOOST_CHECK(!with_optional.arguments().front().isRequired());

    auto with_typed = Option("-n").arg(Argument("count").type<int>());
    BOOST_CHECK(with_typed.arguments().front().typeInfo().check("3"));
}

BOOST_AUTO_TEST_CASE(test_prior_is_a_ladder) {
    // The parser applies the highest specified level rather than handling each level separately.
    // The declaration order of the enumerators is therefore part of their semantics.
    BOOST_CHECK(Option::NoPrior < Option::IgnoreMissingArguments);
    BOOST_CHECK(Option::IgnoreMissingArguments < Option::IgnoreMissingSymbols);
    BOOST_CHECK(Option::IgnoreMissingSymbols < Option::AutoSetWhenNoSymbols);
    BOOST_CHECK(Option::AutoSetWhenNoSymbols < Option::ExclusiveToArguments);
    BOOST_CHECK(Option::ExclusiveToArguments < Option::ExclusiveToOptions);
    BOOST_CHECK(Option::ExclusiveToOptions < Option::ExclusiveToAll);
}

BOOST_AUTO_TEST_CASE(test_command_collects_what_it_is_given) {
    auto command = Command("copy", "Copy files")
                       .addArguments({
                           Argument("src", "Source").multi(),
                           Argument("dest", "Destination"),
                       })
                       .addOptions({
                           Option({"-e", "--exclude"}, "Exclude a pattern").arg("regex").multi(),
                           Option({"-f", "--force"}, "Force overwrite"),
                       })
                       .addOption({Option::Version})
                       .setHandler([](const ParseResult &) { return 7; });

    BOOST_CHECK_EQUAL(command.name(), "copy");
    BOOST_CHECK_EQUAL(command.description(), "Copy files");
    BOOST_REQUIRE_EQUAL(command.arguments().size(), 2u);
    BOOST_CHECK_EQUAL(command.arguments()[0].name(), "src");
    BOOST_CHECK(command.arguments()[0].arity() == Argument::Multiple);
    BOOST_REQUIRE_EQUAL(command.options().size(), 3u);
    BOOST_CHECK(command.options()[2].role() == Option::Version);
    BOOST_REQUIRE(command.handler());
}

BOOST_AUTO_TEST_CASE(test_command_addition_appends_rather_than_replaces) {
    // Each method is called twice. A program adds its common options after its specific options
    // in this way.
    Command command("x");
    command.addOption(Option("-a")).addOptions({Option("-b"), Option("-c")});
    BOOST_CHECK_EQUAL(command.options().size(), 3u);

    command.addArgument(Argument("one")).addArguments({Argument("two")});
    BOOST_CHECK_EQUAL(command.arguments().size(), 2u);

    command.addCommand(Command("sub")).addCommands({Command("other")});
    BOOST_CHECK_EQUAL(command.commands().size(), 2u);
}

BOOST_AUTO_TEST_CASE(test_command_lookup) {
    auto command = Command("root")
                       .addOptions({Option({"-f", "--force"}), Option({"-e", "--exclude"})})
                       .addCommands({Command("copy"), Command("rmdir")});

    BOOST_REQUIRE(command.findCommand("copy") != nullptr);
    BOOST_CHECK_EQUAL(command.findCommand("copy")->name(), "copy");
    BOOST_CHECK(command.findCommand("nothing") == nullptr);

    // Lookup matches every token of an option, not only the first token.
    BOOST_REQUIRE(command.findOption("-f") != nullptr);
    BOOST_CHECK_EQUAL(command.findOption("--force")->token(), "-f");
    BOOST_CHECK_EQUAL(command.findOption("--exclude")->token(), "-e");
    BOOST_CHECK(command.findOption("-x") == nullptr);
    BOOST_CHECK(command.findOption("") == nullptr);

    // Lookup covers the direct children only, not the whole tree.
    auto nested = Command("outer").addCommand(Command("inner").addCommand(Command("deep")));
    BOOST_CHECK(nested.findCommand("inner") != nullptr);
    BOOST_CHECK(nested.findCommand("deep") == nullptr);
}

BOOST_AUTO_TEST_CASE(test_catalogue_groups_by_heading) {
    CommandCatalogue catalogue;
    BOOST_CHECK(catalogue.isEmpty());

    catalogue.addCommands("Filesystem Commands", {"copy", "rmdir", "touch"})
        .addCommands("Buildsystem Commands", {"configure", "deploy"})
        .addOptions("Common Options", {"-V"});

    BOOST_CHECK(!catalogue.isEmpty());
    BOOST_REQUIRE_EQUAL(catalogue.commandGroups().size(), 2u);
    // Because the headings appear in declaration order, the catalogue preserves that order.
    BOOST_CHECK_EQUAL(catalogue.commandGroups()[0].name, "Filesystem Commands");
    BOOST_CHECK_EQUAL(catalogue.commandGroups()[0].members.size(), 3u);
    BOOST_CHECK_EQUAL(catalogue.commandGroups()[1].name, "Buildsystem Commands");
    BOOST_REQUIRE_EQUAL(catalogue.optionGroups().size(), 1u);
    BOOST_CHECK(catalogue.argumentGroups().empty());

    Command command("root");
    BOOST_CHECK(command.catalogue().isEmpty());
    command.setCatalogue(catalogue);
    BOOST_CHECK_EQUAL(command.catalogue().commandGroups().size(), 2u);
}

BOOST_AUTO_TEST_CASE(test_a_command_tree_copies_whole) {
    // Because commands are value types, passing a command copies it rather than sharing it.
    // Building a command in a lambda and returning it must work because trees of any size are
    // built in this way.
    auto make = [] {
        return Command("copy", "Copy files").addOption(Option("-f")).addArgument(Argument("src"));
    };
    Command original = make();
    Command copy = original;

    copy.addOption(Option("-g"));
    BOOST_CHECK_EQUAL(original.options().size(), 1u);
    BOOST_CHECK_EQUAL(copy.options().size(), 2u);
}

// ---------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------

namespace {

    /// Returns \a rest preceded by the program name that the shell supplies as the first element.
    /// The parser is required to skip the program name.
    std::vector<std::string> argv(std::initializer_list<std::string> rest) {
        std::vector<std::string> res{"prog"};
        res.insert(res.end(), rest.begin(), rest.end());
        return res;
    }

    /// Parses \a args and requires success. A failed parse reports its error text at this
    /// assertion rather than causing a failure at a later point.
    ParseResult ok(const Parser &parser, std::initializer_list<std::string> args,
                   Parser::ParseOptions flags = Parser::Standard) {
        auto result = parser.parse(argv(args), flags);
        BOOST_REQUIRE_MESSAGE(result.isValid(), result.errorText());
        return result;
    }

    ParseResult bad(const Parser &parser, std::initializer_list<std::string> args,
                    ParseResult::Error expected, Parser::ParseOptions flags = Parser::Standard) {
        auto result = parser.parse(argv(args), flags);
        BOOST_REQUIRE_MESSAGE(!result.isValid(), "expected a failure, got a clean parse");
        BOOST_CHECK_EQUAL(int(result.error()), int(expected));
        // Every error must have a nonempty error text.
        BOOST_CHECK_MESSAGE(!result.errorText().empty(), "the failure came with nothing to print");
        return result;
    }


    // showError() writes to stderr and showHelp() writes to stdout. Capturing either output
    // requires redirecting the corresponding descriptor to a file for the duration of the call.
    // Because a file is never a terminal, the console code resolves color to never and the
    // captured output is plain text, unless the caller has forced a color mode.
    template <class F>
    std::string captured(FILE *stream, const char *name, F &&body) {
        auto path = std::filesystem::temp_directory_path() / name;

        std::fflush(stream);
        int saved = STDC_TEST_DUP(STDC_TEST_FILENO(stream));
        BOOST_REQUIRE(saved >= 0);

        FILE *file = nullptr;
#ifdef _WIN32
        fopen_s(&file, path.string().c_str(), "wb");
#else
        file = std::fopen(path.string().c_str(), "wb");
#endif
        BOOST_REQUIRE(file != nullptr);

        STDC_TEST_DUP2(STDC_TEST_FILENO(file), STDC_TEST_FILENO(stream));
        body();
        std::fflush(stream);
        STDC_TEST_DUP2(saved, STDC_TEST_FILENO(stream));
        STDC_TEST_CLOSE(saved);
        std::fclose(file);

        std::ifstream in(path, std::ios::binary);
        std::string res((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return res;
    }

    template <class F>
    std::string capturedStderr(F &&body) {
        return captured(stderr, "stdc_cli_stderr.txt", std::forward<F>(body));
    }

    template <class F>
    std::string capturedStdout(F &&body) {
        return captured(stdout, "stdc_cli_stdout.txt", std::forward<F>(body));
    }
}

BOOST_AUTO_TEST_CASE(test_parse_bare_command) {
    Parser parser(Command("prog", "A program"));
    auto result = ok(parser, {});
    BOOST_REQUIRE(result.command() != nullptr);
    BOOST_CHECK_EQUAL(result.command()->name(), "prog");
    BOOST_CHECK(names(result.commandPath()) == std::vector<std::string>({"prog"}));
}

BOOST_AUTO_TEST_CASE(test_positional_arguments) {
    Parser parser(Command("prog").addArguments({Argument("src"), Argument("dest")}));

    auto result = ok(parser, {"a", "b"});
    BOOST_CHECK_EQUAL(must(result.value(0)), "a");
    BOOST_CHECK_EQUAL(must(result.value(1)), "b");

    // An index past the end returns std::nullopt rather than crashing. Therefore, a caller can
    // read an optional argument without a prior check. Because the result is std::nullopt rather
    // than an empty string, it is distinguishable from an argument given as an empty string.
    BOOST_CHECK(!result.value(2).has_value());
    BOOST_CHECK(!result.value(-1).has_value());

    bad(parser, {"a"}, ParseResult::MissingCommandArgument);
    bad(parser, {"a", "b", "c"}, ParseResult::TooManyArguments);
}

BOOST_AUTO_TEST_CASE(test_a_multi_argument_leaves_room_for_what_follows) {
    // The form copy <src>... <dest> requires the greedy argument to leave one token for <dest>.
    Parser parser(
        Parser(Command("prog").addArguments({Argument("src").multi(), Argument("dest")})));

    auto result = ok(parser, {"one", "two", "three", "out"});
    BOOST_CHECK(result.values(0) == std::vector<std::string>({"one", "two", "three"}));
    BOOST_CHECK_EQUAL(must(result.value(1)), "out");

    // Two tokens are assigned one to each argument.
    auto pair = ok(parser, {"one", "out"});
    BOOST_CHECK(pair.values(0) == std::vector<std::string>({"one"}));
    BOOST_CHECK_EQUAL(must(pair.value(1)), "out");

    bad(parser, {"only"}, ParseResult::MissingCommandArgument);
}

// The same form applied to the arguments of an option. This case verifies that the form also
// works for option arguments. Because commands and options share a single rule, every form that
// detail::arguments_can_follow() allows is valid wherever arguments are declared rather than only
// on a command.
BOOST_AUTO_TEST_CASE(test_an_options_multi_argument_leaves_room_for_what_follows) {
    Parser parser(
        Command("prog")
            .addOption(
                Option({"--copy"}, "Copy").arg(Argument("src").multi()).arg(Argument("dest")))
            .addOption(Option({"-v"}, "Say more")));

    auto result = ok(parser, {"--copy", "one", "two", "three", "out"});
    auto handle = result.option("--copy");
    BOOST_REQUIRE(handle);
    BOOST_CHECK(must(handle->values<std::string>(0)) ==
                std::vector<std::string>({"one", "two", "three"}));
    BOOST_CHECK_EQUAL(must(handle->value<std::string>(1)), "out");

    // Two tokens are assigned one to each argument, as for the arguments of a command.
    auto pair = ok(parser, {"--copy", "one", "out"});
    BOOST_REQUIRE(pair.option("--copy"));
    BOOST_CHECK(must(pair.option("--copy")->values<std::string>(0)) ==
                std::vector<std::string>({"one"}));
    BOOST_CHECK_EQUAL(must(pair.option("--copy")->value<std::string>(1)), "out");

    // A single token leaves no value for the destination. The result is a missing argument
    // rather than a greedy source that consumes the token.
    bad(parser, {"--copy", "only"}, ParseResult::MissingOptionArgument);

    // A run is the sequence of tokens that follows an option occurrence and supplies its
    // arguments. The run ends at the next declared option. The tokens are distributed within that
    // run rather than across the whole command line.
    auto stopped = ok(parser, {"--copy", "one", "two", "-v"});
    BOOST_REQUIRE(stopped.option("--copy"));
    BOOST_CHECK(must(stopped.option("--copy")->values<std::string>(0)) ==
                std::vector<std::string>({"one"}));
    BOOST_CHECK_EQUAL(must(stopped.option("--copy")->value<std::string>(1)), "two");
    BOOST_CHECK(stopped.option("-v").has_value());
}

// A run stops at an option in every spelling of that option. The test for whether a token is an
// option must accept the same three additional spellings as option parsing. If the test uses a
// plain lookup instead, a run consumes --out=x and -Dfoo as values of the current argument.
BOOST_AUTO_TEST_CASE(test_a_run_stops_at_every_spelling_of_an_option) {
    Parser parser(
        Command("prog")
            .addOption(Option({"-c"}, "Copy").arg(Argument("src").multi()))
            .addOption(Option({"--out"}, "Where").arg("dir"))
            .addOption(Option({"-D"}, "Define").arg("macro").shortMatch(Option::ShortMatchAll))
            .addOption(Option({"-a"}, "A"))
            .addOption(Option({"-b"}, "B")));

    const std::vector<std::string> one{"x"};

    // The separate spelling, followed by the three spellings that a plain lookup misses.
    BOOST_CHECK(ok(parser, {"-c", "x", "--out", "d"}).option("-c")->values() == one);

    auto joined = ok(parser, {"-c", "x", "--out=d"});
    BOOST_CHECK(joined.option("-c")->values() == one);
    BOOST_CHECK_EQUAL(joined.valueForOption("--out").value_or(""), "d");

    auto stuck = ok(parser, {"-c", "x", "-Dfoo"});
    BOOST_CHECK(stuck.option("-c")->values() == one);
    BOOST_CHECK_EQUAL(stuck.valueForOption("-D").value_or(""), "foo");

    auto grouped = ok(parser, {"-c", "x", "-ab"}, Parser::AllowUnixGroupFlags);
    BOOST_CHECK(grouped.option("-c")->values() == one);
    BOOST_CHECK(grouped.option("-a").has_value());
    BOOST_CHECK(grouped.option("-b").has_value());

    // With grouping disabled, -ab matches no option and is an ordinary value.
    BOOST_CHECK(ok(parser, {"-c", "x", "-ab"}).option("-c")->values() ==
                std::vector<std::string>({"x", "-ab"}));

    // The same option given twice. Each occurrence reads its own run.
    Parser twice(
        Command("prog").addOption(Option({"-c"}, "Copy").arg(Argument("src").multi()).multi()));
    auto both = ok(twice, {"-c=a", "b", "-c=x", "y"});
    BOOST_CHECK_EQUAL(both.option("-c")->count(), 2);
    BOOST_CHECK(both.option("-c")->at(0).values() == std::vector<std::string>({"a", "b"}));
    BOOST_CHECK(both.option("-c")->at(1).values() == std::vector<std::string>({"x", "y"}));
}

// An occurrence of an option provides the same four accessors as a command result, with the same
// names. The accessors of an option without an occurrence index read the first occurrence and no
// other occurrence. The accessors that read every occurrence have separate names. Without at(),
// a greedy argument given twice has no accessor at all.
BOOST_AUTO_TEST_CASE(test_each_occurrence_reads_the_way_a_command_does) {
    Parser parser(Command("prog").addOption(
        Option({"-c"}, "Copy").arg(Argument("src").multi()).arg("dest").multi()));
    // The ParseResult is stored in a variable rather than queried as a temporary. The warning on
    // OptionResult against outliving its ParseResult applies.
    auto result = ok(parser, {"-c", "a", "b", "d1", "-c", "x", "y", "d2"});
    auto given = result.option("-c");
    BOOST_REQUIRE(given.has_value());
    BOOST_CHECK_EQUAL(given->count(), 2);

    // Per-occurrence access. Without at(), this access is unavailable.
    BOOST_CHECK(given->at(0).values(0) == std::vector<std::string>({"a", "b"}));
    BOOST_CHECK(given->at(1).values(0) == std::vector<std::string>({"x", "y"}));
    BOOST_CHECK_EQUAL(given->at(0).value(1).value_or(""), "d1");
    BOOST_CHECK_EQUAL(given->at(1).value(1).value_or(""), "d2");

    // The four accessors of the option return exactly the values of the first occurrence.
    BOOST_CHECK(given->value(0) == given->at(0).value(0));
    BOOST_CHECK(given->values(0) == given->at(0).values(0));
    BOOST_CHECK(given->rawValue(1) == given->at(0).rawValue(1));
    BOOST_CHECK(given->rawValues(0) == given->at(0).rawValues(0));

    // The remaining pair reads every occurrence at once. No other accessor provides this.
    BOOST_CHECK(given->allValues(0) == std::vector<std::string>({"a", "b", "x", "y"}));
    BOOST_CHECK(given->allValues(1) == std::vector<std::string>({"d1", "d2"}));

    // An undeclared argument index returns an empty result, as everywhere a value is read. An
    // occurrence index beyond the given occurrences violates the precondition of at(). Therefore,
    // this case does not test at(2) or at(-1).
    BOOST_CHECK(!given->at(0).value(9).has_value());
    BOOST_CHECK(given->at(1).values(9) == std::vector<std::string>());

    // The form that distinguishes the two kinds of accessor is an optional argument that is
    // omitted in the first occurrence and given in the second. The accessors of the option read
    // the empty first occurrence. Only the accessor with the all prefix returns the given value.
    Parser maybe(Command("prog").addOption(Option({"-p"}, "Maybe").arg("v", false).multi()));
    auto twice = ok(maybe, {"-p", "-p", "x"});
    auto p = twice.option("-p");
    BOOST_REQUIRE(p.has_value());
    BOOST_CHECK_EQUAL(p->count(), 2);
    BOOST_CHECK(!p->at(0).value().has_value());
    BOOST_CHECK_EQUAL(p->at(1).value().value_or(""), "x");
    BOOST_CHECK(!p->value().has_value());
    BOOST_CHECK(p->values() == std::vector<std::string>());
    BOOST_CHECK(p->allValues() == std::vector<std::string>({"x"}));
}

BOOST_AUTO_TEST_CASE(test_remainder_takes_everything_left) {
    Parser parser(Command("prog").addArguments(
        {Argument("script"), Argument("args").nargs(Argument::Remainder).optional()}));

    auto result = ok(parser, {"run.sh", "one", "two"});
    BOOST_CHECK_EQUAL(must(result.value(0)), "run.sh");
    BOOST_CHECK(result.values(1) == std::vector<std::string>({"one", "two"}));
}

BOOST_AUTO_TEST_CASE(test_default_value_stands_in) {
    Parser parser(Command("prog").addArgument(
        Argument("level", "How loud", false).defaultValue("3").type<int>()));

    BOOST_CHECK_EQUAL(must(ok(parser, {}).value<int>(0)), 3);
    BOOST_CHECK_EQUAL(must(ok(parser, {"7"}).value<int>(0)), 7);
}

BOOST_AUTO_TEST_CASE(test_options_in_their_several_spellings) {
    Parser parser(
        Command("prog").addOptions({Option({"-f", "--force"}, "Force"),
                                    Option({"-o", "--out"}, "Where to write").arg("dir")}));

    BOOST_CHECK(ok(parser, {"-f"}).option("-f").has_value());
    // Every token identifies the same option, both on the command line and in a query.
    BOOST_CHECK(ok(parser, {"--force"}).option("-f").has_value());
    BOOST_CHECK(ok(parser, {"-f"}).option("--force").has_value());
    BOOST_CHECK(!ok(parser, {}).option("-f").has_value());

    BOOST_CHECK_EQUAL(must(ok(parser, {"-o", "build"}).valueForOption("-o")), "build");
    BOOST_CHECK_EQUAL(must(ok(parser, {"--out=build"}).valueForOption("--out")), "build");

    bad(parser, {"-x"}, ParseResult::UnknownOption);
    bad(parser, {"-o"}, ParseResult::MissingOptionArgument);
    // An option without arguments rejects an attached value.
    bad(parser, {"--force=yes"}, ParseResult::UnknownOption);
}

BOOST_AUTO_TEST_CASE(test_repeated_options) {
    Parser parser(Command("prog").addOptions({
        Option({"-e", "--exclude"}, "Exclude").arg("pattern").multi(),
        Option({"-f"}, "Force"),
    }));

    auto result = ok(parser, {"-e", "a", "-e", "b", "-e", "c"});
    BOOST_CHECK_EQUAL(result.option("-e")->count(), 3);
    BOOST_CHECK(result.option("-e")->allRawValues() ==
                std::vector<std::string_view>({"a", "b", "c"}));
    // Each occurrence remains individually accessible. The four accessors of the option return
    // the values of the first occurrence rather than the values of all three occurrences.
    BOOST_CHECK_EQUAL(must(result.option("-e")->at(1).rawValue(0)), "b");
    BOOST_CHECK(result.option("-e")->rawValues() == std::vector<std::string_view>({"a"}));

    // An option not declared as repeatable rejects a second occurrence.
    bad(parser, {"-f", "-f"}, ParseResult::OptionOccurTooMuch);
}

BOOST_AUTO_TEST_CASE(test_short_match_joins_a_value_to_its_option) {
    Parser parser(Command("prog").addOptions({
        Option({"-D", "--define"}, "Define")
            .arg("expr")
            .multi()
            .shortMatch(Option::ShortMatchSingleChar),
        Option({"-p"}, "Plain").arg("value"),
    }));

    auto result = ok(parser, {"-DKEY=VALUE"});
    BOOST_CHECK_EQUAL(must(result.valueForOption("-D")), "KEY=VALUE");

    // The separate form remains valid, and both forms can be combined.
    auto mixed = ok(parser, {"-D", "A=1", "-DB=2"});
    BOOST_CHECK(mixed.option("-D")->allRawValues() ==
                std::vector<std::string_view>({"A=1", "B=2"}));

    // Short matching applies only to an option that enables it.
    bad(parser, {"-pvalue"}, ParseResult::UnknownOption);
}

BOOST_AUTO_TEST_CASE(test_short_match_rules_differ) {
    auto build = [](Option::ShortMatch rule) {
        return Parser(Command("prog").addOption(
            Option({"-1", "--one"}, "Numeric token").arg("value").shortMatch(rule)));
    };

    // Because ShortMatchSingleLetter requires a letter, an option token with a digit does not
    // match.
    BOOST_CHECK(!build(Option::ShortMatchSingleLetter).parse(argv({"-1x"})).isValid());
    // ShortMatchSingleChar accepts any character.
    BOOST_CHECK(build(Option::ShortMatchSingleChar).parse(argv({"-1x"})).isValid());

    // A longer token matches only under the rule that allows any length.
    Parser strict(Command("prog").addOption(
        Option({"--jobs"}, "How many").arg("n").shortMatch(Option::ShortMatchSingleChar)));
    BOOST_CHECK(!strict.parse(argv({"--jobs8"})).isValid());

    Parser loose(Command("prog").addOption(
        Option({"--jobs"}, "How many").arg("n").shortMatch(Option::ShortMatchAll)));
    BOOST_CHECK_EQUAL(must(ok(loose, {"--jobs8"}).valueForOption("--jobs")), "8");
}

BOOST_AUTO_TEST_CASE(test_grouped_short_flags) {
    Parser parser(Command("prog").addOptions({
        Option({"-a"}, "A"),
        Option({"-b"}, "B"),
        Option({"-c"}, "C").arg("value"),
    }));

    auto result = ok(parser, {"-ab"}, Parser::AllowUnixGroupFlags);
    BOOST_CHECK(result.option("-a").has_value());
    BOOST_CHECK(result.option("-b").has_value());

    // Because grouping is disabled by default, the same command line is one unknown option.
    bad(parser, {"-ab"}, ParseResult::UnknownOption);

    // An option that requires a value cannot appear inside a group. The whole group is therefore
    // rejected rather than partially accepted.
    bad(parser, {"-abc"}, ParseResult::UnknownOption, Parser::AllowUnixGroupFlags);
}

// The program author declares the point where option parsing stops and chooses the token that
// marks it. Because the parser reserves no token, the conventional -- is an ordinary declaration.
BOOST_AUTO_TEST_CASE(test_a_program_says_where_options_stop) {
    const auto &tree = [](const char *word) {
        return Parser(Command("prog")
                          .addOption(Option({"-f"}, "Force"))
                          .addOption(Option({word}, "The rest, whatever it looks like")
                                         .arg(Argument("rest").nargs(Argument::Remainder))));
    };

    auto usual = tree("--");
    auto result = ok(usual, {"-f", "--", "-f", "--not-an-option"});
    BOOST_CHECK(result.option("-f").has_value());
    BOOST_CHECK_EQUAL(result.option("-f")->count(), 1);
    BOOST_REQUIRE(result.option("--"));
    BOOST_CHECK(must(result.option("--")->values<std::string>(0)) ==
                std::vector<std::string>({"-f", "--not-an-option"}));

    // Any other token behaves identically because -- has no built-in meaning.
    auto chosen = tree("--exec");
    auto other = ok(chosen, {"-f", "--exec", "-f", "--not-an-option"});
    BOOST_CHECK_EQUAL(other.option("-f")->count(), 1);
    BOOST_CHECK(must(other.option("--exec")->values<std::string>(0)) ==
                std::vector<std::string>({"-f", "--not-an-option"}));

    // If undeclared, -- is an unknown option rather than a rule of the syntax.
    Parser bare(Command("prog").addOption(Option({"-f"}, "Force")));
    bad(bare, {"--", "x"}, ParseResult::UnknownOption);
}

// The terminator combined with an option whose argument is greedy. Other cases cover each feature
// separately but not the combination. This case detects a defect in which the greedy run stops at
// every declared option except the terminator, which is the token whose purpose is to end the
// run.
BOOST_AUTO_TEST_CASE(test_the_terminator_ends_a_greedy_option_too) {
    Parser parser(Command("prog")
                      .addArgument(Argument("dest"))
                      .addOption(Option({"-f"}, "Files").arg(Argument("file").multi()))
                      .addOption(Option({"-v"}, "Say more")));

    // The behavior at an ordinary declared option, for comparison.
    auto stopped = ok(parser, {"-f", "a", "b", "-v", "dest"});
    BOOST_CHECK(must(stopped.option("-f")->values<std::string>(0)) ==
                std::vector<std::string>({"a", "b"}));
    BOOST_CHECK_EQUAL(must(stopped.value(0)), "dest");

    // A declared terminator also ends the run. Every token after the terminator is a value, even
    // if it has the form of an option. This is the sole purpose of Remainder.
    Parser withRest(
        Command("prog")
            .addArgument(Argument("dest"))
            .addOption(Option({"-f"}, "Files").arg(Argument("file").multi()))
            .addOption(
                Option({"--"}, "The rest").arg(Argument("rest").nargs(Argument::Remainder))));
    auto ended = ok(withRest, {"dest", "-f", "a", "b", "--", "-f", "x"});
    BOOST_CHECK(must(ended.option("-f")->values<std::string>(0)) ==
                std::vector<std::string>({"a", "b"}));
    BOOST_CHECK_EQUAL(must(ended.value(0)), "dest");
    BOOST_CHECK(must(ended.option("--")->values<std::string>(0)) ==
                std::vector<std::string>({"-f", "x"}));

    // A required argument does not accept a declared option as its value. A negative number is
    // a value because it matches no declared option.
    Parser single(Command("prog")
                      .addOption(Option({"-o"}, "Out").arg("dir"))
                      .addOption(Option({"-v"}, "Say more")));
    bad(single, {"-o", "-v"}, ParseResult::MissingOptionArgument);
    BOOST_CHECK_EQUAL(must(ok(single, {"-o", "-5"}).valueForOption("-o")), "-5");
}

BOOST_AUTO_TEST_CASE(test_subcommands) {
    Parser parser(Command("prog").addCommands({
        Command("copy", "Copy").addArgument(Argument("src")),
        Command("remove", "Remove").addArgument(Argument("path")),
    }));

    auto result = ok(parser, {"copy", "a"});
    BOOST_REQUIRE(result.command() != nullptr);
    BOOST_CHECK_EQUAL(result.command()->name(), "copy");
    BOOST_CHECK(names(result.commandPath()) == std::vector<std::string>({"prog", "copy"}));
    BOOST_CHECK_EQUAL(must(result.value(0)), "a");

    // On a command without arguments, a token that matches no subcommand is reported as an
    // unknown command and named in the error text, rather than counted as a surplus argument.
    auto failure = bad(parser, {"nonsense"}, ParseResult::UnknownCommand);
    BOOST_CHECK(failure.errorText().find("nonsense") != std::string::npos);

    // On a command with arguments, a token that matches no subcommand is an argument, and only
    // surplus tokens are counted as excess.
    Parser mixed(Command("prog").addArgument(Argument("a")).addCommand(Command("copy")));
    BOOST_CHECK_EQUAL(must(ok(mixed, {"nonsense"}).value(0)), "nonsense");
    bad(mixed, {"one", "two"}, ParseResult::TooManyArguments);
}

BOOST_AUTO_TEST_CASE(test_nested_subcommands) {
    Parser parser(Command("prog").addCommand(
        Command("remote").addCommand(Command("add").addArgument(Argument("name")))));

    auto result = ok(parser, {"remote", "add", "origin"});
    BOOST_CHECK(names(result.commandPath()) == std::vector<std::string>({"prog", "remote", "add"}));
    BOOST_CHECK_EQUAL(must(result.value(0)), "origin");
}

BOOST_AUTO_TEST_CASE(test_global_options_reach_subcommands) {
    Parser parser(Command("prog")
                      .addOption(Option({"-V", "--verbose"}, "Talk more").recursive())
                      .addOption(Option({"-q"}, "Local to the root"))
                      .addCommand(Command("copy")));

    auto result = ok(parser, {"copy", "-V"});
    BOOST_CHECK(result.option("-V").has_value());

    // A non-recursive option is valid only on the command that declares it.
    bad(parser, {"copy", "-q"}, ParseResult::UnknownOption);
}

BOOST_AUTO_TEST_CASE(test_required_option) {
    Parser parser(Command("prog").addOption(Option({"-o"}, "Out").arg("dir").required()));

    BOOST_CHECK(ok(parser, {"-o", "x"}).option("-o").has_value());
    bad(parser, {}, ParseResult::MissingRequiredOption);
}

BOOST_AUTO_TEST_CASE(test_a_declared_type_is_checked_while_parsing) {
    Parser parser(Command("prog")
                      .addArgument(Argument("count").type<int>())
                      .addOption(Option({"-r"}, "Ratio").arg(Argument("value").type<double>())));

    BOOST_CHECK_EQUAL(must(ok(parser, {"12"}).value<int>(0)), 12);
    BOOST_CHECK_CLOSE(must(ok(parser, {"1", "-r", "0.5"}).valueForOption<double>("-r")), 0.5, 1e-9);

    // A declared type turns an invalid token into a diagnostic rather than a zero read later.
    auto failure = bad(parser, {"twelve"}, ParseResult::ArgumentTypeMismatch);
    BOOST_CHECK(failure.errorText().find("int") != std::string::npos);
    bad(parser, {"1", "-r", "half"}, ParseResult::ArgumentTypeMismatch);
}

BOOST_AUTO_TEST_CASE(test_expected_values_and_validators) {
    Parser parser(Command("prog")
                      .addArgument(Argument("mode").expect({"fast", "slow"}))
                      .addOption(Option({"-n"}, "Name")
                                     .arg(Argument("value").validate(
                                         [](std::string_view token, std::string *error) {
                                             if (token.empty()) {
                                                 *error = "a name cannot be empty";
                                                 return false;
                                             }
                                             return true;
                                         }))));

    BOOST_CHECK_EQUAL(must(ok(parser, {"fast"}).value(0)), "fast");
    bad(parser, {"medium"}, ParseResult::InvalidArgumentValue);

    auto failure = bad(parser, {"fast", "-n", ""}, ParseResult::ArgumentValidateFailed);
    // The error text is the message from the validator rather than a generic message.
    BOOST_CHECK_EQUAL(failure.errorText(), "a name cannot be empty");
}

BOOST_AUTO_TEST_CASE(test_prior_lets_help_answer_an_incomplete_line) {
    Parser parser(Command("prog")
                      .addArgument(Argument("required one"))
                      .addOption(Option(Option::Help).prior(Option::IgnoreMissingSymbols)));

    // Without the prior level this is a missing argument.
    BOOST_CHECK(!parser.parse(argv({})).isValid());
    auto result = ok(parser, {"--help"});
    BOOST_CHECK(result.isRoleSet(Option::Help));
    // The caller queries the role, not the token.
    BOOST_CHECK(!result.isRoleSet(Option::Version));
}

BOOST_AUTO_TEST_CASE(test_prior_can_set_itself_on_an_empty_line) {
    Parser parser(Command("prog")
                      .addArgument(Argument("required one"))
                      .addOption(Option(Option::Help).prior(Option::AutoSetWhenNoSymbols)));

    auto result = ok(parser, {});
    BOOST_CHECK(result.option("--help").has_value());

    // If any token is given, the option is not set automatically.
    BOOST_CHECK(!ok(parser, {"value"}).option("--help").has_value());
}

// Every tokenizer behavior, verified at the root and one level down with the same declarations in
// both places. Logic that reads the whole command line rather than the tokens given to the reached
// command is correct on a flat tree and incorrect under a subcommand. This case detects a defect in
// which AutoSetWhenNoSymbols fails on every subcommand, which another case misses although it
// executes the line of code that evaluates the condition.
BOOST_AUTO_TEST_CASE(test_the_parser_reads_the_same_a_level_down) {
    const auto &declare = [](std::string name) {
        return Command(std::move(name))
            .addArguments({Argument("src").multi(), Argument("dest")})
            .addOptions({
                Option({"-f", "--force"}, "Force"),
                Option({"-o", "--output"}, "Out").arg("file"),
                Option({"-n"}, "Count").arg(Argument("n").type<int>()),
                Option({"-m"}, "Mode").arg(Argument("mode").expect({"fast", "slow"})),
                Option({"-c"}, "Config").arg(Argument("path", {}, false).defaultValue("none")),
            });
    };

    // Every value a caller can read, collected into one string. A mismatch therefore reports the
    // differing values rather than only the existence of a difference.
    const auto &readBack = [](const ParseResult &result) {
        std::string res = result.isValid() ? "ok" : "error " + std::to_string(int(result.error()));
        for (int i = 0; i < 2; ++i) {
            res += " |";
            for (auto value : result.rawValues(i)) {
                res += " " + std::string(value);
            }
        }
        for (const auto *token : {"-f", "-o", "-n", "-m", "-c"}) {
            auto given = result.option(token);
            res += std::string(" ") + token + "=";
            res += given ? given->value<std::string>().value_or("(set)") : "(no)";
        }
        return res;
    };

    const std::vector<std::vector<std::string>> lines = {
        {},
        {"a"},
        {"a", "b"},
        {"a", "b", "c"},
        {"-f", "a", "b"},
        {"a", "-f", "b"},
        {"a", "b", "-f"},
        {"-o", "x", "a", "b"},
        {"--output=x", "a", "b"},
        {"-ox", "a", "b"},
        {"--force", "--output", "x", "a", "b"},
        {"-fo", "x", "a", "b"},
        {"--", "-f", "b"},
        {"-n", "12", "a", "b"},
        {"-n", "notanumber", "a", "b"},
        {"-m", "fast", "a", "b"},
        {"-m", "sideways", "a", "b"},
        {"-c", "a", "b"},
        {"--unknown", "a", "b"},
        {"-f", "-f", "a", "b"},
        {"-o", "a", "b"},
    };

    for (auto options : {Parser::Standard, Parser::AllowUnixGroupFlags}) {
        Parser flat(declare("prog"));
        Parser deep(Command("prog").addCommand(declare("inner")));
        Parser deeper(Command("prog").addCommand(Command("mid").addCommand(declare("inner"))));

        for (const auto &line : lines) {
            std::string what;
            for (const auto &item : line) {
                what += " " + item;
            }

            const auto &at = [&line, options](const Parser &parser, std::vector<std::string> path) {
                path.insert(path.end(), line.begin(), line.end());
                return parser.parse(path, options);
            };
            auto root = readBack(at(flat, {"prog"}));
            for (const auto &deeperOne :
                 {std::make_pair(std::vector<std::string>{"prog", "inner"}, &deep),
                  std::make_pair(std::vector<std::string>{"prog", "mid", "inner"}, &deeper)}) {
                auto below = readBack(at(*deeperOne.second, deeperOne.first));
                BOOST_CHECK_MESSAGE(root == below,
                                    "[" + what + "] reads one way at the root and another " +
                                        std::to_string(deeperOne.first.size() - 1) +
                                        " down:\n  root " + root + "\n  down " + below);
            }
        }
    }
}

// The behavior of every Option::Prior level, verified at the root and one level down. In a tree
// with only a root, an empty command line and a reached command without tokens are the same
// condition. The two conditions differ only below the root. This case detects a defect in which
// AutoSetWhenNoSymbols fails on every subcommand, which another case misses although it executes
// the line of code that evaluates the condition.
BOOST_AUTO_TEST_CASE(test_the_prior_ladder_reads_the_same_a_level_down) {
    const auto &shaped = [](Option::Prior level, bool nested) {
        auto inner = Command(nested ? "build" : "prog")
                         .addArgument(Argument("target"))
                         .addOption(Option({"-j"}, "Jobs").arg("n"))
                         .addOption(Option(Option::Help).prior(level));
        return nested ? Command("prog").addCommand(std::move(inner)) : std::move(inner);
    };
    const auto &given = [&shaped](Option::Prior level, bool nested, std::vector<std::string> args) {
        Parser parser(shaped(level, nested));
        std::vector<std::string> line = {"prog"};
        if (nested) {
            line.push_back("build");
        }
        line.insert(line.end(), args.begin(), args.end());
        return parser.parse(line);
    };

    for (auto level :
         {Option::NoPrior, Option::IgnoreMissingArguments, Option::IgnoreMissingSymbols,
          Option::AutoSetWhenNoSymbols, Option::ExclusiveToArguments, Option::ExclusiveToOptions,
          Option::ExclusiveToAll}) {
        const std::string at = "prior level " + std::to_string(int(level));
        for (const auto &args : {
                 std::vector<std::string>{},
                 std::vector<std::string>{"--help"},
                 std::vector<std::string>{"--help", "-j", "4"},
                 std::vector<std::string>{"x"}
        }) {
            auto flat = given(level, false, args);
            auto deep = given(level, true, args);

            std::string what = at + ", given";
            for (const auto &item : args) {
                what += " " + item;
            }
            BOOST_CHECK_MESSAGE(flat.isValid() == deep.isValid(),
                                what + ": valid at the root and not a level down, or the other "
                                       "way about");
            BOOST_CHECK_MESSAGE(int(flat.error()) == int(deep.error()), what + ": a different "
                                                                               "error each way");
            BOOST_CHECK_MESSAGE(flat.isRoleSet(Option::Help) == deep.isRoleSet(Option::Help),
                                what + ": the option was set at one depth and not the other");
        }
    }
}

// The relevant condition is the set of tokens given to the reached command, not the length of the
// command line. The name of a subcommand is a token. Counting all tokens therefore treats
// "prog build" as a nonempty command line, and the automatic option of the subcommand is never
// set. This case detects a defect in which every addHelpOption(true) on a subcommand has no
// effect.
BOOST_AUTO_TEST_CASE(test_prior_sets_itself_on_a_bare_subcommand) {
    const auto &tree = [] {
        return Parser(
            Command("prog")
                .addOption(Option(Option::Version).prior(Option::AutoSetWhenNoSymbols))
                .addCommand(
                    Command("build")
                        .addArgument(Argument("target"))
                        .addOption(Option({"-j"}, "Jobs").arg("n"))
                        .addOption(Option(Option::Help).prior(Option::AutoSetWhenNoSymbols))));
    };

    // A subcommand without following tokens is the form that fails if all tokens of the command
    // line are counted. The required argument is not reported as missing because the set option
    // replaces the whole command line.
    auto parser = tree();
    auto bare = ok(parser, {"build"});
    BOOST_CHECK(bare.option("--help").has_value());
    BOOST_CHECK(bare.isRoleSet(Option::Help));

    // If a value is given, the option is not set and the argument is read normally.
    auto given = ok(parser, {"build", "x"});
    BOOST_CHECK(!given.option("--help").has_value());
    BOOST_CHECK_EQUAL(must(given.value(0)), "x");

    // If an option is given instead of a value, the option is not set either, and the missing
    // argument is reported.
    bad(parser, {"build", "-j", "4"}, ParseResult::MissingCommandArgument);

    // Reaching a subcommand does not set the automatic option of the root because the command
    // line is not empty.
    BOOST_CHECK(!bare.isRoleSet(Option::Version));
    BOOST_CHECK(ok(parser, {}).isRoleSet(Option::Version));
}

BOOST_AUTO_TEST_CASE(test_exclusive_prior_levels) {
    auto build = [](Option::Prior level) {
        return Parser(Command("prog")
                          .addArgument(Argument("path").optional())
                          .addOption(Option({"-f"}, "Force"))
                          .addOption(Option(Option::Version).prior(level)));
    };

    // A sole option is valid at every exclusive level.
    BOOST_CHECK(build(Option::ExclusiveToAll).parse(argv({"--version"})).isValid());

    // With an additional argument, only the levels that forbid arguments report an error.
    BOOST_CHECK(build(Option::ExclusiveToOptions).parse(argv({"--version", "x"})).isValid());
    auto with_argument = build(Option::ExclusiveToArguments).parse(argv({"--version", "x"}));
    BOOST_CHECK_EQUAL(int(with_argument.error()), int(ParseResult::PriorOptionWithArguments));

    // With an additional option, only the levels that forbid options report an error.
    BOOST_CHECK(build(Option::ExclusiveToArguments).parse(argv({"--version", "-f"})).isValid());
    auto with_option = build(Option::ExclusiveToAll).parse(argv({"--version", "-f"}));
    BOOST_CHECK_EQUAL(int(with_option.error()), int(ParseResult::PriorOptionWithOptions));
}

// The constraint "no arguments may be given" applies to the given tokens. A default value
// substitutes for an absent argument. If the check reads the arguments after defaults are
// applied, an option that forbids arguments rejects a command line that contains none.
BOOST_AUTO_TEST_CASE(test_an_exclusive_option_reads_what_was_given_not_what_stood_in) {
    auto build = [](Option::Prior level, bool with_default) {
        auto path = Argument("path").optional();
        if (with_default) {
            path.defaultValue("a.out");
        }
        return Parser(Command("prog")
                          .addArgument(path)
                          .addOption(Option({"-f"}, "Force"))
                          .addOption(Option({"--stat"}, "Just report").prior(level)));
    };

    // In both variants the command line contains no argument, and the default remains readable.
    BOOST_CHECK(ok(build(Option::ExclusiveToArguments, false), {"--stat"}).values(0) ==
                std::vector<std::string>());
    BOOST_CHECK_EQUAL(
        ok(build(Option::ExclusiveToArguments, true), {"--stat"}).value(0).value_or(""), "a.out");

    // A given argument is rejected. This is the sole purpose of the level.
    bad(build(Option::ExclusiveToArguments, true), {"--stat", "x"},
        ParseResult::PriorOptionWithArguments);

    // ExclusiveToAll forbids both, and the error must concern the additional option. If the
    // check reads the defaults, the error incorrectly concerns arguments.
    bad(build(Option::ExclusiveToAll, true), {"--stat", "-f"}, ParseResult::PriorOptionWithOptions);
    BOOST_CHECK_EQUAL(ok(build(Option::ExclusiveToAll, true), {"--stat"}).value(0).value_or(""),
                      "a.out");

    // Several arguments, so that the token count and the argument count differ.
    Parser several(
        Command("prog")
            .addArgument(Argument("a").optional().defaultValue("1"))
            .addArgument(Argument("b").optional().defaultValue("2"))
            .addOption(Option({"--stat"}, "Just report").prior(Option::ExclusiveToArguments)));
    auto filled = ok(several, {"--stat"});
    BOOST_CHECK_EQUAL(filled.value(0).value_or(""), "1");
    BOOST_CHECK_EQUAL(filled.value(1).value_or(""), "2");
    bad(several, {"--stat", "x"}, ParseResult::PriorOptionWithArguments);

    // Combined with a leading Remainder. Its default substitutes in the same way, and its tokens
    // are read before any argument is filled.
    Parser tail(
        Command("prog")
            .addArgument(
                Argument("rest").nargs(Argument::Remainder).optional().defaultValue("none"))
            .addOption(Option({"--stat"}, "Just report").prior(Option::ExclusiveToArguments)));
    BOOST_CHECK_EQUAL(ok(tail, {"--stat"}).value(0).value_or(""), "none");
    bad(tail, {"--stat", "x"}, ParseResult::PriorOptionWithArguments);

    // Combined with a recursive option. The option that forbids arguments belongs to another
    // command.
    Parser below(
        Command("prog")
            .addOption(
                Option({"--stat"}, "Just report").prior(Option::ExclusiveToArguments).recursive())
            .addCommand(
                Command("build").addArgument(Argument("target").optional().defaultValue("all"))));
    BOOST_CHECK_EQUAL(ok(below, {"build", "--stat"}).value(0).value_or(""), "all");
    bad(below, {"build", "--stat", "x"}, ParseResult::PriorOptionWithArguments);
}

BOOST_AUTO_TEST_CASE(test_an_option_may_ignore_its_own_missing_arguments) {
    Parser parser(Command("prog").addOption(
        Option({"-l"}, "List").arg("what").prior(Option::IgnoreMissingArguments)));

    auto result = ok(parser, {"-l"});
    BOOST_CHECK(result.option("-l").has_value());
    // The option is present and its argument is absent, which differs from an argument that is
    // the empty string. \c -l and \c -l "" produce different results.
    BOOST_CHECK(!result.valueForOption("-l").has_value());
}

BOOST_AUTO_TEST_CASE(test_case_insensitivity_is_asked_for) {
    Parser parser(
        Command("prog").addOption(Option({"--force"}, "Force")).addCommand(Command("copy")));

    BOOST_CHECK(ok(parser, {"--FORCE"}, Parser::IgnoreOptionCase).option("--force").has_value());
    BOOST_CHECK(!parser.parse(argv({"--FORCE"})).isValid());

    BOOST_CHECK_EQUAL(ok(parser, {"COPY"}, Parser::IgnoreCommandCase).command()->name(), "copy");
    // Without the flag the token is not a command. Therefore, it is treated as an argument.
    BOOST_CHECK(!parser.parse(argv({"COPY"})).isValid());
}

BOOST_AUTO_TEST_CASE(test_dos_short_options) {
    Parser parser(Command("prog").addOption(Option({"-f"}, "Force")));

    BOOST_CHECK(ok(parser, {"/f"}, Parser::AllowDosShortOptions).option("-f").has_value());
    // The flag is disabled by default. The token is then a positional argument, and the command
    // accepts none.
    bad(parser, {"/f"}, ParseResult::TooManyArguments);
}

BOOST_AUTO_TEST_CASE(test_unix_short_options_can_be_turned_off) {
    Parser parser(Command("prog")
                      .addArgument(Argument("path").optional())
                      .addOption(Option({"-f", "--force"}, "Force")));

    BOOST_CHECK(ok(parser, {"-f"}).option("-f").has_value());
    // With Unix short options disabled, a token with a single dash is an ordinary token and is
    // assigned to the argument.
    auto result = ok(parser, {"-f"}, Parser::DontAllowUnixShortOptions);
    BOOST_CHECK(!result.option("-f").has_value());
    BOOST_CHECK_EQUAL(must(result.value(0)), "-f");
    // The long token is unaffected.
    BOOST_CHECK(
        ok(parser, {"--force"}, Parser::DontAllowUnixShortOptions).option("-f").has_value());
}

BOOST_AUTO_TEST_CASE(test_invoke_runs_the_command_that_was_reached) {
    std::string seen;
    Parser parser(Command("prog")
                      .addCommand(Command("copy")
                                      .addArgument(Argument("src"))
                                      .setHandler([&seen](const ParseResult &result) {
                                          seen = must(result.value(0));
                                          return 3;
                                      }))
                      .addCommand(Command("bare")));

    BOOST_CHECK_EQUAL(parser.invoke(argv({"copy", "file"})), 3);
    BOOST_CHECK_EQUAL(seen, "file");

    // A missing handler and a failed parse both return the default code supplied by the caller.
    // invoke() prints the parse error because no other code reports it.
    BOOST_CHECK_EQUAL(parser.invoke(argv({"bare"}), -9), -9);
    int code = 0;
    auto complaint = capturedStderr([&] { code = parser.invoke(argv({"copy"}), -9); });
    BOOST_CHECK_EQUAL(code, -9);
    BOOST_CHECK(!complaint.empty());
}

BOOST_AUTO_TEST_CASE(test_typed_reads) {
    Parser parser(Command("prog")
                      .addArgument(Argument("numbers").multi().type<int>())
                      .addOption(Option({"-n"}, "How many").arg(Argument("n").type<int>())));

    auto result = ok(parser, {"1", "2", "3", "-n", "9"});
    BOOST_CHECK(result.values<int>(0) == std::vector<int>({1, 2, 3}));
    BOOST_CHECK_EQUAL(must(result.value<int>(0)), 1);
    BOOST_CHECK_EQUAL(must(result.valueForOption<int>("-n")), 9);
    // The untyped read returns the text, which is the storage form of every value.
    BOOST_CHECK_EQUAL(must(result.value(0)), "1");
}

// ---------------------------------------------------------------------------------------------
// Edge cases from the CLI11, argparse and argtable3 test suites
//
// The semantics of those libraries differ from the semantics of this library. Each case
// therefore adopts the input under test rather than the expected result.
// ---------------------------------------------------------------------------------------------

// From CLI11 DashedOptions and FlagLikeOption: the treatment of a value with the form of a switch.
BOOST_AUTO_TEST_CASE(test_a_value_that_looks_like_an_option) {
    Parser parser(Command("prog")
                      .addOption(Option({"-o"}, "Out").arg("dir"))
                      .addOption(Option({"-f"}, "Force")));

    // A declared option is never silently consumed as the value of another option. The error
    // names the option without a value, which is preferable to consuming -f without a diagnostic.
    bad(parser, {"-o", "-f"}, ParseResult::MissingOptionArgument);

    // Attached with an equals sign, the token is an ordinary value.
    BOOST_CHECK_EQUAL(must(ok(parser, {"-o=-f"}).valueForOption("-o")), "-f");

    // A token with the form of an option that matches no declared option is also an ordinary
    // value. A negative number is the relevant instance.
    BOOST_CHECK_EQUAL(must(ok(parser, {"-o", "-5"}).valueForOption("-o")), "-5");
    BOOST_CHECK_EQUAL(must(ok(parser, {"-o", "-nonsense"}).valueForOption("-o")), "-nonsense");
}

// From CLI11 ForcedPositional, extended beyond the first terminator case in this file.
BOOST_AUTO_TEST_CASE(test_what_survives_a_remainder) {
    Parser parser(Command("prog")
                      .addOption(Option({"-f"}, "Force"))
                      .addOption(Option({"--"}, "The rest")
                                     .arg(Argument("rest").nargs(Argument::Remainder).optional())));

    // A second terminator is a value, as is every token after the first terminator.
    auto twice = ok(parser, {"--", "--", "-f"});
    BOOST_REQUIRE(twice.option("--"));
    BOOST_CHECK(must(twice.option("--")->values<std::string>(0)) ==
                std::vector<std::string>({"--", "-f"}));
    BOOST_CHECK(!twice.option("-f").has_value());

    // A sole terminator produces an empty list because an optional Remainder accepts zero tokens.
    auto alone = ok(parser, {"--"});
    BOOST_REQUIRE(alone.option("--"));
    BOOST_CHECK(must(alone.option("--")->values<std::string>(0)).empty());
}

// From argparse: equals signs inside values.
BOOST_AUTO_TEST_CASE(test_only_the_first_equals_sign_splits) {
    Parser parser(Command("prog").addOption(Option({"-D", "--define"}, "Define").arg("expr")));

    BOOST_CHECK_EQUAL(must(ok(parser, {"--define=KEY=VALUE"}).valueForOption("-D")), "KEY=VALUE");
    BOOST_CHECK_EQUAL(must(ok(parser, {"--define="}).valueForOption("-D")), "");
    // A token without text before the equals sign matches no declared option.
    bad(parser, {"=value"}, ParseResult::TooManyArguments);
}

// From several CLI11 tests: an option name that is a prefix of another option name.
BOOST_AUTO_TEST_CASE(test_one_option_being_a_prefix_of_another) {
    Parser parser(Command("prog").addOptions({
        Option({"--out"}, "Out").arg("dir"),
        Option({"--output"}, "Output").arg("file"),
    }));

    // Because the whole token is matched before any splitting, the longer name is reachable.
    BOOST_CHECK_EQUAL(must(ok(parser, {"--output", "a"}).valueForOption("--output")), "a");
    BOOST_CHECK_EQUAL(must(ok(parser, {"--out", "b"}).valueForOption("--out")), "b");
    BOOST_CHECK(!ok(parser, {"--output", "a"}).option("--out").has_value());
}

// From argtable3: the count of a repeatable flag rather than its values.
BOOST_AUTO_TEST_CASE(test_counting_a_flag_that_carries_nothing) {
    Parser parser(Command("prog").addOption(Option({"-v"}, "More talk").multi()));

    // Because a count is available only for a given option, it is never zero.
    auto once = ok(parser, {"-v"});
    BOOST_REQUIRE(once.option("-v").has_value());
    BOOST_CHECK_EQUAL(once.option("-v")->count(), 1);

    auto thrice = ok(parser, {"-v", "-v", "-v"});
    BOOST_CHECK_EQUAL(thrice.option("-v")->count(), 3);

    // A declared option that is not given returns an empty result, as does an undeclared option.
    // Because the program author knows which options the program declares, the two cases need not
    // be distinguished.
    BOOST_CHECK(!ok(parser, {}).option("-v").has_value());
    BOOST_CHECK(!ok(parser, {}).option("-q").has_value());
    BOOST_CHECK(!ok(parser, {"-v"}).option("-q").has_value());
}

// From CLI11 ExpectedRange, adapted to the arities of this library.
BOOST_AUTO_TEST_CASE(test_how_few_and_how_many_a_multi_argument_takes) {
    Parser parser(Command("prog").addArgument(Argument("files").multi()));

    bad(parser, {}, ParseResult::MissingCommandArgument);
    BOOST_CHECK_EQUAL(must(ok(parser, {"one"}).values(0)).size(), 1u);
    BOOST_CHECK_EQUAL(must(ok(parser, {"a", "b", "c", "d", "e"}).values(0)).size(), 5u);

    // An optional multi argument accepts zero tokens.
    Parser lenient(Command("prog").addArgument(Argument("files").multi().optional()));
    BOOST_CHECK(must(ok(lenient, {}).values(0)).empty());
}

// Two consecutive multi arguments. This case verifies that the reservation rule counts required
// arguments rather than all arguments.
BOOST_AUTO_TEST_CASE(test_two_greedy_arguments_in_a_row) {
    // This declaration is an error, and every check of it reports the error. A greedy argument
    // reserves one token for each required argument after it and none for other arguments.
    // Therefore, a second greedy argument never receives a token.
    BOOST_CHECK(!detail::arguments_can_follow({Argument("first").multi()},
                                              Argument("second").multi().optional()));

#ifdef NDEBUG
    // The tree is parsed regardless because parse() does not validate the tree in a release
    // build. Because the first argument is greedy and the second is not required, the first
    // argument receives every token.
    Parser parser(Command("prog").addArguments({
        Argument("first").multi(),
        Argument("second").multi().optional(),
    }));
    auto result = ok(parser, {"a", "b", "c"});
    BOOST_CHECK_EQUAL(must(result.values(0)).size(), 3u);
    BOOST_CHECK(must(result.values(1)).empty());
#endif
}

// Interleaved options and positional arguments. All three reference suites test this.
BOOST_AUTO_TEST_CASE(test_options_may_come_anywhere) {
    Parser parser(Command("prog")
                      .addArguments({Argument("a"), Argument("b")})
                      .addOption(Option({"-f"}, "Force"))
                      .addOption(Option({"-o"}, "Out").arg("dir")));

    for (auto args : {
             std::vector<std::string>{"-f",  "one", "two"},
             std::vector<std::string>{"one", "-f",  "two"},
             std::vector<std::string>{"one", "two", "-f" }
    }) {
        auto full = argv({});
        full.insert(full.end(), args.begin(), args.end());
        auto result = parser.parse(full);
        BOOST_REQUIRE_MESSAGE(result.isValid(), result.errorText());
        BOOST_CHECK(result.option("-f").has_value());
        BOOST_CHECK_EQUAL(must(result.value(0)), "one");
        BOOST_CHECK_EQUAL(must(result.value(1)), "two");
    }

    // The value of an option belongs to that option and is not counted as a positional argument.
    auto result = ok(parser, {"one", "-o", "dir", "two"});
    BOOST_CHECK_EQUAL(must(result.value(1)), "two");
    BOOST_CHECK_EQUAL(must(result.valueForOption("-o")), "dir");
}

// A subcommand name used as a value. CLI11 tests this as a fallthrough case.
BOOST_AUTO_TEST_CASE(test_a_subcommand_name_used_as_a_value) {
    Parser parser(Command("prog")
                      .addOption(Option({"-o"}, "Out").arg("dir"))
                      .addCommand(Command("copy").addArgument(Argument("src"))));

    // A command is matched only at a command position, which precedes every other token.
    auto result = ok(parser, {"copy", "copy"});
    BOOST_CHECK_EQUAL(result.command()->name(), "copy");
    BOOST_CHECK_EQUAL(must(result.value(0)), "copy");

    // Because a preceding option ends the command path, a following token cannot be a command.
    // A recursive option is written after the command that receives it, like every other option.
    Parser recursive(Command("prog")
                         .addOption(Option({"-V"}, "Verbose").recursive())
                         .addCommand(Command("copy").addArgument(Argument("src"))));
    bad(recursive, {"-V", "copy", "x"}, ParseResult::UnknownCommand);
    auto after = ok(recursive, {"copy", "-V", "x"});
    BOOST_CHECK_EQUAL(after.command()->name(), "copy");
    BOOST_CHECK(after.option("-V").has_value());
    BOOST_CHECK_EQUAL(must(after.value(0)), "x");

    // After a value has been read, a name is a value and no longer a command. Without this rule,
    // a program cannot receive a file whose name equals the name of a subcommand.
    Parser positional(
        Command("prog").addArguments({Argument("a"), Argument("b")}).addCommand(Command("copy")));
    auto late = ok(positional, {"x", "copy"});
    BOOST_CHECK_EQUAL(late.command()->name(), "prog");
    BOOST_CHECK_EQUAL(must(late.value(1)), "copy");

    // The same applies after a Remainder has started. Within a Remainder no token is a command or
    // an option.
    Parser rest(Command("prog")
                    .addArgument(Argument("head"))
                    .addArgument(Argument("tail").nargs(Argument::Remainder).optional())
                    .addOption(Option({"-f"}, "Force"))
                    .addCommand(Command("copy")));
    auto forced = ok(rest, {"x", "copy", "-f"});
    BOOST_CHECK_EQUAL(forced.command()->name(), "prog");
    BOOST_CHECK(forced.values(1) == std::vector<std::string>({"copy", "-f"}));
    BOOST_CHECK(!forced.option("-f").has_value());
}

// Because short matching supplies exactly one value, it is available only to an option with
// exactly one required argument. For any other option, short matching sets the option partially
// and the remaining arguments are reported as missing at a later point.
BOOST_AUTO_TEST_CASE(test_short_matching_needs_one_required_argument) {
    Parser two(Command("prog").addOption(
        Option({"-o"}, "Two values").arg("a").arg("b").shortMatch(Option::ShortMatchAll)));
    bad(two, {"-oX"}, ParseResult::UnknownOption);
    // The separate form is valid.
    BOOST_CHECK(ok(two, {"-o", "X", "Y"}).option("-o").has_value());

    Parser optional(Command("prog").addOption(
        Option({"-p"}, "Maybe a value").arg("v", false).shortMatch(Option::ShortMatchAll)));
    bad(optional, {"-pX"}, ParseResult::UnknownOption);

    Parser none(Command("prog").addOption(
        Option({"-f"}, "No value at all").shortMatch(Option::ShortMatchAll)));
    bad(none, {"-fX"}, ParseResult::UnknownOption);

    // A required greedy argument falls under the same rule. A joined value, which is a value
    // attached to the option token, indicates the single value, but a greedy run also consumes
    // the next token. The separate form with a space is valid.
    Parser greedy(Command("prog").addOption(
        Option({"-I"}, "Includes").arg(Argument("dir").multi()).shortMatch(Option::ShortMatchAll)));
    bad(greedy, {"-IX"}, ParseResult::UnknownOption);
    BOOST_CHECK(ok(greedy, {"-I", "X", "Y"}).option("-I")->values() ==
                std::vector<std::string>({"X", "Y"}));
}

// A joined value is the first value of the first argument of the option, not the complete
// argument. A Single argument is complete after the joined value, and a greedy argument continues
// with the next token. --opt=a b and --opt a b are therefore two forms of the same command line.
BOOST_AUTO_TEST_CASE(test_a_joined_value_starts_an_argument_rather_than_finishing_it) {
    const std::vector<std::string> both{"a", "b"};

    Parser greedy(
        Command("prog").addOption(Option({"-I"}, "Includes").arg(Argument("dir").multi())));
    BOOST_CHECK(ok(greedy, {"-I=a", "b"}).option("-I")->values() == both);
    BOOST_CHECK(ok(greedy, {"-I", "a", "b"}).option("-I")->values() == both);
    // Without following tokens, the joined value is the only required value of the argument.
    BOOST_CHECK(ok(greedy, {"-I=a"}).option("-I")->values() == std::vector<std::string>({"a"}));

    // The same applies to a Remainder, which receives every remaining token including options.
    Parser rest(
        Command("prog")
            .addOption(Option({"--rest"}, "The rest").arg(Argument("r").nargs(Argument::Remainder)))
            .addOption(Option({"-f"}, "Force")));
    auto joined = ok(rest, {"--rest=a", "-f", "b"});
    BOOST_CHECK(joined.option("--rest")->values() == std::vector<std::string>({"a", "-f", "b"}));
    BOOST_CHECK(!joined.option("-f").has_value());

    // A Single argument receives only the joined value. The remaining tokens are positional.
    Parser one(
        Command("prog").addArgument(Argument("path")).addOption(Option({"-o"}, "Out").arg("dir")));
    auto single = ok(one, {"-o=x", "y"});
    BOOST_CHECK_EQUAL(single.option("-o")->value().value_or(""), "x");
    BOOST_CHECK_EQUAL(single.value(0).value_or(""), "y");

    // The reservation for following arguments counts the joined value as part of the greedy run.
    Parser reserved(Command("prog").addOption(
        Option({"-c"}, "Copy").arg(Argument("src").multi()).arg(Argument("dest"))));
    auto pair = ok(reserved, {"-c=a", "b", "c"});
    BOOST_CHECK(pair.option("-c")->values(0) == both);
    BOOST_CHECK_EQUAL(pair.option("-c")->value(1).value_or(""), "c");
}

// A greedy argument consumes at least one token regardless of the reservation because its arity
// is one or more. Because a joined value already satisfies that minimum, no token is forced and
// the following arguments retain their reserved tokens. Without this rule, -c=a b, the shortest
// valid command line, fails.
BOOST_AUTO_TEST_CASE(test_a_joined_value_keeps_the_promise_the_reservation_would_have_kept) {
    Parser copy(
        Command("prog").addOption(Option({"-c"}, "Copy").arg(Argument("src").multi()).arg("dest")));

    // Each command line here must parse identically to its separate form. This is the property
    // that the case guards.
    const auto &same_as_spaced = [&copy](std::initializer_list<std::string> joined,
                                         std::initializer_list<std::string> spaced) {
        auto a = ok(copy, joined);
        auto b = ok(copy, spaced);
        BOOST_CHECK(a.option("-c")->values(0) == b.option("-c")->values(0));
        BOOST_CHECK(a.option("-c")->value(1) == b.option("-c")->value(1));
        return a;
    };
    auto minimum = same_as_spaced({"-c=a", "b"}, {"-c", "a", "b"});
    BOOST_CHECK(minimum.option("-c")->values(0) == std::vector<std::string>({"a"}));
    BOOST_CHECK_EQUAL(minimum.option("-c")->value(1).value_or(""), "b");

    auto spare = same_as_spaced({"-c=a", "b", "c"}, {"-c", "a", "b", "c"});
    BOOST_CHECK(spare.option("-c")->values(0) == std::vector<std::string>({"a", "b"}));

    // Because no token is available for the reservation, the following argument is reported as
    // missing rather than taken from the run.
    bad(copy, {"-c=a"}, ParseResult::MissingOptionArgument);
    bad(copy, {"-c", "a"}, ParseResult::MissingOptionArgument);

    // Two reserved tokens rather than one. The shortest command line contains exactly enough.
    Parser three(Command("prog").addOption(
        Option({"-c"}, "Copy").arg(Argument("src").multi()).arg("mid").arg("dest")));
    auto tight = ok(three, {"-c=a", "b", "c"});
    BOOST_CHECK(tight.option("-c")->values(0) == std::vector<std::string>({"a"}));
    BOOST_CHECK_EQUAL(tight.option("-c")->value(1).value_or(""), "b");
    BOOST_CHECK_EQUAL(tight.option("-c")->value(2).value_or(""), "c");

    // Because no argument follows, nothing is reserved and the run consumes every token. Because
    // an optional argument cannot follow a greedy argument, the last position is the only
    // position that reserves nothing.
    Parser last(Command("prog").addOption(Option({"-I"}, "Includes").arg(Argument("dir").multi())));
    BOOST_CHECK(ok(last, {"-I=a", "b"}).option("-I")->values() ==
                ok(last, {"-I", "a", "b"}).option("-I")->values());

    // An empty joined value is a value, as --prefix= is elsewhere. Therefore, it also satisfies
    // the minimum.
    auto empty = ok(copy, {"-c=", "b"});
    BOOST_CHECK(empty.option("-c")->values(0) == std::vector<std::string>({""}));
    BOOST_CHECK_EQUAL(empty.option("-c")->value(1).value_or("-"), "b");

    // Combined with the rule that a run ends at a declared option. Because no token is available
    // after the joined value, the following argument is reported as missing.
    Parser stopped(Command("prog")
                       .addOption(Option({"-c"}, "Copy").arg(Argument("src").multi()).arg("dest"))
                       .addOption(Option({"-f"}, "Force")));
    bad(stopped, {"-c=a", "-f"}, ParseResult::MissingOptionArgument);

    // Because positional arguments use the same reservation without a joined value, their
    // behavior is unchanged.
    Parser positional(
        Command("prog").addArgument(Argument("src").multi()).addArgument(Argument("dest")));
    BOOST_CHECK(ok(positional, {"a", "b"}).values(0) == std::vector<std::string>({"a"}));
    BOOST_CHECK(ok(positional, {"a", "b", "c"}).values(0) == std::vector<std::string>({"a", "b"}));
}

// Because IgnoreOptionCase applies to the declared option tokens, it applies wherever a token is
// matched. If whole tokens are matched case-insensitively and short-match prefixes are matched
// case-sensitively, -d foo succeeds and -dfoo fails.
BOOST_AUTO_TEST_CASE(test_ignoring_case_reaches_a_value_stuck_to_the_spelling) {
    Parser parser(Command("prog").addOption(
        Option({"-D"}, "Define").arg("macro").shortMatch(Option::ShortMatchAll)));

    BOOST_CHECK_EQUAL(ok(parser, {"-Dfoo"}).option("-D")->value().value_or(""), "foo");
    BOOST_CHECK_EQUAL(
        ok(parser, {"-dfoo"}, Parser::IgnoreOptionCase).option("-D")->value().value_or(""), "foo");
    BOOST_CHECK_EQUAL(
        ok(parser, {"-d", "foo"}, Parser::IgnoreOptionCase).option("-D")->value().value_or(""),
        "foo");

    // Without the flag only the declared case matches, in both forms of the same command line.
    bad(parser, {"-dfoo"}, ParseResult::UnknownOption);
    bad(parser, {"-d", "foo"}, ParseResult::UnknownOption);
}

BOOST_AUTO_TEST_CASE(test_response_files) {
    auto path = std::filesystem::temp_directory_path() / "stdc_cli_response.txt";
    {
        std::ofstream file(path);
        file << "-f\n"
             << "one\n"
             << "\n"          // blank lines are ignored
             << "--out=dir\n" // joined values are preserved
             << "two\n";
    }

    Parser parser(Command("prog")
                      .addArguments({Argument("a"), Argument("b")})
                      .addOption(Option({"-f"}, "Force"))
                      .addOption(Option({"--out"}, "Out").arg("dir")));

    auto result = ok(parser, {"@" + path.string()}, Parser::EnableResponseFile);
    BOOST_CHECK(result.option("-f").has_value());
    BOOST_CHECK_EQUAL(must(result.value(0)), "one");
    BOOST_CHECK_EQUAL(must(result.value(1)), "two");
    BOOST_CHECK_EQUAL(must(result.valueForOption("--out")), "dir");

    // The flag is disabled by default. The token is then a literal value rather than a file name.
    auto literal = ok(parser, {"@" + path.string(), "x"});
    BOOST_CHECK_EQUAL(must(literal.value(0)), "@" + path.string());
    BOOST_CHECK_EQUAL(must(literal.value(1)), "x");

    // A missing file is reported as an error rather than passed through as a value.
    bad(parser, {"@no_such_response_file.txt"}, ParseResult::ErrorReadingResponseFile,
        Parser::EnableResponseFile);

    std::filesystem::remove(path);
}

// A build system writes a response file, rather than a user typing it at a shell. A line therefore
// contains characters that a shell removes. All three forms were measured against the qmcorecmd
// suite, and without this handling each of them turns a valid command line into a diagnostic.
BOOST_AUTO_TEST_CASE(test_a_response_file_line_is_not_taken_as_it_is_written) {
    auto path = std::filesystem::temp_directory_path() / "stdc_cli_response_shapes.txt";
    const auto &given = [&path](const std::string &contents) {
        {
            // Binary mode, so that the file contains exactly the bytes that the case specifies.
            std::ofstream file(path, std::ios::binary);
            file << contents;
        }
        Parser parser(Command("prog")
                          .addArguments({Argument("a"), Argument("b")})
                          .addOption(Option({"-f"}, "Force"))
                          .addOption(Option({"--out"}, "Out").arg("dir")));
        return ok(parser, {"@" + path.string()}, Parser::EnableResponseFile);
    };

    // Leading and trailing blanks, which a generator writes to align its arguments.
    {
        auto result = given("  -f  \n\tone\t\n   --out=dir   \n  two\n");
        BOOST_CHECK(result.option("-f").has_value());
        BOOST_CHECK_EQUAL(must(result.value(0)), "one");
        BOOST_CHECK_EQUAL(must(result.value(1)), "two");
        BOOST_CHECK_EQUAL(must(result.valueForOption("--out")), "dir");
    }

    // One pair of quotes, which encloses a path that contains a space. CMake quotes every path.
    // Therefore, this is the common case rather than an exception.
    {
        auto result = given("\"src dir/\"\n\"dest dir/\"\n");
        BOOST_CHECK_EQUAL(must(result.value(0)), "src dir/");
        BOOST_CHECK_EQUAL(must(result.value(1)), "dest dir/");
    }

    // Only one enclosing pair is removed, not every quote on the line, and only if the line has a
    // matching pair.
    {
        auto result = given("\"a\"b\"c\"\n\"unbalanced\n");
        BOOST_CHECK_EQUAL(must(result.value(0)), "a\"b\"c");
        BOOST_CHECK_EQUAL(must(result.value(1)), "\"unbalanced");
    }

    // Because quotes are removed after blanks, a quoted path indented by its generator yields the
    // path.
    BOOST_CHECK_EQUAL(must(given("   \"src dir/\"   \nx\n").value(0)), "src dir/");

    // Blanks inside a pair of quotes belong to the argument and are preserved.
    BOOST_CHECK_EQUAL(must(given("\"  padded  \"\nx\n").value(0)), "  padded  ");

    // A byte order mark on the first line, which a Windows editor writes.
    {
        auto result = given("\xEF\xBB\xBF-f\none\ntwo\n");
        BOOST_CHECK(result.option("-f").has_value());
        BOOST_CHECK_EQUAL(must(result.value(0)), "one");
    }

    // A byte order mark is removed only from the first line and only if complete. Two of its three
    // bytes are not a byte order mark and remain part of the surrounding text.
    BOOST_CHECK_EQUAL(must(given("x\n\xEF\xBB\xBFy\n").value(1)), "\xEF\xBB\xBFy");
    BOOST_CHECK_EQUAL(must(given("\xEF\xBB"
                                 "y\nx\n")
                               .value(0)),
                      "\xEF\xBB"
                      "y");

    // A line that consists of a pair of quotes is an empty argument, which is the only notation for
    // an empty argument in a response file. An empty line or a line of blanks is not an argument.
    {
        auto result = given("\"\"\n   \n\nsecond\n");
        BOOST_CHECK_EQUAL(must(result.value(0)), "");
        BOOST_CHECK_EQUAL(must(result.value(1)), "second");
    }

    // CRLF is removed regardless of the platform that wrote or reads the file.
    {
        auto result = given("-f\r\n\"one two\"\r\nthree\r\n");
        BOOST_CHECK(result.option("-f").has_value());
        BOOST_CHECK_EQUAL(must(result.value(0)), "one two");
        BOOST_CHECK_EQUAL(must(result.value(1)), "three");
    }

    // The file is read as bytes, so that no content is interpreted. A Windows text stream treats
    // Ctrl-Z as the end of the file and drops every byte after it.
    {
        auto result = given("one\n\x1a"
                            "two\n");
        BOOST_CHECK_EQUAL(must(result.value(0)), "one");
        BOOST_CHECK_EQUAL(must(result.value(1)), "\x1a"
                                                 "two");
    }

    std::filesystem::remove(path);
}

// The file name after the @ is UTF-8, as is every string in this library. On Windows, ifstream
// interprets a narrow string in the system code page. Passing the name as a narrow string
// therefore reports an existing file with a non-ASCII name as missing.
BOOST_AUTO_TEST_CASE(test_a_response_file_is_found_by_a_name_that_is_not_ascii) {
    auto path = std::filesystem::temp_directory_path() /
                stdc::path::from_utf8("stdc_cli_\xe5\x93\x8d\xe5\xba\x94.txt");
    {
        std::ofstream file(path, std::ios::binary);
        file << "one\ntwo\n";
    }

    Parser parser(Command("prog").addArguments({Argument("a"), Argument("b")}));
    auto result = ok(parser, {"@" + stdc::path::to_utf8(path)}, Parser::EnableResponseFile);
    BOOST_CHECK_EQUAL(must(result.value(0)), "one");
    BOOST_CHECK_EQUAL(must(result.value(1)), "two");

    std::filesystem::remove(path);
}

BOOST_AUTO_TEST_CASE(test_an_empty_command_line_is_not_an_error_by_itself) {
    // No declarations and no tokens produce no error. argparse tests this case because a parser
    // can easily fail on it.
    Parser parser(Command("prog"));
    BOOST_CHECK(parser.parse({}).isValid());
    BOOST_CHECK(parser.parse({"prog"}).isValid());
}

// ---------------------------------------------------------------------------------------------
// The help text
// ---------------------------------------------------------------------------------------------

namespace {

    /// Returns the position of \a needle in \a text and requires that it is present. Cases
    /// compare two positions for order.
    size_t at(const std::string &text, std::string_view needle) {
        auto pos = text.find(needle);
        BOOST_REQUIRE_MESSAGE(pos != std::string::npos, "\"" << needle << "\" is nowhere in:\n"
                                                             << text);
        return pos;
    }

    bool has(const std::string &text, std::string_view needle) {
        return text.find(needle) != std::string::npos;
    }

    /// Returns the column where the description starts on the line that contains \a left. Used by
    /// the alignment cases.
    size_t descriptionColumn(const std::string &text, std::string_view left) {
        auto start = text.rfind('\n', at(text, left)) + 1;
        auto line = text.substr(start, text.find('\n', start) - start);
        auto after = line.find(left) + left.size();
        auto column = line.find_first_not_of(' ', after);
        BOOST_REQUIRE_MESSAGE(column != std::string::npos, "nothing after \"" << left << "\"");
        return column;
    }

    Parser helpTree() {
        CommandCatalogue catalogue;
        catalogue.addCommands("Filesystem Commands", {"copy"})
            .addCommands("Buildsystem Commands", {"configure"});

        Parser parser(
            Command("prog", "What the program is for")
                .addOptions({Option(Option::Help), Option({"-V"}, "Say more").recursive()})
                .addCommands({
                    Command("copy", "Copy things")
                        .addArguments(
                            {Argument("src", "Where from").multi(), Argument("dest", "Where to")})
                        .addOption(Option({"-f", "--force"}, "Overwrite")),
                    Command("configure", "Configure things")
                        .addArgument(Argument("mode", "Which way", false)
                                         .defaultValue("fast")
                                         .expect({"fast", "slow"}))
                        .addOption(Option({"-p"}, "Project").arg("name").required()),
                    Command("orphan", "Not in any group"),
                })
                .setCatalogue(catalogue));
        parser.setPrologue("A prologue line");
        parser.setEpilogue("An epilogue line");
        // A fixed width, so that the assertions do not depend on the terminal that runs the suite
        // or on the COLUMNS environment variable.
        parser.setTextWidth(80);
        return parser;
    }

}

BOOST_AUTO_TEST_CASE(test_help_layout_is_in_a_fixed_order) {
    auto text = helpTree().parse(argv({})).helpText();

    BOOST_CHECK(at(text, "A prologue line") < at(text, "What the program is for"));
    BOOST_CHECK(at(text, "What the program is for") < at(text, "Usage:"));
    BOOST_CHECK(at(text, "Usage:") < at(text, "Options:"));
    BOOST_CHECK(at(text, "Options:") < at(text, "Filesystem Commands:"));
    BOOST_CHECK(at(text, "Filesystem Commands:") < at(text, "An epilogue line"));

    // One blank line between any two blocks, none above the first and none below the last.
    // Checking only the relative order does not detect a blank line at the top of the text.
    BOOST_CHECK_EQUAL(text.rfind("A prologue line", 0), 0u);
    BOOST_CHECK(!has(text, "\n\n\n"));
    BOOST_CHECK(has(text, "An epilogue line\n"));
    BOOST_CHECK(!has(text, "An epilogue line\n\n"));
}

BOOST_AUTO_TEST_CASE(test_usage_names_the_path_it_took) {
    auto parser = helpTree();
    BOOST_CHECK(has(parser.parse(argv({})).helpText(), "Usage:\n    prog [commands] [options]"));
    BOOST_CHECK(has(parser.parse(argv({"copy", "a", "b"})).helpText(),
                    "Usage:\n    prog copy [options] <src>... <dest>"));
    // An optional argument is enclosed in brackets, and a repeatable argument has an ellipsis.
    // Because the only option that this command declares is required, the usage line lists it.
    // The [options] hint represents the recursive option inherited from the root.
    BOOST_CHECK(has(parser.parse(argv({"configure"})).helpText(),
                    "Usage:\n    prog configure -p <name> [options] [<mode>]"));
}

// Because the name of a subcommand is valid only in the first position, the usage line writes it
// first. The arrangement "prog [options] [commands]" is the arrangement that the parser
// rejects.
BOOST_AUTO_TEST_CASE(test_usage_writes_a_subcommand_before_the_options) {
    Parser parser(Command("prog")
                      .addOption(Option({"--plain"}, "Plain"))
                      .addCommand(Command("build").addArgument(Argument("target"))));
    BOOST_CHECK(has(parser.parse(argv({})).helpText(), "Usage:\n    prog [commands] [options]\n"));

    // The parser accepts the order shown on the usage line and rejects the reverse order.
    BOOST_CHECK_EQUAL(ok(parser, {"build", "x"}).command()->name(), "build");
    BOOST_CHECK(ok(parser, {"--plain"}).option("--plain").has_value());
    bad(parser, {"--plain", "build", "x"}, ParseResult::UnknownCommand);

    // A command with subcommands only.
    Parser bare(Command("prog").addCommand(Command("build")));
    BOOST_CHECK(has(bare.parse(argv({})).helpText(), "Usage:\n    prog [commands]\n"));

    // Without subcommands, nothing precedes the options.
    Parser flat(
        Command("prog").addArgument(Argument("path")).addOption(Option({"-v"}, "Say more")));
    BOOST_CHECK(has(flat.parse(argv({"x"})).helpText(), "Usage:\n    prog [options] <path>\n"));

    // The usage line of a reached command mentions commands only if that command has
    // subcommands.
    BOOST_CHECK(
        has(parser.parse(argv({"build", "x"})).helpText(), "Usage:\n    prog build <target>\n"));
}

// A required option belongs on the usage line. Inside "[options]" it is indistinguishable from
// the optional options, and the distinction is the essential property of a required option.
BOOST_AUTO_TEST_CASE(test_usage_spells_out_the_options_that_are_required) {
    // The first token of the option and its arguments, in declaration order, precede the hint
    // that represents the remaining options.
    {
        Parser parser(
            Command("prog")
                .addArgument(Argument("path"))
                .addOption(Option({"-o", "--output"}, "Where to write").arg("file").required())
                .addOption(Option({"-v"}, "Say more")));
        BOOST_CHECK(
            has(parser.parse(argv({})).helpText(), "Usage:\n    prog -o <file> [options] <path>"));
    }

    // Several required options. The hint is omitted if every option is listed.
    {
        Parser parser(Command("prog")
                          .addOption(Option({"-i"}, "In").arg("in").required())
                          .addOption(Option({"-o"}, "Out").arg("out").required()));
        BOOST_CHECK(has(parser.parse(argv({"-i", "a", "-o", "b"})).helpText(),
                        "Usage:\n    prog -i <in> -o <out>\n"));
    }

    // Without required options, the usage line contains only the hint.
    {
        Parser parser(Command("prog").addOption(Option({"-v"}, "Say more")));
        BOOST_CHECK(has(parser.parse(argv({})).helpText(), "Usage:\n    prog [options]\n"));
    }

    // The optional argument of a required option retains its brackets.
    {
        Parser parser(
            Command("prog").addOption(Option({"-c"}, "Config").arg("file", false).required()));
        BOOST_CHECK(has(parser.parse(argv({"-c"})).helpText(), "Usage:\n    prog -c [<file>]\n"));
    }

    // The required options of a subcommand appear on the usage line of that subcommand.
    {
        Parser parser(Command("prog").addCommand(
            Command("build").addOption(Option({"-t"}, "Target").arg("name").required())));
        auto text = parser.parse(argv({"build", "-t", "x"})).helpText();
        BOOST_CHECK(has(text, "Usage:\n    prog build -t <name>\n"));
    }

#ifdef NDEBUG
    // An option without tokens cannot be typed. Therefore, it is not counted for the hint either.
    // If it is counted, "[options]" appears on a command without usable options. Adding such an
    // option is an error that parse() asserts through validate() in a debug build. This block
    // verifies the behavior in a release build, which does not assert.
    // \sa test_an_option_with_no_spelling_is_ignored_rather_than_fatal
    {
        Parser parser(Command("prog").addOption(Option()));
        BOOST_CHECK(has(parser.parse(argv({})).helpText(), "Usage:\n    prog\n"));
    }
#endif
}

BOOST_AUTO_TEST_CASE(test_a_catalogue_names_the_headings_and_keeps_their_order) {
    auto text = helpTree().parse(argv({})).helpText();

    BOOST_CHECK(at(text, "Filesystem Commands:") < at(text, "Buildsystem Commands:"));
    // Commands that the catalogue does not mention appear at the end under the default heading.
    // The search is anchored to the start of a line because "Commands:" is a suffix of the
    // preceding headings.
    BOOST_CHECK(at(text, "Buildsystem Commands:") < at(text, "\nCommands:"));
    BOOST_CHECK(at(text, "\nCommands:") < at(text, "orphan"));

    // Every command appears exactly once, regardless of its group.
    for (auto name : {"copy", "configure", "orphan"}) {
        BOOST_CHECK_MESSAGE(has(text, name), name);
    }
}

BOOST_AUTO_TEST_CASE(test_aligning_all_catalogues_shares_one_column) {
    auto parser = helpTree();

    parser.setDisplayOptions(Parser::Normal);
    auto apart = parser.parse(argv({})).helpText();
    // With per-group alignment, a group of short names has a narrow first column.
    BOOST_CHECK(descriptionColumn(apart, "copy") < descriptionColumn(apart, "-h, --help"));

    parser.setDisplayOptions(Parser::AlignAllCatalogues);
    auto together = parser.parse(argv({})).helpText();
    BOOST_CHECK_EQUAL(descriptionColumn(together, "copy"),
                      descriptionColumn(together, "configure"));
    BOOST_CHECK_EQUAL(descriptionColumn(together, "copy"),
                      descriptionColumn(together, "-h, --help"));
}

BOOST_AUTO_TEST_CASE(test_the_extras_are_asked_for) {
    auto parser = helpTree();

    auto plain = parser.parse(argv({"configure"})).helpText();
    BOOST_CHECK(!has(plain, "default:"));
    BOOST_CHECK(!has(plain, "fast, slow"));
    BOOST_CHECK(!has(plain, "(required)"));

    parser.setDisplayOptions(Parser::ShowArgumentDefaultValue | Parser::ShowArgumentExpectedValues |
                             Parser::ShowOptionIsRequired);
    auto full = parser.parse(argv({"configure"})).helpText();
    BOOST_CHECK(has(full, "(default: fast)"));
    BOOST_CHECK(has(full, "[fast, slow]"));
    BOOST_CHECK(has(full, "(required)"));
}

BOOST_AUTO_TEST_CASE(test_an_options_own_argument_carries_its_extras_too) {
    Parser parser(Command("prog").addOption(
        Option({"-l"}, "How loud")
            .arg(Argument("n", {}, false).defaultValue("1").expect({"0", "1", "2"}))));
    parser.setDisplayOptions(Parser::ShowArgumentDefaultValue | Parser::ShowArgumentExpectedValues);

    auto text = parser.parse(argv({})).helpText();
    BOOST_CHECK(has(text, "-l [<n>]"));
    BOOST_CHECK(has(text, "(default: 1)"));
    BOOST_CHECK(has(text, "[0, 1, 2]"));
}

BOOST_AUTO_TEST_CASE(test_roles_describe_themselves) {
    // Without default descriptions, the two options that every program has are the only rows
    // without a description, and a generated help text appears incomplete.
    Parser parser(Command("prog").addOptions({Option(Option::Help), Option(Option::Version)}));
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(has(text, "Show this help and exit"));
    BOOST_CHECK(has(text, "Show the version and exit"));

    // An explicit description takes precedence.
    Parser named(Command("prog").addOption(Option(Option::Help, {}, "Read this")));
    BOOST_CHECK(has(named.parse(argv({})).helpText(), "Read this"));
    BOOST_CHECK(!has(named.parse(argv({})).helpText(), "Show this help"));
}

BOOST_AUTO_TEST_CASE(test_help_of_a_command_that_was_never_reached_is_empty) {
    // A default-constructed result has no command. Its help text is empty rather than an
    // out-of-bounds access.
    ParseResult empty;
    BOOST_CHECK(empty.helpText().empty());
    BOOST_CHECK(empty.command() == nullptr);
}

BOOST_AUTO_TEST_CASE(test_a_parser_can_be_built_and_returned) {
    // helpTree() returns a named local variable, which requires the move constructor. Deleting
    // the copy constructor suppresses the implicit move constructor, and this case detects that.
    auto parser = helpTree();
    BOOST_CHECK_EQUAL(parser.rootCommand().name(), "prog");
    BOOST_CHECK_EQUAL(parser.prologue(), "A prologue line");

    Parser moved = std::move(parser);
    BOOST_CHECK_EQUAL(moved.rootCommand().name(), "prog");
    BOOST_CHECK(moved.parse(argv({"copy", "a", "b"})).isValid());

    // Move assignment to a parser that already holds a tree. A parser stored as a member and
    // rebuilt later uses this path. Other cases exercise only the constructor.
    Parser assigned(Command("placeholder"));
    assigned = std::move(moved);
    BOOST_CHECK_EQUAL(assigned.rootCommand().name(), "prog");
    BOOST_CHECK(assigned.parse(argv({"copy", "a", "b"})).isValid());
}

// Every setter of a parser, verified through its getter. A program that wraps a parser, as a
// custom help formatter must, reads all of these settings. Other cases call only the two width
// getters.
BOOST_AUTO_TEST_CASE(test_a_parser_answers_for_what_it_was_told) {
    Parser parser;
    BOOST_CHECK(parser.prologue().empty());
    BOOST_CHECK(parser.epilogue().empty());
    BOOST_CHECK(parser.displayOptions() == Parser::Normal);
    BOOST_CHECK_EQUAL(parser.textWidth(), 0);
    BOOST_CHECK_EQUAL(parser.indent(), 4);
    BOOST_CHECK_EQUAL(parser.spacing(), 4);
    // The default layout is not empty. A parser without settings still produces a help text.
    BOOST_CHECK(!parser.helpLayout().isEmpty());

    parser.setRootCommand(Command("prog"));
    parser.setPrologue("A prologue line");
    parser.setEpilogue("An epilogue line");
    parser.setDisplayOptions(Parser::AlignAllCatalogues | Parser::ShowOptionIsRequired);
    parser.setTextWidth(72);
    parser.setIndent(2);
    parser.setSpacing(3);

    HelpLayout layout;
    layout.add(HelpBlock::Usage).add(HelpBlock::Options);
    parser.setHelpLayout(layout);

    BOOST_CHECK_EQUAL(parser.rootCommand().name(), "prog");
    BOOST_CHECK_EQUAL(parser.prologue(), "A prologue line");
    BOOST_CHECK_EQUAL(parser.epilogue(), "An epilogue line");
    BOOST_CHECK(parser.displayOptions() ==
                (Parser::AlignAllCatalogues | Parser::ShowOptionIsRequired));
    BOOST_CHECK_EQUAL(parser.textWidth(), 72);
    BOOST_CHECK_EQUAL(parser.indent(), 2);
    BOOST_CHECK_EQUAL(parser.spacing(), 3);
    BOOST_REQUIRE_EQUAL(parser.helpLayout().blocks().size(), 2u);
    BOOST_CHECK_EQUAL(int(parser.helpLayout().blocks().front().role), int(HelpBlock::Usage));
}

// A catalogue groups arguments and options as well as commands. Other cases render only the
// command groups. Every item not named by a group remains under the default heading at the end.
// This case therefore verifies the order and the ungrouped items, not only the headings.
BOOST_AUTO_TEST_CASE(test_a_catalogue_groups_arguments_and_options_too) {
    // An option is matched by its first token, which token() returns.
    CommandCatalogue catalogue;
    catalogue.addArguments("Inputs", {"source"})
        .addArguments("Outputs", {"dest"})
        .addOptions("Common Options", {"-f"})
        .addOptions("Rare Options", {"-m"});

    Parser parser(Command("prog", "Something")
                      .addArguments({Argument("source", "Where from"), Argument("dest", "Where to"),
                                     Argument("extra", "An ungrouped argument")})
                      .addOptions({Option({"-f", "--force"}, "Overwrite"),
                                   Option({"-m", "--mode"}, "How").arg("name"),
                                   Option({"-q"}, "An ungrouped option")})
                      .setCatalogue(catalogue));
    parser.setTextWidth(80);
    auto text = parser.parse(argv({})).helpText();

    // The search for a default heading includes the preceding newline because "Options:" is a
    // substring of "Common Options:".
    BOOST_CHECK(at(text, "Inputs:") < at(text, "Outputs:"));
    BOOST_CHECK(at(text, "Outputs:") < at(text, "\nArguments:"));
    BOOST_CHECK(at(text, "\nArguments:") < at(text, "Common Options:"));
    BOOST_CHECK(at(text, "Common Options:") < at(text, "Rare Options:"));
    BOOST_CHECK(at(text, "Rare Options:") < at(text, "\nOptions:"));

    // Each row appears under the heading of its group rather than under the default heading.
    BOOST_CHECK(at(text, "Inputs:") < at(text, "Where from"));
    BOOST_CHECK(at(text, "Where from") < at(text, "Outputs:"));
    BOOST_CHECK(at(text, "\nArguments:") < at(text, "An ungrouped argument"));
    BOOST_CHECK(at(text, "Common Options:") < at(text, "Overwrite"));
    BOOST_CHECK(at(text, "Overwrite") < at(text, "Rare Options:"));
    BOOST_CHECK(at(text, "\nOptions:") < at(text, "An ungrouped option"));
}

// ---------------------------------------------------------------------------------------------
// Forms required by complete programs
//
// Transcribing the command tree of a real build tool with this library revealed three forms that
// no preceding case declares. The cases describe the forms rather than the program. Tests specific
// to that program belong in its own suite.
// ---------------------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(test_an_option_carrying_two_arguments) {
    // One option with a pair of values, given more than once. No preceding case declares such an
    // option, and a tool that maps patterns to directories consists mostly of such options.
    Parser parser(Command("prog")
                      .addArguments({Argument("src"), Argument("dest")})
                      .addOptions({Option({"-i", "--include"}, "A pattern and its subdirectory")
                                       .arg("regex")
                                       .arg("subdir")
                                       .multi(),
                                   Option({"-e", "--exclude"}, "A pattern").arg("regex").multi()}));

    auto result = ok(parser, {"src", "dst", "-i", "a", "x", "-i", "b", "y", "-e", "z"});
    BOOST_CHECK_EQUAL(must(result.value(0)), "src");
    BOOST_CHECK_EQUAL(must(result.value(1)), "dst");

    auto given = result.option("-i");
    BOOST_REQUIRE(given.has_value());
    const auto &include = *given;
    BOOST_REQUIRE_EQUAL(include.count(), 2);
    // Access by argument index within one occurrence, which is the only meaningful access to a
    // pair.
    BOOST_CHECK_EQUAL(must(include.at(0).rawValue(0)), "a");
    BOOST_CHECK_EQUAL(must(include.at(0).rawValue(1)), "x");
    BOOST_CHECK_EQUAL(must(include.at(1).rawValue(0)), "b");
    BOOST_CHECK_EQUAL(must(include.at(1).rawValue(1)), "y");
    // Alternatively, every value of one argument index across all occurrences.
    BOOST_CHECK(include.allRawValues(0) == std::vector<std::string_view>({"a", "b"}));
    BOOST_CHECK(include.allRawValues(1) == std::vector<std::string_view>({"x", "y"}));

    // Both values of a pair are required. A single value produces a diagnostic.
    bad(parser, {"src", "dst", "-i", "a"}, ParseResult::MissingOptionArgument);

    // The exception is an option declared with IgnoreMissingArguments.
    Parser lenient(Command("prog").addOption(Option({"-c"}, "A pair")
                                                 .arg("src")
                                                 .arg("dir")
                                                 .multi()
                                                 .prior(Option::IgnoreMissingArguments)));
    BOOST_CHECK(ok(lenient, {"-c"}).option("-c").has_value());
    BOOST_CHECK_EQUAL(must(ok(lenient, {"-c", "a", "b"}).option("-c")->rawValue(1)), "b");
}

BOOST_AUTO_TEST_CASE(test_the_two_options_every_program_has) {
    // If declared manually, these options require the correct prior level to function. The
    // correct level requires knowledge that a program author cannot be expected to guess.
    Parser parser(Command("prog")
                      .addArgument(Argument("required one"))
                      .addVersionOption("1.2.3")
                      .addHelpOption(true, true)
                      .addCommand(Command("copy").addArgument(Argument("src"))));

    // A bare program name sets the help option rather than reporting the missing argument.
    auto bare = ok(parser, {});
    BOOST_CHECK(bare.isRoleSet(Option::Help));

    // On a command line without the required argument, the option is still set.
    BOOST_CHECK(ok(parser, {"--help"}).isRoleSet(Option::Help));
    BOOST_CHECK(ok(parser, {"--version"}).isRoleSet(Option::Version));

    // The help option is recursive, so that the subcommands also accept it.
    BOOST_CHECK(ok(parser, {"copy", "--help"}).isRoleSet(Option::Help));

    // The command stores the version string that the version option prints.
    BOOST_CHECK_EQUAL(parser.rootCommand().version(), "1.2.3");

    // The caller can still specify the tokens and the description.
    Parser renamed(Command("prog").addHelpOption(false, false, {"-?"}, "How to use this"));
    BOOST_CHECK(ok(renamed, {"-?"}).isRoleSet(Option::Help));
    BOOST_CHECK(has(renamed.parse(argv({})).helpText(), "How to use this"));
}

BOOST_AUTO_TEST_CASE(test_a_tree_of_several_commands_reads_as_one_page) {
    CommandCatalogue catalogue;
    catalogue.addCommands("Filesystem Commands", {"copy", "rmdir", "touch"})
        .addCommands("Buildsystem Commands", {"configure", "incsync", "deploy"});

    Command root("tool", "Utility commands");
    for (auto [name, desc] : {
             std::pair{"copy",      "Copy files"          },
             {"rmdir",     "Remove directories"  },
             {"touch",     "Update timestamps"   },
             {"configure", "Generate a header"   },
             {"incsync",   "Reorganize headers"  },
             {"deploy",    "Resolve dependencies"}
    }) {
        root.addCommand(Command(name, desc).addArgument(Argument("path").multi()));
    }
    root.addVersionOption("1.0").addHelpOption(true, true).setCatalogue(catalogue);

    Parser parser(std::move(root));
    parser.setDisplayOptions(Parser::AlignAllCatalogues);
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(at(text, "Filesystem Commands:") < at(text, "Buildsystem Commands:"));
    for (auto name : {"copy", "rmdir", "touch", "configure", "incsync", "deploy"}) {
        BOOST_CHECK_MESSAGE(has(text, name), name);
    }
    // Six commands under two headings are aligned as one table.
    BOOST_CHECK_EQUAL(descriptionColumn(text, "copy"), descriptionColumn(text, "configure"));

    // The tree also parses, which the help text alone does not verify.
    BOOST_CHECK_EQUAL(ok(parser, {"deploy", "a", "b"}).command()->name(), "deploy");
}

// ---------------------------------------------------------------------------------------------
// Degenerate trees and misuse
//
// The preceding sections describe programs that use the library correctly. For this reason they
// do not detect the defects that this section covers. Deliberately breaking a line of code
// indicates whether the tests exercise that line. It indicates nothing about a form that no test
// builds, and every such mutation passed while these defects remained undetected.
// ---------------------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(test_an_option_with_no_spelling_is_ignored_rather_than_fatal) {
    // token() calls front() on a vector that is empty in a default-constructed Option. This case
    // detects a defect in which the help text calls token() on every option, including an option
    // without tokens.
    //
    // The catalogue is required to expose the defect. Because the name of an option is queried
    // only while matching the option against a group, a tree without a catalogue never calls
    // token(). A version of this case without a catalogue passes despite the defect.
    // The declaration is an error, and every check of it reports the error. An option without
    // tokens cannot be typed and therefore cannot be given.
    BOOST_CHECK(!detail::options_can_join({}, Option()));
    BOOST_CHECK(!detail::options_can_join({}, Option(Option::NoRole)));

#ifdef NDEBUG
    // The tree is parsed regardless because parse() does not validate the tree in a release
    // build.
    CommandCatalogue catalogue;
    catalogue.addOptions("Common Options", {"-f"});

    Parser parser(Command("prog", "Something")
                      .addOption(Option())
                      .addOption(Option(Option::NoRole))
                      .addOption(Option({"-f"}, "Force"))
                      .setCatalogue(catalogue));

    auto text = parser.parse(argv({})).helpText();
    BOOST_CHECK(has(text, "Common Options:"));
    BOOST_CHECK(has(text, "-f"));
    BOOST_CHECK(has(text, "Force"));

    // Parsing also succeeds. The option without tokens cannot appear on a command line.
    BOOST_CHECK(ok(parser, {"-f"}).option("-f").has_value());
    BOOST_CHECK(!ok(parser, {}).option("").has_value());
#endif
}

BOOST_AUTO_TEST_CASE(test_a_result_outlives_the_parse_and_the_tree_it_read) {
    // The result holds pointers into the command tree and shares ownership of the tree to keep it
    // alive. Without shared ownership, a new tree overwrites the old tree, and every existing
    // result reads freed vectors.
    Parser parser(Command("first").addCommand(
        Command("copy").addArgument(Argument("src")).addOption(Option({"-f"}, "Force"))));

    auto result = parser.parse(argv({"copy", "-f", "x"}));
    BOOST_REQUIRE(result.isValid());
    BOOST_CHECK_EQUAL(result.command()->name(), "copy");

    Command replacement("second");
    for (int i = 0; i < 64; ++i) {
        replacement.addCommand(
            Command("filler" + std::to_string(i)).addOption(Option({"-x" + std::to_string(i)})));
    }
    parser.setRootCommand(std::move(replacement));

    // The existing result remains unchanged and refers to the tree that produced it.
    BOOST_CHECK_EQUAL(result.command()->name(), "copy");
    BOOST_CHECK_EQUAL(must(result.value(0)), "x");
    BOOST_CHECK(result.option("-f").has_value());
    BOOST_CHECK(has(result.helpText(), "Force"));

    // The parser uses the new tree.
    BOOST_CHECK_EQUAL(parser.rootCommand().name(), "second");
    BOOST_CHECK(parser.parse(argv({"filler3"})).isValid());
}

BOOST_AUTO_TEST_CASE(test_a_result_outlives_the_parser) {
    ParseResult result;
    {
        Parser parser(Command("prog").addArgument(Argument("path")));
        result = parser.parse(argv({"x"}));
    }
    BOOST_REQUIRE(result.isValid());
    BOOST_CHECK_EQUAL(result.command()->name(), "prog");
    BOOST_CHECK_EQUAL(must(result.value(0)), "x");
}

BOOST_AUTO_TEST_CASE(test_reading_with_a_type_the_argument_never_declared) {
    // The compiler cannot detect this mismatch because the declared type is stored in the
    // Argument and not in the template argument of the caller. The conversion performs the check.
    Parser parser(Command("prog").addArgument(Argument("name")));
    auto result = ok(parser, {"not-a-number"});

    BOOST_CHECK(!result.value<int>(0).has_value());
    // The caller specifies the fallback value at the read, rather than initializing a variable
    // beforehand and relying on the read to leave it unmodified.
    BOOST_CHECK_EQUAL(result.value<int>(0).value_or(12345), 12345);

    // A present value that converts successfully is returned in the optional.
    auto number_result = ok(Parser(Command("prog").addArgument(Argument("n"))), {"42"});
    BOOST_CHECK_EQUAL(must(number_result.value<int>(0)), 42);

    // An absent value and a zero are different results, which is the reason for the optional
    // return type. A plain int return type cannot distinguish the two.
    Parser optional(Command("prog").addArgument(Argument("n").optional()));
    BOOST_CHECK(!ok(optional, {}).value<int>(0).has_value());
    BOOST_CHECK_EQUAL(must(ok(optional, {"0"}).value<int>(0)), 0);
    BOOST_CHECK_EQUAL(ok(optional, {"0"}).value<int>(0).value_or(99), 0);

    // The default value substitutes for the absent argument, so that the result is present.
    Parser defaulted(
        Command("prog").addArgument(Argument("n").optional().defaultValue("5").type<int>()));
    BOOST_CHECK_EQUAL(ok(defaulted, {}).value<int>(0).value_or(99), 5);

    // An undeclared index returns std::nullopt, not an empty string.
    BOOST_CHECK(!result.value(7).has_value());
    BOOST_CHECK(!result.value(-1).has_value());
    BOOST_CHECK(!result.rawValue(7).has_value());

    static_assert(std::is_same_v<decltype(result.value(0)), std::optional<std::string>>,
                  "the read defaults to a type that owns what it holds");
}

BOOST_AUTO_TEST_CASE(test_the_checking_read_on_an_option) {
    Parser parser(Command("prog").addOption(Option({"-n"}, "How many").arg("count")));

    // If the option is not given, no OptionResult exists.
    BOOST_CHECK(!ok(parser, {}).option("-n").has_value());
    // The option is given, but its value is not a number.
    BOOST_CHECK(!ok(parser, {"-n", "many"}).option("-n")->value<int>().has_value());
    BOOST_CHECK_EQUAL(must(ok(parser, {"-n", "7"}).option("-n")->value<int>()), 7);

    // The shortcut must return the same result as the explicit access.
    BOOST_CHECK(!ok(parser, {}).valueForOption<int>("-n").has_value());
    BOOST_CHECK_EQUAL(must(ok(parser, {"-n", "7"}).valueForOption<int>("-n")), 7);

    // An undeclared option returns std::nullopt, not a zero.
    BOOST_CHECK(!ok(parser, {}).valueForOption<int>("-x").has_value());

    // An option given an empty value has a value, which is the empty string. The presence of a
    // token and the emptiness of a token are different conditions, and empty text cannot
    // represent both. The read therefore returns an optional rather than a view.
    Parser prefix(Command("prog").addOption(Option({"--prefix"}, "Prefix").arg("text")));
    for (const auto &given : {
             std::vector<std::string>{"prog", "--prefix="},
             std::vector<std::string>{"prog", "--prefix", ""}
    }) {
        auto result = prefix.parse(given);
        BOOST_REQUIRE_MESSAGE(result.isValid(), result.errorText());
        auto option = result.option("--prefix");
        BOOST_REQUIRE(option.has_value());
        BOOST_CHECK_EQUAL(must(option->value()), "");
        BOOST_CHECK_EQUAL(must(option->rawValue()), "");
        // The numeric read returns std::nullopt because an empty token is not a number.
        BOOST_CHECK(!option->value<int>().has_value());
    }

    // An option that is not given has no value, which must remain distinct from an empty value.
    // The result is stored in a variable rather than queried as a temporary because an
    // OptionResult owns nothing.
    auto empty = ok(prefix, {});
    BOOST_CHECK(!empty.option("--prefix").has_value());
    BOOST_CHECK(!empty.valueForOption("--prefix").has_value());
}

// An argument given an empty string has a value. No isSet() exists as an alternative check.
BOOST_AUTO_TEST_CASE(test_an_argument_given_an_empty_string_has_a_value) {
    Parser parser(Command("prog").addArgument(Argument("name").optional()));

    auto given = parser.parse({"prog", ""});
    BOOST_REQUIRE_MESSAGE(given.isValid(), given.errorText());
    BOOST_CHECK_EQUAL(must(given.value(0)), "");
    BOOST_CHECK_EQUAL(must(given.rawValue(0)), "");

    auto omitted = ok(parser, {});
    BOOST_CHECK(!omitted.rawValue(0).has_value());
    BOOST_CHECK(!omitted.value(0).has_value());

    // Because a default value is an ordinary token, the argument has a value.
    Parser defaulted(Command("prog").addArgument(Argument("name").optional().defaultValue("x")));
    BOOST_CHECK_EQUAL(must(ok(defaulted, {}).value(0)), "x");
}

BOOST_AUTO_TEST_CASE(test_a_tree_with_nothing_in_it) {
    // Every accessor returns a defined result rather than accessing out of bounds.
    Parser parser;
    BOOST_CHECK_EQUAL(parser.rootCommand().name(), "");

    auto result = parser.parse({});
    BOOST_CHECK(result.isValid());
    BOOST_REQUIRE(result.command() != nullptr);
    BOOST_CHECK(!result.rawValue(0).has_value());
    BOOST_CHECK(result.rawValues(0).empty());
    BOOST_CHECK(!result.option("-f").has_value());
    BOOST_CHECK(!result.valueForOption("-f").has_value());
    BOOST_CHECK(!result.isRoleSet(Option::Help));
    // NoRole is never set, regardless of the tree contents. Otherwise every option matches it.
    BOOST_CHECK(!result.isRoleSet(Option::NoRole));
    BOOST_CHECK_EQUAL(result.invoke(-1), -1);
    // An empty tree produces an empty help text. A block without contents is not printed, and a
    // nameless command without arguments, options or subcommands leaves every block empty,
    // including the usage line. Without this rule, the help text is a bare "Usage:" followed by
    // the absent name.
    BOOST_CHECK(result.helpText().empty());
    BOOST_CHECK(result.helpBlocks().empty());
}

BOOST_AUTO_TEST_CASE(test_a_command_with_no_name) {
    // Because no rule forbids an empty name, parsing and help generation must succeed.
    Parser parser(Command("").addArgument(Argument("path")));
    auto result = ok(parser, {"x"});
    BOOST_CHECK_EQUAL(must(result.value(0)), "x");
    BOOST_CHECK(has(result.helpText(), "Usage:"));
}

// ---------------------------------------------------------------------------------------------
// Program flow from argv to a handler
// ---------------------------------------------------------------------------------------------

// The library declares these two roles, defines their tokens and stores the version string.
// Therefore, the library handles them. If the caller handles them, a program written in the
// straightforward way runs the command for "prog copy --help" rather than describing it. On a
// destructive command, --help then performs the destructive operation.
BOOST_AUTO_TEST_CASE(test_invoke_answers_help_and_version_before_the_handler) {
    int ran = 0;
    const auto &tree = [&ran] {
        Parser parser(Command("prog", "What it is for")
                          .addVersionOption("1.2.3")
                          .addHelpOption(false, true)
                          .addCommand(Command("copy", "Copy things")
                                          .addArgument(Argument("src"))
                                          .setHandler([&ran](const ParseResult &) {
                                              ran++;
                                              return 7;
                                          })));
        parser.setTextWidth(80);
        return parser;
    };

    // The handler runs if neither help nor version is requested.
    {
        auto parser = tree();
        BOOST_CHECK_EQUAL(parser.invoke(argv({"copy", "x"})), 7);
        BOOST_CHECK_EQUAL(ran, 1);
    }

    // The handler does not run if help is requested. This is the primary property of the case.
    {
        ran = 0;
        auto parser = tree();
        int code = 0;
        auto printed = capturedStdout([&] { code = parser.invoke(argv({"copy", "--help"})); });
        BOOST_CHECK_EQUAL(code, 0);
        BOOST_CHECK_EQUAL(ran, 0);
        // The help text is that of the reached command, not that of the root.
        BOOST_CHECK(has(printed, "Usage:\n    prog copy"));
        BOOST_CHECK(has(printed, "Copy things"));
    }

    // The version request prints the version and does not run the handler either.
    {
        ran = 0;
        auto parser = tree();
        int code = 0;
        auto printed = capturedStdout([&] { code = parser.invoke(argv({"--version"})); });
        BOOST_CHECK_EQUAL(code, 0);
        BOOST_CHECK_EQUAL(ran, 0);
        BOOST_CHECK_EQUAL(printed, "1.2.3\n");
    }

    // Unlike addHelpOption(), addVersionOption() has no recursive flag. The version option
    // therefore belongs to the root only, unless the program author declares otherwise, and a
    // subcommand rejects it.
    {
        auto parser = tree();
        BOOST_CHECK(!parser.parse(argv({"copy", "x", "--version"})).isValid());
    }

    // If both are requested, help takes precedence because the help text describes the version
    // option.
    {
        auto parser = tree();
        auto printed = capturedStdout([&] { parser.invoke(argv({"--help", "--version"})); });
        BOOST_CHECK(has(printed, "Usage:"));
        BOOST_CHECK(!has(printed, "1.2.3\n"));
    }

    // invoke() reports a failed parse itself rather than leaving the detection to the caller.
    // The return value of invoke() is the return value of main, and no other place exists to
    // report the error.
    {
        ran = 0;
        auto parser = tree();
        int code = 0;
        auto complaint = capturedStderr([&] { code = parser.invoke(argv({"copy"})); });
        BOOST_CHECK_EQUAL(code, -1);
        BOOST_CHECK_EQUAL(ran, 0);
        BOOST_CHECK(has(complaint, "needs a value"));
        BOOST_CHECK(has(complaint, "Try \"prog copy --help\""));
    }

    // A command without a handler still handles a help request.
    {
        Parser bare(Command("prog", "What it is for").addHelpOption());
        bare.setTextWidth(80);
        BOOST_CHECK_EQUAL(bare.invoke(argv({})), -1);
        int code = 0;
        auto printed = capturedStdout([&] { code = bare.invoke(argv({"--help"})); });
        BOOST_CHECK_EQUAL(code, 0);
        BOOST_CHECK(has(printed, "Usage:"));
    }

    // Because a version option without a version string is not handled, the handler runs
    // normally.
    {
        ran = 0;
        Parser silent(Command("prog")
                          .addOption(Option(Option::Version))
                          .setHandler([&ran](const ParseResult &) {
                              ran++;
                              return 4;
                          }));
        BOOST_CHECK_EQUAL(silent.invoke(argv({"--version"})), 4);
        BOOST_CHECK_EQUAL(ran, 1);
    }
}

// The pre and post handlers of the commands on the path run around the handler of the command
// reached. A post handler runs only for a command whose pre handler succeeded, in the same way as
// a destructor runs only for an object whose constructor completed.
BOOST_AUTO_TEST_CASE(test_invoke_runs_the_pre_and_post_handlers_on_the_path) {
    std::vector<std::string> trace;
    // Each hook appends its name to the trace. A post handler returns the received value plus
    // ten. The final return value therefore records the number of post handlers run and that
    // each passed its result to the next.
    const auto &pre = [&trace](std::string name, int code) {
        return [&trace, name, code](const ParseResult &) {
            trace.push_back(name + " pre");
            return code;
        };
    };
    const auto &post = [&trace](std::string name) {
        return [&trace, name](const ParseResult &, int code) {
            trace.push_back(name + " post " + std::to_string(code));
            return code + 10;
        };
    };
    const auto &handler = [&trace](std::string name, int code) {
        return [&trace, name, code](const ParseResult &) {
            trace.push_back(name);
            return code;
        };
    };
    // The path is prog, sub and foo. The hooks of bar, a sibling of sub, must not run.
    const auto &tree = [&](int progPre, int subPre, int fooPre, int fooCode, bool subHasPre) {
        Command sub = Command("sub").setPostHandler(post("sub"));
        if (subHasPre) {
            sub.setPreHandler(pre("sub", subPre));
        }
        sub.addCommand(Command("foo")
                           .addHelpOption()
                           .setPreHandler(pre("foo", fooPre))
                           .setPostHandler(post("foo"))
                           .setHandler(handler("foo", fooCode)));
        return Parser(Command("prog")
                          .setPreHandler(pre("prog", progPre))
                          .setPostHandler(post("prog"))
                          .addCommand(std::move(sub))
                          .addCommand(Command("bar")
                                          .setPreHandler(pre("bar", 0))
                                          .setPostHandler(post("bar"))
                                          .setHandler(handler("bar", 0))));
    };
    const auto &ran = [&trace](std::vector<std::string> expected) {
        BOOST_CHECK_EQUAL_COLLECTIONS(trace.begin(), trace.end(), expected.begin(), expected.end());
        trace.clear();
    };

    // All succeed. The pre handlers run downward, then the handler, then the post handlers
    // upward.
    BOOST_CHECK_EQUAL(tree(0, 0, 0, 0, true).invoke(argv({"sub", "foo"})), 30);
    ran({"prog pre", "sub pre", "foo pre", "foo", "foo post 0", "sub post 10", "prog post 20"});

    // A failed pre handler skips the hooks and the handler below it. Only the commands above it
    // run their post handlers, and the first post handler receives the failure value.
    BOOST_CHECK_EQUAL(tree(0, 5, 0, 0, true).invoke(argv({"sub", "foo"})), 15);
    ran({"prog pre", "sub pre", "prog post 5"});

    // A failed pre handler of the root leaves no post handler to run.
    BOOST_CHECK_EQUAL(tree(5, 0, 0, 0, true).invoke(argv({"sub", "foo"})), 5);
    ran({"prog pre"});

    // A command without a pre handler counts as entered.
    BOOST_CHECK_EQUAL(tree(0, 0, 5, 0, false).invoke(argv({"sub", "foo"})), 25);
    ran({"prog pre", "foo pre", "sub post 5", "prog post 15"});

    // A failed handler still runs every post handler, each receiving the current value.
    BOOST_CHECK_EQUAL(tree(0, 0, 0, 3, true).invoke(argv({"sub", "foo"})), 33);
    ran({"prog pre", "sub pre", "foo pre", "foo", "foo post 3", "sub post 13", "prog post 23"});

    // No hook runs if the handler does not: on help, on a failed parse, and without a handler.
    {
        int code = -1;
        std::ignore = capturedStdout(
            [&] { code = tree(0, 0, 0, 0, true).invoke(argv({"sub", "foo", "--help"})); });
        BOOST_CHECK_EQUAL(code, 0);
        ran({});

        std::ignore = capturedStderr(
            [&] { code = tree(0, 0, 0, 0, true).invoke(argv({"sub", "foo", "--nope"}), -9); });
        BOOST_CHECK_EQUAL(code, -9);
        ran({});

        BOOST_CHECK_EQUAL(tree(0, 0, 0, 0, true).invoke(argv({"sub"}), -9), -9);
        ran({});
    }
}

// The version text is that of the innermost command on the path that has a version.
BOOST_AUTO_TEST_CASE(test_which_version_a_result_answers_with) {
    Parser parser(Command("prog")
                      .setVersion("1.0")
                      .addCommand(Command("build").setVersion("2.0").addCommand(Command("deep")))
                      .addCommand(Command("clean")));

    BOOST_CHECK_EQUAL(parser.parse(argv({})).versionText(), "1.0");
    // The version of a command takes precedence over the version of its parent.
    BOOST_CHECK_EQUAL(parser.parse(argv({"build"})).versionText(), "2.0");
    // A command without a version inherits the version of its parent.
    BOOST_CHECK_EQUAL(parser.parse(argv({"build", "deep"})).versionText(), "2.0");
    BOOST_CHECK_EQUAL(parser.parse(argv({"clean"})).versionText(), "1.0");

    // A tree without a version returns an empty text rather than a fabricated one.
    BOOST_CHECK(Parser(Command("prog")).parse(argv({})).versionText().empty());
}

// ---------------------------------------------------------------------------------------------
// Error reporting: corrections and printed output
// ---------------------------------------------------------------------------------------------

// A misspelled name is reported together with the similar declared names. Without corrections,
// the diagnostic for a mistyped subcommand states only that it is unknown, which is the least
// useful accurate message.
BOOST_AUTO_TEST_CASE(test_a_mistyped_name_is_answered_with_the_ones_it_is_near) {
    // a subcommand
    {
        Parser parser(Command("prog")
                          .addCommand(Command("copy"))
                          .addCommand(Command("move"))
                          .addCommand(Command("remove")));
        auto text = bad(parser, {"copyy"}, ParseResult::UnknownCommand).correctionText();
        BOOST_CHECK(has(text, "\"copyy\" is not matched"));
        BOOST_CHECK(has(text, "\n    copy"));
        BOOST_CHECK(!has(text, "\n  move"));
        BOOST_CHECK(!has(text, "\n  remove"));
    }

    // an option, matched against every token in scope rather than the first token only
    {
        Parser parser(Command("prog").addOptions({
            Option({"-v", "--verbose"}, "Say more"),
            Option({"--version"}, "Say which"),
        }));
        auto text = bad(parser, {"--verbse"}, ParseResult::UnknownOption).correctionText();
        BOOST_CHECK(has(text, "\n    --verbose"));
        BOOST_CHECK(has(text, "\n    --version"));
    }

    // one of the few expected values of an argument
    {
        Parser parser(
            Command("prog").addArgument(Argument("mode").expect({"fast", "slow", "careful"})));
        auto text = bad(parser, {"fest"}, ParseResult::InvalidArgumentValue).correctionText();
        BOOST_CHECK(has(text, "\n    fast"));
        BOOST_CHECK(!has(text, "\n  careful"));
    }

    // Without a similar name, the correction text is empty rather than the complete list.
    {
        Parser parser(Command("prog").addCommand(Command("copy")));
        auto text = bad(parser, {"zzzzzzzz"}, ParseResult::UnknownCommand).correctionText();
        BOOST_CHECK(text.empty());
    }

    // A failure other than a misspelled name has no correction.
    {
        Parser parser(Command("prog").addArgument(Argument("needed")));
        auto text = bad(parser, {}, ParseResult::MissingCommandArgument).correctionText();
        BOOST_CHECK(text.empty());
    }

    // A successful parse has no correction either.
    {
        Parser parser(Command("prog").addCommand(Command("copy")));
        BOOST_CHECK(ok(parser, {"copy"}).correctionText().empty());
    }
}

// The output of showError() on stderr, which is the purpose of computing the edit distance.
BOOST_AUTO_TEST_CASE(test_show_error_offers_the_correction) {
    Parser parser(Command("prog")
                      .addOption(Option(Option::Help))
                      .addCommand(Command("copy"))
                      .addCommand(Command("move")));

    auto result = bad(parser, {"copyy"}, ParseResult::UnknownCommand);
    auto printed = capturedStderr([&] { result.showError(); });
    BOOST_CHECK(has(printed, "is not a command"));
    BOOST_CHECK(has(printed, "Do you mean"));
    BOOST_CHECK(has(printed, "copy"));
    BOOST_CHECK(has(printed, "Try \"prog --help\""));

    // With corrections disabled, the error is still printed and only the suggestion is omitted.
    parser.setDisplayOptions(Parser::SkipCorrection);
    auto quiet = bad(parser, {"copyy"}, ParseResult::UnknownCommand);
    auto printed_quiet = capturedStderr([&] { quiet.showError(); });
    BOOST_CHECK(has(printed_quiet, "is not a command"));
    BOOST_CHECK(!has(printed_quiet, "Do you mean"));
    BOOST_CHECK(has(printed_quiet, "Try \"prog --help\""));
}

// The line that refers to help names the help token of this program rather than the conventional
// token. Without this rule, the line names --help regardless of the declared tree, and a tree
// without a help option still suggests one.
BOOST_AUTO_TEST_CASE(test_the_error_points_at_the_help_this_program_has) {
    const auto &printedFor = [](Command root) {
        Parser parser(std::move(root));
        auto result = parser.parse(argv({"nope"}));
        BOOST_REQUIRE(!result.isValid());
        return capturedStderr([&] { result.showError(); });
    };

    // The long token if one exists, because the long token is more readable.
    BOOST_CHECK(has(printedFor(Command("prog").addOption(Option(Option::Help))),
                    "Try \"prog --help\" for more information."));

    // A custom token is used if the default tokens are replaced.
    BOOST_CHECK(has(printedFor(Command("prog").addOption(
                        Option(Option::Help, {"-?", "--usage"}, "Say how to use this"))),
                    "Try \"prog --usage\" for more information."));
    BOOST_CHECK(has(
        printedFor(Command("prog").addOption(Option(Option::Help, {"-h"}, "Say how to use this"))),
        "Try \"prog -h\" for more information."));

    // A tree without a help option produces no suggestion.
    BOOST_CHECK(!has(printedFor(Command("prog")), "Try "));

    // An option with the token --help is not the help option without the Help role.
    BOOST_CHECK(
        !has(printedFor(Command("prog").addOption(Option({"--help"}, "Not the role"))), "Try "));

    // For a subcommand, the line names the subcommand and the inherited help option.
    Parser parser(Command("prog")
                      .addOption(Option(Option::Help).recursive())
                      .addCommand(Command("build").addArgument(Argument("target"))));
    auto result = parser.parse(argv({"build"}));
    BOOST_REQUIRE(!result.isValid());
    BOOST_CHECK(has(capturedStderr([&] { result.showError(); }),
                    "Try \"prog build --help\" for more information."));
}

// The overload that accepts the parameters of main, so that a caller does not have to build the
// vector.
BOOST_AUTO_TEST_CASE(test_parsing_from_argc_and_argv) {
    char arg0[] = "prog";
    char arg1[] = "-f";
    char arg2[] = "file.txt";
    char *args[] = {arg0, arg1, arg2};

    {
        Parser parser(
            Command("prog").addArgument(Argument("path")).addOption(Option({"-f"}, "Force")));
        auto result = parser.parse(3, args);
        BOOST_REQUIRE_MESSAGE(result.isValid(), result.errorText());
        BOOST_CHECK(result.option("-f").has_value());
        BOOST_CHECK_EQUAL(must(result.value(0)), "file.txt");
    }

    // The same overload of invoke(), which a typical main function calls.
    {
        std::string seen;
        Parser parser(Command("prog")
                          .addArgument(Argument("path"))
                          .addOption(Option({"-f"}, "Force"))
                          .setHandler([&seen](const ParseResult &result) {
                              seen = must(result.value(0));
                              return 7;
                          }));
        BOOST_CHECK_EQUAL(parser.invoke(3, args), 7);
        BOOST_CHECK_EQUAL(seen, "file.txt");
    }
}

namespace {

    // Returns the lines of the help text that contain the description of \a needle, the first
    // line and every continuation line. Leading blanks are preserved so that alignment can be
    // checked.
    std::vector<std::string> entryLines(const std::string &text, const std::string &needle) {
        std::vector<std::string> all;
        for (size_t start = 0; start <= text.size();) {
            auto end = text.find('\n', start);
            all.push_back(text.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }

        std::vector<std::string> res;
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].find(needle) == std::string::npos) {
                continue;
            }
            res.push_back(all[i]);
            // A continuation line contains only the description. Therefore, it starts after the
            // end of the left column.
            for (size_t j = i + 1; j < all.size(); ++j) {
                auto first = all[j].find_first_not_of(' ');
                if (first == std::string::npos || first <= all[i].find_first_not_of(' ')) {
                    break;
                }
                res.push_back(all[j]);
            }
            break;
        }
        return res;
    }

}

// ---------------------------------------------------------------------------------------------
// Line wrapping and wrap width
// ---------------------------------------------------------------------------------------------

// A description longer than the terminal width is wrapped rather than overflowing, and the wrap
// width is measured in columns.
BOOST_AUTO_TEST_CASE(test_a_long_description_is_wrapped) {
    const std::string sentence = "Overwrite whatever is already there, without asking first, "
                                 "which is what a script wants and a person rarely does";

    const auto &help = [&sentence](int width) {
        Parser parser(Command("prog").addOption(Option({"-f", "--force"}, sentence)));
        parser.setTextWidth(width);
        return parser.parse({"prog"}).helpText();
    };

    // Because the width accommodates the whole description, no line break occurs.
    {
        auto lines = entryLines(help(200), "--force");
        BOOST_REQUIRE_EQUAL(lines.size(), 1u);
        BOOST_CHECK(has(lines[0], sentence));
    }

    // The width requires line breaks, and no line may exceed the width.
    {
        auto lines = entryLines(help(60), "--force");
        BOOST_REQUIRE_GT(lines.size(), 1u);
        for (const auto &line : lines) {
            BOOST_CHECK_MESSAGE(stdc::console::display_width(line) <= 60,
                                "line runs past the width: [" + line + "]");
        }
    }

    // A narrower width produces more lines. This property indicates that the configured width
    // is used rather than a constant.
    BOOST_CHECK_GT(entryLines(help(40), "--force").size(), entryLines(help(60), "--force").size());

    // The continuation lines start under the description, not under the option.
    {
        auto lines = entryLines(help(60), "--force");
        BOOST_REQUIRE_GT(lines.size(), 1u);
        auto column = lines[0].find("Overwrite");
        BOOST_REQUIRE(column != std::string::npos);
        for (size_t i = 1; i < lines.size(); ++i) {
            BOOST_CHECK_EQUAL(lines[i].find_first_not_of(' '), column);
        }
    }

    // Words remain whole. No line ends inside a word if a space is available.
    {
        auto lines = entryLines(help(60), "--force");
        std::string rejoined;
        for (const auto &line : lines) {
            auto first = line.find_first_not_of(' ');
            rejoined += (rejoined.empty() ? "" : " ") + line.substr(first);
        }
        BOOST_CHECK(has(rejoined, sentence));
    }
}

// Edge cases of wrapping beyond the ordinary case.
BOOST_AUTO_TEST_CASE(test_wrapping_edges) {
    const auto &help = [](const std::string &description, int width) {
        Parser parser(Command("prog").addOption(Option({"-x"}, description)));
        parser.setTextWidth(width);
        return parser.parse({"prog"}).helpText();
    };

    // A single word wider than the column contains no space for a break. It is therefore broken
    // at the column edge rather than allowed to overflow.
    {
        std::string word(120, 'w');
        auto lines = entryLines(help(word, 50), "-x");
        BOOST_REQUIRE_GT(lines.size(), 1u);
        for (const auto &line : lines) {
            BOOST_CHECK(stdc::console::display_width(line) <= 50);
        }
    }

    // Newlines written by the caller are preserved.
    {
        auto lines = entryLines(help("first\nsecond\nthird", 200), "-x");
        BOOST_REQUIRE_EQUAL(lines.size(), 3u);
        BOOST_CHECK(has(lines[0], "first"));
        BOOST_CHECK(has(lines[1], "second"));
        BOOST_CHECK(has(lines[2], "third"));
    }

    // A width narrower than the left column leaves no width for the description. The result is
    // nevertheless a readable column rather than one character per line. The text is still
    // wrapped. Writing one long line is the alternative fallback, and that behavior is not
    // intended.
    {
        auto lines =
            entryLines(help("a description of some length here that will not fit", 4), "-x");
        BOOST_REQUIRE_GT(lines.size(), 1u);
        for (const auto &line : lines) {
            auto first = line.find_first_not_of(' ');
            BOOST_CHECK_GT(line.size() - first, 1u);
        }
    }

    // An empty description.
    {
        auto lines = entryLines(help("", 60), "-x");
        BOOST_REQUIRE_EQUAL(lines.size(), 1u);
    }
}

// Non-ASCII text is measured in display columns, not in bytes.
BOOST_AUTO_TEST_CASE(test_wrapping_counts_columns_not_bytes) {
    // Twenty CJK characters: sixty bytes, forty columns.
    std::string cjk;
    for (int i = 0; i < 20; i++) {
        cjk += "\xe4\xb8\xad";
    }
    BOOST_REQUIRE_EQUAL(cjk.size(), 60u);
    BOOST_REQUIRE_EQUAL(stdc::console::display_width(cjk), 40);

    Parser parser(Command("prog").addOption(Option({"-x"}, cjk)));
    parser.setTextWidth(40);
    auto lines = entryLines(parser.parse({"prog"}).helpText(), "-x");

    BOOST_REQUIRE_GT(lines.size(), 1u);
    for (const auto &line : lines) {
        // Measured in columns. If bytes are counted, each line holds three times the text that
        // it can display.
        BOOST_CHECK_MESSAGE(stdc::console::display_width(line) <= 40,
                            "line is " + std::to_string(stdc::console::display_width(line)) +
                                " columns wide");
        // A line never splits a character, which produces an invalid byte sequence in the
        // output.
        BOOST_CHECK_EQUAL((line.size() - line.find_first_not_of(' ')) % 3, 0u);
    }
}

// ---------------------------------------------------------------------------------------------
// Subcommands and inherited options
// ---------------------------------------------------------------------------------------------

// A subcommand inherits the recursive options of every command above it, and a command line that
// omits a required inherited option is rejected. Its help text must therefore list these options.
// If the help text lists only the options that the command declares, the error "option \"-C\" is
// required" names an option that the help text does not mention.
BOOST_AUTO_TEST_CASE(test_a_subcommand_lists_what_it_inherited) {
    const auto &tree = [] {
        Parser parser(Command("prog")
                          .addOptions({
                              Option({"-v", "--verbose"}, "Say more").recursive(),
                              Option({"-C"}, "Work here").arg("dir").recursive().required(),
                              Option({"--local"}, "Root only"),
                          })
                          .addCommand(Command("build", "Build it")
                                          .addArgument(Argument("target"))
                                          .addOption(Option({"-j"}, "Jobs").arg("n"))));
        parser.setTextWidth(80);
        return parser;
    };

    auto parser = tree();
    auto text = parser.parse(argv({"build", "x"})).helpText();

    // A separate heading, because these options belong to the program rather than to this
    // command.
    BOOST_CHECK(has(text, "Global options:"));
    BOOST_CHECK(at(text, "Options:") < at(text, "Global options:"));
    BOOST_CHECK(has(text, "-v, --verbose"));
    BOOST_CHECK(has(text, "-C <dir>"));

    // The options declared by the command remain under the regular heading.
    BOOST_CHECK(has(text, "-j <n>"));

    // A non-recursive option of the root is not in scope and is not listed.
    BOOST_CHECK(!has(text, "--local"));

    // The required inherited option appears on the usage line, as a required declared option
    // does.
    BOOST_CHECK(has(text, "Usage:\n    prog build -C <dir> [options] <target>"));

    // The help text and the parser behavior must agree.
    auto refused = parser.parse(argv({"build", "x"}));
    BOOST_REQUIRE(!refused.isValid());
    BOOST_CHECK(has(refused.errorText(), "-C"));
    // The option is written after the reached command, as every option is.
    BOOST_CHECK(ok(parser, {"build", "-C", "somewhere", "x"}).option("-C").has_value());

    // Because the root has no parent, its help text has no such section.
    BOOST_CHECK(!has(parser.parse(argv({})).helpText(), "Global options:"));
}

// A recursive option is valid wherever it is in scope. The command line descends through three
// commands, and each level declares one option. The option may be written after every command
// below the declaring command, and it is not in scope at the commands above the declaring
// command.
BOOST_AUTO_TEST_CASE(test_recursive_options_are_written_after_the_command_that_was_reached) {
    const auto &tree = [] {
        Parser parser(
            Command("prog")
                .addOption(Option({"--root-wide"}, "From the top").recursive())
                .addCommand(Command("remote")
                                .addOption(Option({"--mid"}, "From the middle").recursive())
                                .addCommand(Command("add")
                                                .addOption(Option({"--leaf"}, "Here"))
                                                .addArgument(Argument("name")))));
        parser.setTextWidth(80);
        return parser;
    };

    // Every option follows the reached command, which is the only valid position for any option.
    // Two levels of recursive options and the option of the leaf appear together.
    auto parser = tree();
    auto trailing = ok(parser, {"remote", "add", "--root-wide", "--mid", "--leaf", "x"});
    BOOST_CHECK(names(trailing.commandPath()) ==
                std::vector<std::string>({"prog", "remote", "add"}));
    BOOST_CHECK(trailing.option("--root-wide").has_value());
    BOOST_CHECK(trailing.option("--mid").has_value());
    BOOST_CHECK(trailing.option("--leaf").has_value());
    BOOST_CHECK_EQUAL(must(trailing.value(0)), "x");

    // The options may appear in any order because their position after the command has no
    // meaning.
    for (const auto &line : {
             std::vector<std::string>{"remote", "add", "--mid",  "--leaf",      "--root-wide", "x"     },
             std::vector<std::string>{"remote", "add", "--leaf", "x",           "--root-wide", "--mid" },
             std::vector<std::string>{"remote", "add", "x",      "--root-wide", "--mid",       "--leaf"},
    }) {
        std::vector<std::string> args{"prog"};
        args.insert(args.end(), line.begin(), line.end());
        auto result = parser.parse(args);
        BOOST_REQUIRE_MESSAGE(result.isValid(), result.errorText());
        BOOST_CHECK(result.option("--root-wide").has_value());
        BOOST_CHECK(result.option("--mid").has_value());
        BOOST_CHECK(result.option("--leaf").has_value());
        BOOST_CHECK_EQUAL(must(result.value(0)), "x");
    }

    // The command path consists of consecutive command names only. Any option ends the path,
    // regardless of the command that declares the option. A following name is therefore not a
    // command and is reported as an unknown command.
    bad(parser, {"--root-wide", "remote", "add", "--leaf", "x"}, ParseResult::UnknownCommand);
    bad(parser, {"remote", "--mid", "add", "--leaf", "x"}, ParseResult::UnknownCommand);

    // Written above the declaring command, the token matches no option. This rule allows a
    // subcommand to give a token a different meaning than its parent gives it.
    bad(parser, {"--mid", "remote", "add", "x"}, ParseResult::UnknownOption);
    bad(parser, {"remote", "--leaf", "add", "x"}, ParseResult::UnknownOption);

    // A second occurrence is rejected unless the option is declared as repeatable. Being
    // recursive does not permit additional occurrences.
    bad(parser, {"remote", "add", "--root-wide", "--root-wide", "x"},
        ParseResult::OptionOccurTooMuch);

    // If the option is declared as repeatable, both occurrences are counted.
    Parser repeatable(Command("prog")
                          .addOption(Option({"-v"}, "Say more").recursive().multi())
                          .addCommand(Command("remote").addCommand(
                              Command("add").addArgument(Argument("name")))));
    auto twice = ok(repeatable, {"remote", "add", "-v", "-v", "x"});
    BOOST_REQUIRE(twice.option("-v").has_value());
    BOOST_CHECK_EQUAL(twice.option("-v")->count(), 2);

    // Recursive options from both levels are listed under the heading for inherited options.
    auto text = parser.parse(argv({"remote", "add", "x"})).helpText();
    BOOST_CHECK(has(text, "Global options:"));
    BOOST_CHECK(has(text, "--root-wide"));
    BOOST_CHECK(has(text, "--mid"));
    BOOST_CHECK(has(text, "Usage:\n    prog remote add"));
    // The option declared by the command remains under the regular heading.
    BOOST_CHECK(at(text, "Options:") < at(text, "Global options:"));
    BOOST_CHECK(has(text, "--leaf"));
}

// A response file is expanded before command lookup, so that it may contain a subcommand name.
BOOST_AUTO_TEST_CASE(test_a_response_file_may_name_a_subcommand) {
    auto path = std::filesystem::temp_directory_path() / "stdc_cli_response_nested.txt";
    {
        std::ofstream file(path, std::ios::binary);
        file << "build\n-j\n4\ntarget\n";
    }

    Parser parser(Command("prog").addCommand(Command("build")
                                                 .addArgument(Argument("target"))
                                                 .addOption(Option({"-j"}, "Jobs").arg("n"))));
    auto result = ok(parser, {"@" + path.string()}, Parser::EnableResponseFile);
    BOOST_CHECK(names(result.commandPath()) == std::vector<std::string>({"prog", "build"}));
    BOOST_CHECK_EQUAL(must(result.value(0)), "target");
    BOOST_CHECK_EQUAL(must(result.valueForOption<int>("-j")), 4);

    std::filesystem::remove(path);
}

// A command inherits from every ancestor, not only from its parent.
BOOST_AUTO_TEST_CASE(test_globals_reach_a_grandchild) {
    Parser parser(
        Command("prog")
            .addOption(Option({"--root-wide"}, "From the top").recursive())
            .addCommand(Command("remote", "Remotes")
                            .addOption(Option({"--mid"}, "From the middle").recursive())
                            .addOption(Option({"--mid-local"}, "Not inherited"))
                            .addCommand(Command("add", "Add one").addArgument(Argument("name")))));
    parser.setTextWidth(80);

    auto text = parser.parse(argv({"remote", "add", "x"})).helpText();
    BOOST_CHECK(has(text, "Global options:"));
    BOOST_CHECK(has(text, "--root-wide"));
    BOOST_CHECK(has(text, "--mid"));
    BOOST_CHECK(!has(text, "--mid-local"));

    // The help text of the middle command lists the recursive option of the root and the own
    // option of the middle command, which is not recursive.
    auto middle = parser.parse(argv({"remote"})).helpText();
    BOOST_CHECK(has(middle, "--root-wide"));
    BOOST_CHECK(has(middle, "--mid-local"));
}

// The usage line is wrapped like every other block, and each of its lines starts at the indent of
// its section rather than at the margin.
BOOST_AUTO_TEST_CASE(test_the_usage_line_is_wrapped) {
    const auto &usageLines = [](int width) {
        Parser parser(Command("program")
                          .addOption(Option({"--output"}, "Out").arg("file").required())
                          .addOption(Option({"--config"}, "Config").arg("path").required())
                          .addOption(Option({"--target"}, "Target").arg("triple").required())
                          .addOption(Option({"-v"}, "Loud"))
                          .addArguments({Argument("source"), Argument("destination")}));
        parser.setTextWidth(width);
        auto text = parser.parse({"program"}).helpText();

        // The section body, which consists of every indented line under the heading.
        std::vector<std::string> res;
        auto heading = text.find("Usage:\n");
        for (size_t start = heading == std::string::npos ? heading : heading + 7;
             start != std::string::npos && text.compare(start, 4, "    ") == 0;) {
            auto end = text.find('\n', start);
            res.push_back(text.substr(start, end == std::string::npos ? end : end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return res;
    };

    // The width accommodates the whole usage line.
    {
        auto lines = usageLines(200);
        BOOST_REQUIRE_EQUAL(lines.size(), 1u);
        BOOST_CHECK(has(lines[0], "--output <file>"));
        BOOST_CHECK(has(lines[0], "<destination>"));
    }

    // Because the width is insufficient, the line breaks and no line exceeds the width.
    {
        auto lines = usageLines(50);
        BOOST_REQUIRE_GT(lines.size(), 1u);
        for (const auto &line : lines) {
            BOOST_CHECK_MESSAGE(stdc::console::display_width(line) <= 50,
                                "usage line runs past the width: [" + line + "]");
        }

        // Every line starts at the indent, so that the block reads as one unit.
        for (const auto &line : lines) {
            BOOST_CHECK_EQUAL(line.find_first_not_of(' '), 4u);
        }
    }

    // Because an option and its value form one unit, a break never falls between them. The check
    // covers a range of widths because a single width places the break in one position only, and
    // at most widths the two parts remain together by chance.
    for (int width = 28; width <= 70; width++) {
        for (const auto &line : usageLines(width)) {
            for (const auto &pair : {std::make_pair("--output", "--output <file>"),
                                     std::make_pair("--config", "--config <path>"),
                                     std::make_pair("--target", "--target <triple>")}) {
                if (line.find(pair.first) == std::string::npos) {
                    continue;
                }
                BOOST_CHECK_MESSAGE(has(line, pair.second),
                                    std::string(pair.first) + " was split at width " +
                                        std::to_string(width) + ": [" + line + "]");
            }
        }
    }

    // Every unit is preserved regardless of the line breaks.
    {
        std::string joined;
        for (const auto &line : usageLines(40)) {
            auto first = line.find_first_not_of(' ');
            joined += (joined.empty() ? "" : " ") + line.substr(first);
        }
        for (const auto *piece : {"--output <file>", "--config <path>", "--target <triple>",
                                  "[options]", "<source>", "<destination>"}) {
            BOOST_CHECK_MESSAGE(has(joined, piece), std::string("lost ") + piece);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Layout: block selection and order
// ---------------------------------------------------------------------------------------------

// The description is a section like every other section. It therefore has a heading, and its
// body is indented under the heading. Without a heading, the description follows the prologue
// directly and the boundary between the two is not visible.
BOOST_AUTO_TEST_CASE(test_the_description_is_a_section_of_its_own) {
    Parser parser(Command("prog", "What the program is for"));
    parser.setPrologue("A prologue line");
    parser.setTextWidth(80);
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(has(text, "Description:\n    What the program is for\n"));
    // The prologue is not a section and starts at the margin, because a banner belongs at its
    // written position rather than under an unrequested heading.
    BOOST_CHECK(has(text, "A prologue line\n"));
    BOOST_CHECK(!has(text, "    A prologue line"));

    // A command without a description produces no heading.
    Parser bare(Command("prog"));
    BOOST_CHECK(!has(bare.parse(argv({})).helpText(), "Description:"));
}

// The program author chooses the indent of a section body and the spacing between the two
// columns of a list. A downstream project can retain an indent of four, and a project that
// requires a tighter layout can configure one.
BOOST_AUTO_TEST_CASE(test_the_indent_and_the_spacing_can_be_changed) {
    const auto &helpAt = [](int indent, int spacing, int width) {
        Parser parser(Command("prog", "What the program is for")
                          .addOption(Option({"-v"}, "Say more"))
                          .addOption(Option({"--output"}, "Where to write it, at whatever length "
                                                          "it takes to say so")
                                         .arg("file")));
        parser.setTextWidth(width);
        parser.setIndent(indent);
        parser.setSpacing(spacing);
        return parser.parse(argv({})).helpText();
    };

    // The widest name in the block, which determines the alignment of the descriptions.
    const size_t widest = std::string("--output <file>").size();

    // An indent of four and a spacing of four, which are the defaults of a parser.
    {
        auto text = helpAt(4, 4, 80);
        BOOST_CHECK(has(text, "\n    What the program is for\n"));
        BOOST_CHECK(has(text, "\n    -v "));
        BOOST_CHECK_EQUAL(descriptionColumn(text, "-v"), 4u + widest + 4u);
    }

    // Both values changed. Each value affects only the intended dimension.
    {
        auto text = helpAt(2, 1, 80);
        BOOST_CHECK(has(text, "\n  What the program is for\n"));
        BOOST_CHECK(has(text, "\n  -v "));
        BOOST_CHECK_EQUAL(descriptionColumn(text, "-v"), 2u + widest + 1u);
    }

    // One value changed and the other unchanged, so that neither value substitutes for the other.
    BOOST_CHECK_EQUAL(descriptionColumn(helpAt(8, 4, 80), "-v"), 8u + widest + 4u);
    BOOST_CHECK_EQUAL(descriptionColumn(helpAt(4, 9, 80), "-v"), 4u + widest + 9u);

    // A wrapped description continues at its starting column, regardless of that column.
    {
        auto text = helpAt(6, 3, 60);
        auto start = text.find("Where to write it");
        BOOST_REQUIRE(start != std::string::npos);
        auto next = text.find('\n', start) + 1;
        BOOST_REQUIRE(next != 0u);
        BOOST_CHECK_EQUAL(text.find_first_not_of(' ', next) - next, 6u + widest + 3u);
    }
}

// The layout determines the blocks of the help text and their order.
BOOST_AUTO_TEST_CASE(test_a_layout_says_which_blocks_and_in_what_order) {
    auto parser = helpTree();

    // Blocks are printed in the order in which they are added.
    parser.setHelpLayout(
        HelpLayout().add(HelpBlock::Options).add(HelpBlock::Usage).add(HelpBlock::Description));
    auto text = parser.parse(argv({})).helpText();
    BOOST_CHECK(at(text, "Options:") < at(text, "Usage:"));
    BOOST_CHECK(at(text, "Usage:") < at(text, "Description:"));

    // A block absent from the layout is not printed. A program omits an unneeded block in this
    // way rather than by clearing the data of that block.
    BOOST_CHECK(!has(text, "A prologue line"));
    BOOST_CHECK(!has(text, "An epilogue line"));
    BOOST_CHECK(!has(text, "Filesystem Commands:"));

    // A block added twice is printed twice.
    parser.setHelpLayout(HelpLayout().add(HelpBlock::Description).add(HelpBlock::Description));
    auto twice = parser.parse(argv({})).helpText();
    size_t count = 0;
    for (size_t at_ = twice.find("Description:"); at_ != std::string::npos;
         at_ = twice.find("Description:", at_ + 1)) {
        count++;
    }
    BOOST_CHECK_EQUAL(count, 2u);

    // An empty layout produces an empty help text.
    parser.setHelpLayout(HelpLayout());
    BOOST_CHECK(parser.parse(argv({})).helpText().empty());
}

// A block defined by the program passes through the same layout as the built-in blocks. This is
// the reason for defining it as a block rather than printing it after the help text.
BOOST_AUTO_TEST_CASE(test_a_layout_carries_blocks_of_the_programs_own) {
    Parser parser(Command("prog").addOption(Option({"--output"}, "Where to write").arg("file")));
    parser.setTextWidth(80);

    HelpBlock examples;
    examples.title = "Examples";
    examples.text = "prog --output out.txt";

    HelpBlock environment;
    environment.title = "Environment";
    environment.entries = {
        {"PROG_HOME", "Where it looks for its data"}
    };

    parser.setHelpLayout(HelpLayout().add(HelpBlock::Options).add(environment).add(examples));
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(has(text, "Environment:\n    PROG_HOME    Where it looks for its data\n"));
    BOOST_CHECK(has(text, "Examples:\n    prog --output out.txt\n"));
    BOOST_CHECK(at(text, "Options:") < at(text, "Environment:"));

    // Because the block is a list, it is measured with the other lists if the catalogues are
    // aligned.
    parser.setDisplayOptions(Parser::AlignAllCatalogues);
    auto aligned = parser.parse(argv({})).helpText();
    BOOST_CHECK_EQUAL(descriptionColumn(aligned, "--output <file>"),
                      descriptionColumn(aligned, "PROG_HOME"));

    // An empty custom block is omitted like any other empty block. A block filled from an empty
    // source therefore leaves no heading.
    parser.setHelpLayout(HelpLayout().add(HelpBlock()).add(HelpBlock::Options));
    BOOST_CHECK(!has(parser.parse(argv({})).helpText(), "::"));
}

// The components of the help text before layout. A program that requires an output the layout
// cannot express retrieves these blocks and prints them itself.
BOOST_AUTO_TEST_CASE(test_help_blocks_are_what_the_text_is_made_of) {
    auto blocks = helpTree().parse(argv({})).helpBlocks();
    BOOST_REQUIRE(!blocks.empty());

    const auto &find = [&blocks](HelpBlock::Role role,
                                 const std::string &title) -> const HelpBlock * {
        for (const auto &block : blocks) {
            if (block.role == role && block.title == title) {
                return &block;
            }
        }
        return nullptr;
    };

    // The blocks follow the layout order. The prologue and the epilogue are at the ends, and
    // neither has a heading.
    BOOST_CHECK(blocks.front().role == HelpBlock::Prologue);
    BOOST_CHECK_EQUAL(blocks.front().title, "");
    BOOST_CHECK_EQUAL(blocks.front().text, "A prologue line");
    BOOST_CHECK(blocks.back().role == HelpBlock::Epilogue);
    BOOST_CHECK_EQUAL(blocks.back().text, "An epilogue line");

    // The usage line, already wrapped to the width but without the indent.
    auto usage = find(HelpBlock::Usage, "Usage");
    BOOST_REQUIRE(usage != nullptr);
    BOOST_CHECK_EQUAL(usage->text, "prog [commands] [options]");
    BOOST_CHECK(usage->entries.empty());

    // Each group of a catalogue is a separate block with the role of its source.
    BOOST_CHECK(find(HelpBlock::Commands, "Filesystem Commands") != nullptr);
    BOOST_CHECK(find(HelpBlock::Commands, "Buildsystem Commands") != nullptr);
    BOOST_CHECK(find(HelpBlock::Commands, "Commands") != nullptr);

    // Because the root has no positional arguments, no Arguments block exists.
    BOOST_CHECK(find(HelpBlock::Arguments, "Arguments") == nullptr);

    // The two columns without the alignment padding.
    auto options = find(HelpBlock::Options, "Options");
    BOOST_REQUIRE(options != nullptr);
    BOOST_REQUIRE(!options->entries.empty());
    BOOST_CHECK(options->text.empty());
    BOOST_CHECK_EQUAL(options->entries.front().left, "-h, --help");
}

// A style specifies the presentation of a block, not its content. helpText() returns the text,
// and showHelp() adds the escape sequences around it.
BOOST_AUTO_TEST_CASE(test_styling_is_in_what_is_printed_and_not_in_the_text) {
    Parser parser(Command("prog", "What the program is for").addOption(Option({"-v"}, "Say more")));
    parser.setEpilogue("An epilogue line");
    parser.setTextWidth(80);

    auto plain = parser.parse(argv({})).helpText();

    auto layout = HelpLayout::defaultLayout();
    layout.setTitleStyle({stdc::console::bold});
    layout.setEntryStyle({stdc::console::nostyle, stdc::console::cyan});
    layout.setBodyStyle(HelpBlock::Epilogue, {stdc::console::bold, stdc::console::yellow});
    parser.setHelpLayout(layout);

    // The text is identical because styles add no text.
    BOOST_CHECK_EQUAL(parser.parse(argv({})).helpText(), plain);

    // The printed output contains the styles. The color mode is forced because the capture
    // writes to a file. Because a file is never a terminal, per-target detection correctly
    // produces plain output.
    auto saved = stdc::console::get_color_mode();
    stdc::console::set_color_mode(stdc::console::vt);
    auto printed = capturedStdout([&] { parser.parse(argv({})).showHelp(); });
    stdc::console::set_color_mode(saved);

    BOOST_CHECK(has(printed, "\033[1mOptions:\033[0m"));
    BOOST_CHECK(has(printed, "\033[36m-v\033[0m"));
    BOOST_CHECK(has(printed, "\033[33;1mAn epilogue line\033[0m"));

    // Because the newline of a heading is outside its styling, no attribute set by an escape
    // sequence remains active at the start of the next line. A background color extending to the
    // edge of the terminal makes this defect visible.
    BOOST_CHECK(!has(printed, ":\n\033[0m"));

    // Without the escape sequences, the output equals the text. Therefore, the styling moves no
    // text.
    std::string stripped;
    for (size_t i = 0; i < printed.size(); ++i) {
        if (printed[i] == '\033') {
            i = printed.find('m', i);
            BOOST_REQUIRE(i != std::string::npos);
            continue;
        }
        stripped += printed[i];
    }
    BOOST_CHECK_EQUAL(stripped, plain);
}

namespace {

    /// Returns a tree with one instance of every element that the levels below affect, so that a
    /// case can determine which elements changed and which did not.
    Parser formatterTree() {
        Parser parser(
            Command("prog", "What the program is for")
                .addArgument(Argument("path", "Where to work"))
                .addOption(Option({"-o", "--output"}, "Where to write").arg("file").required())
                .addOption(Option({"-v"}, "Say more")));
        parser.setEpilogue("An epilogue line");
        parser.setTextWidth(80);
        return parser;
    }

    /// Overrides level 1, the display form of a metavar, which both the usage line and every
    /// list request.
    struct Braces : HelpFormatter {
        std::string displayed(const Argument &argument) const override {
            return "{" + argument.displayName() + "}";
        }
    };

}

// ---------------------------------------------------------------------------------------------
// HelpFormatter levels and their overrides
// ---------------------------------------------------------------------------------------------

// Overriding only the bottom level affects every place where a name is written, including the
// option level, which formats its arguments through this level rather than through the base
// implementation directly.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_change_how_a_name_is_spelled) {
    auto parser = formatterTree();
    parser.setHelpFormatter(std::make_shared<Braces>());
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(has(text, "Usage:\n    prog -o {file} [options] {path}"));
    BOOST_CHECK(has(text, "-o, --output {file}"));
    BOOST_CHECK(has(text, "{path}    Where to work"));
    BOOST_CHECK(!has(text, "<"));

    // Because the level above is not overridden, the option tokens are listed in their default
    // form.
    BOOST_CHECK(has(text, "-o, --output"));
}

// The next level up, overridden as an extension of the base implementation rather than as a
// replacement.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_change_how_an_option_is_written) {
    struct Joined : HelpFormatter {
        std::string displayed(const Option &option, bool allSpellings) const override {
            auto res = HelpFormatter::displayed(option, allSpellings);
            auto at = res.find(" <");
            if (at != std::string::npos) {
                res.replace(at, 1, "=");
            }
            return res;
        }
    };

    auto parser = formatterTree();
    parser.setHelpFormatter(std::make_shared<Joined>());
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(has(text, "-o, --output=<file>"));
    BOOST_CHECK(has(text, "Usage:\n    prog -o=<file> [options] <path>"));
    // A positional argument is not an option and retains its default form.
    BOOST_CHECK(has(text, "<path>    Where to work"));
}

// The middle level, which defines the composition of the text. This single hook covers the
// functionality for which CLI11 requires eight overridable methods, because it returns data
// rather than eight pieces of finished text.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_change_what_the_blocks_hold) {
    struct Shouty : HelpFormatter {
        std::vector<HelpBlock> blocks(const ParseResult &result,
                                      const HelpSizes &sizes) const override {
            auto res = HelpFormatter::blocks(result, sizes);
            for (auto &block : res) {
                for (auto &c : block.title) {
                    c = c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
                }
            }
            HelpBlock seeAlso;
            seeAlso.title = "SEE ALSO";
            seeAlso.text = "prog(1)";
            res.push_back(std::move(seeAlso));
            return res;
        }
    };

    auto parser = formatterTree();
    parser.setHelpFormatter(std::make_shared<Shouty>());
    auto result = parser.parse(argv({}));
    auto text = result.helpText();

    BOOST_CHECK(has(text, "OPTIONS:"));
    BOOST_CHECK(!has(text, "Options:"));
    BOOST_CHECK(has(text, "SEE ALSO:\n    prog(1)\n"));
    // The added block is laid out like any other block. This is the reason for adding it here
    // rather than printing it after the help text.
    BOOST_CHECK(has(text, "An epilogue line\n\nSEE ALSO:"));

    // helpBlocks() returns the blocks produced by the formatter rather than the blocks requested
    // by the layout, so that both ways of obtaining the blocks agree.
    BOOST_CHECK_EQUAL(result.helpBlocks().back().title, "SEE ALSO");
}

// One block laid out differently while the other blocks remain unchanged. This is the purpose of
// a base implementation that is callable per block.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_lay_one_block_out_differently) {
    struct BareOptions : HelpFormatter {
        std::vector<Run> renderBlock(const HelpBlock &block, const HelpSizes &sizes,
                                     size_t widest) const override {
            if (block.role != HelpBlock::Options) {
                return HelpFormatter::renderBlock(block, sizes, widest);
            }
            std::vector<Run> res;
            for (const auto &entry : block.entries) {
                res.push_back({block.entryStyle, entry.left + ";"});
            }
            res.push_back({{}, "\n"});
            return res;
        }
    };

    auto parser = formatterTree();
    parser.setHelpFormatter(std::make_shared<BareOptions>());
    auto text = parser.parse(argv({})).helpText();

    BOOST_CHECK(has(text, "-o, --output <file>;-v;\n"));
    BOOST_CHECK(!has(text, "Options:"));
    // Every other block is unchanged, including its heading and indent.
    BOOST_CHECK(has(text, "Description:\n    What the program is for\n"));
    BOOST_CHECK(has(text, "Usage:\n    prog -o <file> [options] <path>\n"));
    BOOST_CHECK(has(text, "Arguments:\n    <path>    Where to work\n"));
}

// The top level, which renders the whole page. This override reuses none of the base
// implementation.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_lay_the_whole_page_out) {
    struct Terse : HelpFormatter {
        std::vector<Run> render(const std::vector<HelpBlock> &blocks,
                                const HelpSizes &) const override {
            std::vector<Run> res;
            for (const auto &block : blocks) {
                res.push_back({{},
                               std::to_string(int(block.role)) + ":" +
                                   std::to_string(block.entries.size()) + " "});
            }
            return res;
        }
    };

    auto parser = formatterTree();
    parser.setHelpFormatter(std::make_shared<Terse>());
    // Description, usage, arguments, options and epilogue, each with its entry count.
    BOOST_CHECK_EQUAL(parser.parse(argv({})).helpText(), "1:0 2:0 3:1 4:2 7:0 ");
}

// The usage line in isolation, and its parameters. The command provides its own options,
// arguments and subcommands. The only additional parameter is therefore the set of options that
// the ancestors leave in scope, which is not accessible from the command.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_change_the_usage_line) {
    struct Synopsis : HelpFormatter {
        mutable std::vector<std::string> inherited;
        std::string usageText(const Command &command, const std::vector<const Command *> &path,
                              const std::vector<Option> &from_above,
                              const HelpSizes &sizes) const override {
            inherited.clear();
            for (const auto &option : from_above) {
                inherited.push_back(option.token());
            }
            return "SYNOPSIS " + std::to_string(path.size()) + "\n" +
                   HelpFormatter::usageText(command, path, from_above, sizes);
        }
    };

    auto formatter = std::make_shared<Synopsis>();
    auto parser = helpTree();
    parser.setHelpFormatter(formatter);

    // At the root, the path contains only the program and no option is inherited.
    auto text = parser.parse(argv({})).helpText();
    BOOST_CHECK(has(text, "SYNOPSIS 1\n"));
    // Both lines pass through the base implementation and both have the indent of the block.
    BOOST_CHECK(has(text, "prog [commands] [options]"));
    BOOST_CHECK(formatter->inherited.empty());

    // One level down, the path has two names, and the recursive option of the root is in scope.
    // The usage line still contains the base output, which indicates that the override calls the
    // base implementation rather than replacing it.
    text = parser.parse(argv({"copy", "a", "b"})).helpText();
    BOOST_CHECK(has(text, "SYNOPSIS 2\n"));
    BOOST_CHECK(has(text, "prog copy [options]"));
    BOOST_REQUIRE_EQUAL(formatter->inherited.size(), 1u);
    BOOST_CHECK_EQUAL(formatter->inherited[0], "-V");
}

// All three overloads of the row level, and the name level for a subcommand. Every help text
// passes through their defaults, and no other case overrides them. A class that specifies each
// level as independently replaceable requires this coverage.
BOOST_AUTO_TEST_CASE(test_a_formatter_can_change_a_row) {
    struct Rows : HelpFormatter {
        std::string displayed(const Command &command) const override {
            return "/" + HelpFormatter::displayed(command);
        }
        HelpBlock::Entry entry(const Argument &argument, const HelpSizes &sizes) const override {
            auto res = HelpFormatter::entry(argument, sizes);
            res.right = "arg: " + res.right;
            return res;
        }
        HelpBlock::Entry entry(const Option &option, const HelpSizes &sizes) const override {
            auto res = HelpFormatter::entry(option, sizes);
            res.right = "opt: " + res.right;
            return res;
        }
        HelpBlock::Entry entry(const Command &command, const HelpSizes &sizes) const override {
            auto res = HelpFormatter::entry(command, sizes);
            res.right = "cmd: " + res.right;
            return res;
        }
    };

    auto parser = helpTree();
    parser.setHelpFormatter(std::make_shared<Rows>());
    auto text = parser.parse(argv({})).helpText();

    // The command row calls the name level through the base implementation, so that an override
    // of either level is visible here.
    BOOST_CHECK(has(text, "/copy"));
    BOOST_CHECK(has(text, "cmd: Copy things"));
    BOOST_CHECK(has(text, "opt: Show this help and exit"));
    // The usage line writes the path rather than a name and is not affected by either override.
    BOOST_CHECK(!has(text, "/prog"));

    // Arguments are checked one level down because the root has none.
    text = parser.parse(argv({"copy", "a", "b"})).helpText();
    BOOST_CHECK(has(text, "arg: Where from"));
    BOOST_CHECK(has(text, "opt: Overwrite"));
}

// ---------------------------------------------------------------------------------------------
// Validity of a command tree
// ---------------------------------------------------------------------------------------------

// The builders retain an invalid tree intact so that one validator can diagnose the complete
// tree. These helpers are private implementation shared by that validator and these tests.
BOOST_AUTO_TEST_CASE(test_what_an_argument_may_follow) {
    const std::vector<Argument> none;
    BOOST_CHECK(detail::arguments_can_follow(none, Argument("path")));

    // An argument requires a nonempty name that no other argument has.
    BOOST_CHECK(!detail::arguments_can_follow(none, Argument("")));
    BOOST_CHECK(!detail::arguments_can_follow({Argument("path")}, Argument("path")));
    BOOST_CHECK(detail::arguments_can_follow({Argument("path")}, Argument("dest")));

    // A required argument cannot follow an optional argument. Otherwise a single token is
    // ambiguous between the two.
    BOOST_CHECK(!detail::arguments_can_follow({Argument("a").optional()}, Argument("b")));
    BOOST_CHECK(detail::arguments_can_follow({Argument("a").optional()}, Argument("b").optional()));
    BOOST_CHECK(detail::arguments_can_follow({Argument("a")}, Argument("b").optional()));

    // A greedy argument reserves a token for each required argument after it. Only a required
    // argument may therefore follow it. This is the form copy <src>... <dest>.
    BOOST_CHECK(detail::arguments_can_follow({Argument("src").multi()}, Argument("dest")));
    BOOST_CHECK(
        !detail::arguments_can_follow({Argument("src").multi()}, Argument("dest").optional()));
    BOOST_CHECK(!detail::arguments_can_follow({Argument("src").multi()}, Argument("dest").multi()));

    // Because a Remainder consumes every token, no argument may follow it.
    BOOST_CHECK(!detail::arguments_can_follow({Argument("rest").nargs(Argument::Remainder)},
                                              Argument("dest")));

    // The same rules apply to every argument list, including the argument list of an option.
    BOOST_CHECK(!detail::arguments_can_follow({Argument("a").optional()}, Argument("b")));
}

BOOST_AUTO_TEST_CASE(test_what_an_option_may_join) {
    const std::vector<Option> none;
    BOOST_CHECK(detail::options_can_join(none, Option({"-f", "--force"}, "Force")));

    // An option requires a token, and the token must not be mistakable for a value.
    BOOST_CHECK(!detail::options_can_join(none, Option()));
    BOOST_CHECK(!detail::options_can_join(none, Option({""}, "Nameless")));
    BOOST_CHECK(!detail::options_can_join(none, Option({"force"}, "No dash")));
    BOOST_CHECK(!detail::options_can_join(none, Option({"-"}, "Just a dash")));
    BOOST_CHECK(detail::options_can_join(none, Option({"/f"}, "The DOS spelling")));

    // -- is an ordinary token because the parser reserves no token. A program that requires the
    // conventional terminator declares it, and a program that requires another token declares
    // that token.
    BOOST_CHECK(detail::options_can_join(none, Option({"--"}, "The rest")));
    BOOST_CHECK(detail::options_can_join(none, Option({"--force"}, "An ordinary long one")));

    // A new option must not reuse any token of an existing option.
    const std::vector<Option> taken = {Option({"-f", "--force"}, "Force")};
    BOOST_CHECK(!detail::options_can_join(taken, Option({"-f"}, "Again")));
    BOOST_CHECK(
        !detail::options_can_join(taken, Option({"-x", "--force"}, "Again by its long name")));
    BOOST_CHECK(detail::options_can_join(taken, Option({"-x", "--other"}, "Neither")));

    // An option that substitutes for an empty command line is never typed. It therefore cannot
    // be required and cannot have arguments.
    BOOST_CHECK(
        detail::options_can_join(none, Option({"-h"}, "Help").prior(Option::AutoSetWhenNoSymbols)));
    BOOST_CHECK(!detail::options_can_join(
        none, Option({"-h"}, "Help").prior(Option::AutoSetWhenNoSymbols).required()));
    BOOST_CHECK(!detail::options_can_join(
        none, Option({"-h"}, "Help").prior(Option::AutoSetWhenNoSymbols).arg("topic")));
}

// A collision that is not detectable while the builders add items, because a command has no
// information about its future ancestors. The check applies to the complete tree.
BOOST_AUTO_TEST_CASE(test_a_recursive_option_and_a_local_one_may_not_share_a_spelling) {
    const auto &tree = [](bool recursive, const char *below) {
        return Command("prog")
            .addOption(Option({"-o"}, "The root's").arg("root").recursive(recursive))
            .addCommand(Command("sub").addOption(Option({below}, "The subcommand's").arg("sub")));
    };

    // Both options are in scope at sub and both match -o, but a result is queried by token.
    BOOST_CHECK(!detail::tree_can_be_parsed(tree(true, "-o")));

    // The same two options are valid if the root option is not recursive, because only one of
    // them is in scope at any command.
    BOOST_CHECK(detail::tree_can_be_parsed(tree(false, "-o")));

    // Different tokens are valid, which is the ordinary case.
    BOOST_CHECK(detail::tree_can_be_parsed(tree(true, "-p")));

    // A declaration two levels up is treated the same as one level up.
    BOOST_CHECK(!detail::tree_can_be_parsed(
        Command("prog")
            .addOption(Option({"-v"}, "The root's").recursive())
            .addCommand(Command("mid").addCommand(
                Command("leaf").addOption(Option({"-v"}, "The leaf's"))))));

    // Two recursive options from different levels are both in scope at the same command.
    BOOST_CHECK(!detail::tree_can_be_parsed(
        Command("prog")
            .addOption(Option({"-v"}, "The root's").recursive())
            .addCommand(Command("mid")
                            .addOption(Option({"-v"}, "The middle's").recursive())
                            .addCommand(Command("leaf")))));
}

// The same check, evaluated with the parse options that apply to the command line. Under
// case-insensitive matching, two tokens that differ only in case are one token, and the parse
// options are unknown before the parse.
BOOST_AUTO_TEST_CASE(test_names_that_are_one_name_only_under_a_matching_rule) {
    auto options = Command("prog").addOptions(
        {Option({"--output"}, "One"), Option({"--OUTPUT"}, "The other")});
    BOOST_CHECK(detail::tree_can_be_parsed(options));
    BOOST_CHECK(!detail::tree_can_be_parsed(options, true, false));
    // The matching rule for commands does not affect the matching rule for options.
    BOOST_CHECK(detail::tree_can_be_parsed(options, false, true));

    auto commands =
        Command("prog").addCommands({Command("build"), Command("BUILD", "The other one")});
    BOOST_CHECK(detail::tree_can_be_parsed(commands));
    BOOST_CHECK(!detail::tree_can_be_parsed(commands, false, true));
    BOOST_CHECK(detail::tree_can_be_parsed(commands, true, false));

    // A recursive option and a local option that differ only in case. Detection requires both
    // the matching rule and the complete tree.
    auto mixed = Command("prog")
                     .addOption(Option({"--force"}, "The root's").recursive())
                     .addCommand(Command("sub").addOption(Option({"--FORCE"}, "The sub's")));
    BOOST_CHECK(detail::tree_can_be_parsed(mixed));
    BOOST_CHECK(!detail::tree_can_be_parsed(mixed, true, false));
}

// Because a Remainder consumes the rest of the command line, every option must precede it. An
// option with a greedy argument consumes tokens until a stop condition occurs. One command cannot
// have both. If the option comes first, it consumes the arguments. If the arguments come first, the
// option becomes one of their values. Only a third option between them separates the two.
BOOST_AUTO_TEST_CASE(test_a_remainder_and_a_greedy_option_cannot_share_a_command) {
    const auto &tree = [](Argument::Arity arity) {
        return Command("prog")
            .addArgument(Argument("script"))
            .addArgument(Argument("args").nargs(arity).optional())
            .addOption(Option({"-f"}, "Files").arg(Argument("file").multi()));
    };
    BOOST_CHECK(!detail::tree_can_be_parsed(tree(Argument::Remainder)));
    // A greedy argument differs from a Remainder. Because it reserves tokens, a valid order exists.
    BOOST_CHECK(detail::tree_can_be_parsed(tree(Argument::Multiple)));

    // An option with a Single argument is valid beside a Remainder because its argument has a
    // fixed end.
    BOOST_CHECK(detail::tree_can_be_parsed(
        Command("prog")
            .addArgument(Argument("args").nargs(Argument::Remainder).optional())
            .addOption(Option({"-o"}, "Out").arg("dir"))));

    // The greedy option may be inherited from an ancestor, which the receiving command cannot
    // detect.
    BOOST_CHECK(!detail::tree_can_be_parsed(
        Command("prog")
            .addOption(Option({"-f"}, "Files").arg(Argument("file").multi()).recursive())
            .addCommand(Command("sub").addArgument(
                Argument("args").nargs(Argument::Remainder).optional()))));
}

// A Remainder starts after the preceding argument is filled. If the Remainder is the first
// argument, no preceding argument exists. It therefore starts at the first token that does not
// have the form of an option. A wrapper uses this behavior to accept its own options and forward
// the remaining tokens, as in sudo -u root ls -l.
BOOST_AUTO_TEST_CASE(test_a_leading_remainder_starts_at_the_first_token_that_is_not_an_option) {
    Parser parser(Command("run")
                      .addArgument(Argument("rest").nargs(Argument::Remainder).optional())
                      .addOption(Option({"-v"}, "Verbose"))
                      .addOption(Option({"-u"}, "As user").arg("who")));

    auto wrapped = ok(parser, {"-u", "root", "ls", "-v", "-u", "x"});
    BOOST_CHECK_EQUAL(wrapped.valueForOption("-u").value_or(""), "root");
    BOOST_CHECK(!wrapped.option("-v").has_value());
    BOOST_CHECK(wrapped.values(0) == std::vector<std::string>({"ls", "-v", "-u", "x"}));

    // Without options of the wrapper, the whole command line is the tail.
    BOOST_CHECK(ok(parser, {"ls", "-l"}).values(0) == std::vector<std::string>({"ls", "-l"}));
    // With only options of the wrapper, the tail is empty.
    auto bare = ok(parser, {"-v"});
    BOOST_CHECK(bare.option("-v").has_value());
    BOOST_CHECK(bare.values(0) == std::vector<std::string>());

    // An undeclared token with the form of an option is reported rather than forwarded, because
    // a wrapper must not treat its own misspelled flags as payload.
    bad(parser, {"-w", "ls"}, ParseResult::UnknownOption);

    // If an argument comes first, filling that argument ends the options. An option written
    // after it therefore belongs to the tail, whether declared or not.
    Parser named(Command("run")
                     .addArgument(Argument("cmd"))
                     .addArgument(Argument("rest").nargs(Argument::Remainder).optional())
                     .addOption(Option({"-v"}, "Verbose")));
    auto after = ok(named, {"-v", "ls", "-v"});
    BOOST_CHECK(after.option("-v").has_value());
    BOOST_CHECK_EQUAL(after.option("-v")->count(), 1);
    BOOST_CHECK_EQUAL(after.value(0).value_or(""), "ls");
    BOOST_CHECK(after.values(1) == std::vector<std::string>({"-v"}));
}

BOOST_AUTO_TEST_CASE(test_what_a_subcommand_may_join_and_what_a_catalogue_may_group) {
    const std::vector<Command> none;
    BOOST_CHECK(detail::commands_can_join(none, Command("copy")));
    BOOST_CHECK(!detail::commands_can_join(none, Command("")));
    BOOST_CHECK(!detail::commands_can_join({Command("copy")}, Command("copy")));
    BOOST_CHECK(detail::commands_can_join({Command("copy")}, Command("move")));

    // A name in two groups is listed under the first group and missing from the second, which
    // appears as if the catalogue were ignored.
    const std::vector<CommandCatalogue::Group> none_yet;
    BOOST_CHECK(detail::catalogue_can_add_group(none_yet, {"copy", "move"}));
    BOOST_CHECK(!detail::catalogue_can_add_group(none_yet, {"copy", "copy"}));

    const std::vector<CommandCatalogue::Group> filesystem = {
        {"Filesystem", {"copy", "move"}}
    };
    BOOST_CHECK(!detail::catalogue_can_add_group(filesystem, {"build", "copy"}));
    BOOST_CHECK(detail::catalogue_can_add_group(filesystem, {"build", "configure"}));
}

BOOST_AUTO_TEST_CASE(test_a_parser_can_validate_a_complete_tree_on_request) {
    Parser valid(Command("prog")
                     .addArgument(Argument("source"))
                     .addOption(Option({"-f", "--force"}, "Force"))
                     .addCommand(Command("copy")));
    BOOST_CHECK(!valid.validate().has_value());

    // Every invalid item is added to the tree. No constructor or builder requires information
    // about whether the caller has finished the tree.
    Parser badArguments(Command("prog")
                            .addArgument(Argument("source").optional())
                            .addArgument(Argument("destination")));
    BOOST_REQUIRE(badArguments.validate().has_value());
    BOOST_CHECK(has(*badArguments.validate(), "argument sequence"));

    Parser badOptions(Command("prog")
                          .addOption(Option({"-f", "-f"}, "Force"))
                          .addOption(Option({"-n"}, "Count").multi(-1)));
    BOOST_REQUIRE(badOptions.validate().has_value());
    BOOST_CHECK(has(*badOptions.validate(), "option spelling"));

    Parser badOccurrence(Command("prog").addOption(Option({"-n"}, "Count").multi(-1)));
    BOOST_REQUIRE(badOccurrence.validate().has_value());
    BOOST_CHECK(has(*badOccurrence.validate(), "negative occurrence"));

    Parser badOptionArguments(Command("prog").addOption(
        Option({"-o"}, "Output").arg(Argument("format").optional()).arg(Argument("path"))));
    BOOST_REQUIRE(badOptionArguments.validate().has_value());
    BOOST_CHECK(has(*badOptionArguments.validate(), "argument sequence"));

    Parser badAutomatic(Command("prog").addOption(
        Option({"-h"}, "Help").required().prior(Option::AutoSetWhenNoSymbols)));
    BOOST_REQUIRE(badAutomatic.validate().has_value());
    BOOST_CHECK(has(*badAutomatic.validate(), "automatic option"));

    Parser badCommands(Command("prog").addCommand(Command("copy")).addCommand(Command("copy")));
    BOOST_REQUIRE(badCommands.validate().has_value());
    BOOST_CHECK(has(*badCommands.validate(), "subcommand name"));
}

BOOST_AUTO_TEST_CASE(test_validation_checks_argument_defaults_and_catalogue_references) {
    Parser badDefault(
        Command("prog").addArgument(Argument("count").optional().type<int>().defaultValue("many")));
    BOOST_REQUIRE(badDefault.validate().has_value());
    BOOST_CHECK(has(*badDefault.validate(), "default value"));

    Parser badExpected(Command("prog").addArgument(Argument("count").type<int>().expect({"many"})));
    BOOST_REQUIRE(badExpected.validate().has_value());
    BOOST_CHECK(has(*badExpected.validate(), "expected value"));

    Parser requiredDefault(Command("prog").addArgument(Argument("count").defaultValue("1")));
    BOOST_REQUIRE(requiredDefault.validate().has_value());
    BOOST_CHECK(has(*requiredDefault.validate(), "required"));

    Parser unexpectedDefault(Command("prog").addArgument(
        Argument("color").optional().expect({"red", "blue"}).defaultValue("green")));
    BOOST_REQUIRE(unexpectedDefault.validate().has_value());
    BOOST_CHECK(has(*unexpectedDefault.validate(), "outside its expected values"));

    Parser refusedDefault(
        Command("prog").addArgument(Argument("count").optional().defaultValue("0").validate(
            [](std::string_view, std::string *error) {
                *error = "zero is reserved";
                return false;
            })));
    BOOST_REQUIRE(refusedDefault.validate().has_value());
    BOOST_CHECK(has(*refusedDefault.validate(), "zero is reserved"));

    CommandCatalogue catalogue;
    catalogue.addOptions("Common", {"--missing"});
    Parser badCatalogue(Command("prog").addOption(Option({"-f"}, "Force")).setCatalogue(catalogue));
    BOOST_REQUIRE(badCatalogue.validate().has_value());
    BOOST_CHECK(has(*badCatalogue.validate(), "unknown member"));

    CommandCatalogue duplicateCatalogue;
    duplicateCatalogue.addOptions("Common", {"-f"}).addOptions("Other", {"-f"});
    Parser duplicateGroup(
        Command("prog").addOption(Option({"-f"}, "Force")).setCatalogue(duplicateCatalogue));
    BOOST_REQUIRE(duplicateGroup.validate().has_value());
    BOOST_CHECK(has(*duplicateGroup.validate(), "more than one group"));
}

BOOST_AUTO_TEST_CASE(test_validation_uses_the_requested_matching_rules) {
    Parser parser(Command("prog")
                      .addOption(Option({"--force"}, "Force"))
                      .addOption(Option({"--FORCE"}, "Also force"))
                      .addCommand(Command("copy"))
                      .addCommand(Command("COPY")));

    BOOST_CHECK(!parser.validate().has_value());
    BOOST_CHECK(parser.validate(Parser::IgnoreOptionCase).has_value());
    BOOST_CHECK(parser.validate(Parser::IgnoreCommandCase).has_value());
}

BOOST_AUTO_TEST_CASE(test_a_tree_is_checked_only_on_request_or_by_a_debug_parse) {
    // Deliberately stateful instrumentation, not an example of a validator to write. It makes
    // the otherwise invisible release guarantee measurable.
    int calls = 0;
    const auto &tree = [&calls] {
        return Command("prog").addArgument(Argument("count").optional().defaultValue("1").validate(
            [&calls](std::string_view, std::string *) {
                ++calls;
                return true;
            }));
    };

    Parser parser(tree());
    BOOST_CHECK_EQUAL(calls, 0);
    parser.setRootCommand(tree());
    BOOST_CHECK_EQUAL(calls, 0);

    BOOST_CHECK(!parser.validate().has_value());
    BOOST_CHECK_EQUAL(calls, 1);

    calls = 0;
    BOOST_CHECK(ok(parser, {}).isValid());
#ifdef NDEBUG
    BOOST_CHECK_EQUAL(calls, 0);
#else
    BOOST_CHECK_EQUAL(calls, 1);
#endif
}

// ---------------------------------------------------------------------------------------------
// Reuse, ownership and lifetimes
// ---------------------------------------------------------------------------------------------

// A parser remains usable after parsing, and its tree can be replaced afterwards. Existing
// results continue to read the tree that produced them, because a result holds that tree rather
// than the parser, and setRootCommand stores a new pointer rather than assigning through the old
// pointer.
BOOST_AUTO_TEST_CASE(test_a_parser_is_reusable_and_its_tree_can_be_replaced) {
    Parser parser(Command("prog", "The first tree")
                      .addArgument(Argument("path"))
                      .addOption(Option({"-f"}, "Force")));
    parser.setTextWidth(80);

    // Two results from one parser. Neither result reads the values of the other.
    auto first = parser.parse(argv({"-f", "one"}));
    auto second = parser.parse(argv({"two"}));
    BOOST_CHECK(first.option("-f").has_value());
    BOOST_CHECK(!second.option("-f").has_value());
    BOOST_CHECK_EQUAL(must(first.value(0)), "one");
    BOOST_CHECK_EQUAL(must(second.value(0)), "two");

    parser.setRootCommand(Command("other", "The second tree").addArgument(Argument("target")));

    // Both results continue to read their original tree, for values and for help text.
    BOOST_CHECK_EQUAL(must(first.value(0)), "one");
    BOOST_CHECK(first.option("-f").has_value());
    BOOST_CHECK_EQUAL(must(second.value(0)), "two");
    BOOST_CHECK(has(first.helpText(), "Usage:\n    prog"));
    BOOST_CHECK(has(first.helpText(), "<path>"));
    BOOST_CHECK(has(first.helpText(), "The first tree"));
    BOOST_CHECK(!has(first.helpText(), "other"));

    // The next parse uses the new tree.
    auto third = parser.parse(argv({"x"}));
    BOOST_CHECK(has(third.helpText(), "Usage:\n    other"));
    BOOST_CHECK(has(third.helpText(), "<target>"));
    BOOST_CHECK(!third.option("-f").has_value());
    BOOST_CHECK_EQUAL(must(third.value(0)), "x");
}

// One command line, parsed once, has one owner of the result. If a result is copyable, the copy
// aliases the original rather than duplicating it, which is easy to do accidentally and never
// intended.
BOOST_AUTO_TEST_CASE(test_a_result_has_one_owner) {
    static_assert(!std::is_copy_constructible_v<ParseResult>, "a result is not copied");
    static_assert(!std::is_copy_assignable_v<ParseResult>, "a result is not copied");
    // The move operations must be noexcept. Otherwise a vector of results copies instead of
    // moving, and no copy operation exists.
    static_assert(std::is_nothrow_move_constructible_v<ParseResult>, "a result moves");
    static_assert(std::is_nothrow_move_assignable_v<ParseResult>, "a result moves");

    auto parser = formatterTree();
    auto first = parser.parse(argv({"-o", "out.txt", "in.txt"}));
    BOOST_REQUIRE(first.isValid());

    // After a move, every value that the result returns refers to the destination.
    auto second = std::move(first);
    BOOST_CHECK_EQUAL(must(second.value(0)), "in.txt");
    BOOST_CHECK_EQUAL(must(second.valueForOption("-o")), "out.txt");
    BOOST_CHECK(has(second.helpText(), "Usage:"));

    // A container of results requires this behavior.
    std::vector<ParseResult> results;
    results.push_back(std::move(second));
    results.push_back(parser.parse(argv({"-o", "b", "a"})));
    BOOST_CHECK_EQUAL(must(results.front().value(0)), "in.txt");
    BOOST_CHECK_EQUAL(must(results.back().value(0)), "a");
}

// A formatter is shared rather than exclusively owned by the parser. A ParseResult prints its
// help without access to the parser that produced it and must keep the help formatter alive.
BOOST_AUTO_TEST_CASE(test_a_formatter_outlives_the_parser_that_used_it) {
    auto formatter = std::make_shared<Braces>();
    ParseResult result;
    {
        auto parser = formatterTree();
        parser.setHelpFormatter(formatter);
        result = parser.parse(argv({}));
    }
    BOOST_CHECK(has(result.helpText(), "{path}"));

    // Because a formatter holds no state between calls, one formatter can serve two parsers.
    auto one = formatterTree();
    auto two = formatterTree();
    one.setHelpFormatter(formatter);
    two.setHelpFormatter(formatter);
    BOOST_CHECK_EQUAL(one.parse(argv({})).helpText(), two.parse(argv({})).helpText());
}

// A program removes a custom formatter by setting null. The parser then uses the default
// formatter rather than having no formatter for its help text.
BOOST_AUTO_TEST_CASE(test_no_formatter_means_the_plain_one) {
    auto parser = formatterTree();
    BOOST_CHECK(parser.helpFormatter() != nullptr);

    parser.setHelpFormatter(std::make_shared<Braces>());
    BOOST_CHECK(has(parser.parse(argv({})).helpText(), "{path}"));

    parser.setHelpFormatter(nullptr);
    BOOST_CHECK(parser.helpFormatter() != nullptr);
    BOOST_CHECK(has(parser.parse(argv({})).helpText(), "<path>"));
}

// The inputs available to a formatter, which are the same inputs that the default formatter
// uses.
BOOST_AUTO_TEST_CASE(test_a_result_answers_for_what_its_help_is_made_from) {
    auto parser = helpTree();
    auto root = parser.parse(argv({}));

    BOOST_CHECK_EQUAL(root.prologue(), "A prologue line");
    BOOST_CHECK_EQUAL(root.epilogue(), "An epilogue line");
    BOOST_CHECK(!root.helpLayout().isEmpty());
    // Because the root has no parent, it inherits no option.
    BOOST_CHECK(root.inheritedOptions().empty());

    auto sub = parser.parse(argv({"copy", "a", "b"}));
    BOOST_REQUIRE_EQUAL(sub.inheritedOptions().size(), 1u);
    BOOST_CHECK(sub.inheritedOptions().front()->isRecursive());
}

// The measurement helpers that a formatter requires to lay out a block. The helpers are public,
// so that a formatter does not reimplement them.
BOOST_AUTO_TEST_CASE(test_the_formatter_lends_out_what_it_measures_with) {
    auto lines = HelpFormatter::wrapped("one two three four", 9);
    BOOST_REQUIRE_EQUAL(lines.size(), 3u);
    BOOST_CHECK_EQUAL(lines[0], "one two");
    BOOST_CHECK_EQUAL(lines[2], "four");

    HelpBlock block;
    block.entries = {
        {"-v",        "Say more"},
        {"--verbose", "The same"}
    };
    BOOST_CHECK_EQUAL(HelpFormatter::widestOf(block), 9u);

    HelpBlock wider;
    wider.entries = {
        {"--configuration", ""}
    };
    BOOST_CHECK_EQUAL(HelpFormatter::widestOf(std::vector<HelpBlock>{block, wider}), 15u);
}

// The left column is also measured in display columns. A non-ASCII metavar has more bytes than
// display columns, and counting bytes shifts every description in the block too far to the
// right.
BOOST_AUTO_TEST_CASE(test_alignment_counts_columns_not_bytes) {
    // <模式> has eight bytes and six columns. <path> has six of each.
    const std::string metavar = "\xe6\xa8\xa1\xe5\xbc\x8f";
    BOOST_REQUIRE_EQUAL(stdc::console::display_width("<" + metavar + ">"), 6);
    BOOST_REQUIRE_EQUAL(("<" + metavar + ">").size(), 8u);

    Parser parser(Command("prog")
                      .addArgument(Argument("path", "Where to write"))
                      .addArgument(Argument("mode", "How to write it").metavar(metavar)));
    parser.setTextWidth(80);
    auto text = parser.parse({"prog", "a", "b"}).helpText();

    // The widest entry in the block is followed by exactly the spacing. Both entries here have
    // the same width, and therefore both are followed by exactly the spacing. Counting bytes gives
    // the entry with more bytes unnecessary padding and shifts the whole column.
    // Rows are located by description because the usage line also contains the metavars.
    for (const auto &pair : {std::make_pair(std::string("<path>"), std::string("Where to write")),
                             std::make_pair("<" + metavar + ">", std::string("How to write it"))}) {
        auto lines = entryLines(text, pair.second);
        BOOST_REQUIRE_MESSAGE(!lines.empty(), "no row for " + pair.second);
        auto at = lines[0].find(pair.second);
        BOOST_REQUIRE(at != std::string::npos);

        auto left = lines[0].substr(0, at);
        BOOST_CHECK_MESSAGE(has(left, pair.first),
                            "[" + left + "] is not the row for " + pair.first);
        auto spaces = left.size() - left.find_last_not_of(' ') - 1;
        BOOST_CHECK_MESSAGE(spaces == 4, pair.first + " is followed by " + std::to_string(spaces) +
                                             " spaces, not 4");
    }
}

// A width of zero, the default, requests detection rather than a fixed value. Without a terminal,
// the width is taken from COLUMNS, and it is 80 columns if COLUMNS is also unset.
BOOST_AUTO_TEST_CASE(test_the_default_width_is_asked_for) {
    Parser parser(Command("prog").addOption(
        Option({"-x"}, "A description long enough that it has to be broken somewhere along the "
                       "way, wherever that turns out to be")));
    BOOST_CHECK_EQUAL(parser.textWidth(), 0);

    const char *saved = std::getenv("COLUMNS");
    std::string keep = saved ? saved : std::string();
    const auto &setColumns = [](const char *value) {
#ifdef _WIN32
        _putenv_s("COLUMNS", value ? value : "");
#else
        if (value) {
            setenv("COLUMNS", value, 1);
        } else {
            unsetenv("COLUMNS");
        }
#endif
    };

    setColumns(nullptr);
    auto wide = entryLines(parser.parse({"prog"}).helpText(), "-x");

    setColumns("40");
    auto narrow = entryLines(parser.parse({"prog"}).helpText(), "-x");

    setColumns(saved ? keep.c_str() : nullptr);

    BOOST_CHECK_GT(narrow.size(), wide.size());

    // An explicit width ignores the environment entirely.
    setColumns("40");
    parser.setTextWidth(200);
    BOOST_CHECK_EQUAL(entryLines(parser.parse({"prog"}).helpText(), "-x").size(), 1u);
    setColumns(saved ? keep.c_str() : nullptr);
}

// A read returns an owning value, which therefore remains valid after the source result is
// destroyed.
//
// A view is the cheaper default and the incorrect one. Every view that a result returns refers
// into the storage of that result, and the form below is common in caller code. With a view as
// the default, this form reads freed storage. The address sanitizer reports the access directly,
// and an ordinary build prints arbitrary memory contents.
// This is the counterpart of the ownership split described in the OptionResult warning. The
// handle borrows from the result, and every value read through the handle is owning. A value
// read while the result is alive therefore remains valid after both are destroyed, and only the
// handle must not outlive the result.
BOOST_AUTO_TEST_CASE(test_a_read_through_an_option_handle_owns_what_it_answers) {
    Parser parser(Command("prog").addOption(Option({"-j"}, "Jobs").arg("n")));

    std::optional<int> jobs;
    std::optional<std::string> raw;
    std::vector<std::string> all;
    {
        auto result = parser.parse(argv({"-j", "8"}));
        auto handle = result.option("-j");
        BOOST_REQUIRE(handle);
        BOOST_CHECK_EQUAL(handle->count(), 1);

        // The declaration behind the handle, which a program reads to identify the matched
        // option. It refers into the tree whose lifetime the result extends, not into the handle.
        BOOST_REQUIRE(handle->option() != nullptr);
        BOOST_CHECK_EQUAL(handle->option()->token(), "-j");
        BOOST_CHECK_EQUAL(handle->option()->description(), "Jobs");

        jobs = handle->value<int>();
        raw = handle->value<std::string>();
        auto values = handle->values<std::string>();
        BOOST_REQUIRE(values);
        all = *values;
    }

    BOOST_CHECK(jobs == 8);
    BOOST_CHECK(raw == std::string("8"));
    BOOST_REQUIRE_EQUAL(all.size(), 1u);
    BOOST_CHECK_EQUAL(all[0], "8");
}

BOOST_AUTO_TEST_CASE(test_a_read_outlives_the_result_it_came_from) {
    Parser parser(Command("prog")
                      .addArgument(Argument("source"))
                      .addOption(Option({"-f"}, "File").arg("path")));

    auto positional = parser.parse(argv({"-f", "some/path.txt", "the-source"})).value(0);
    auto from_option =
        parser.parse(argv({"-f", "some/path.txt", "the-source"})).valueForOption("-f");
    auto several = parser.parse(argv({"-f", "some/path.txt", "the-source"})).values(0);

    static_assert(std::is_same_v<decltype(positional), std::optional<std::string>>,
                  "the default read has to own what it holds");
    static_assert(std::is_same_v<decltype(from_option), std::optional<std::string>>,
                  "the default read has to own what it holds");
    static_assert(std::is_same_v<decltype(several), std::optional<std::vector<std::string>>>,
                  "the default read has to own what it holds");

    BOOST_CHECK_EQUAL(must(positional), "the-source");
    BOOST_CHECK_EQUAL(must(from_option), "some/path.txt");
    BOOST_REQUIRE_EQUAL(must(several).size(), 1u);
    BOOST_CHECK_EQUAL(must(several)[0], "the-source");

    // A caller can still request a view and is then responsible for keeping the result alive.
    auto result = parser.parse(argv({"-f", "some/path.txt", "the-source"}));
    static_assert(std::is_same_v<decltype(result.value<std::string_view>(0)),
                                 std::optional<std::string_view>>,
                  "asking for a view still gives a view");
    BOOST_CHECK_EQUAL(must(result.value<std::string_view>(0)), "the-source");
    BOOST_CHECK_EQUAL(must(result.rawValue(0)), "the-source");
}

BOOST_AUTO_TEST_CASE(test_reading_a_result_that_failed) {
    // A caller that omits the isValid check still receives defined results rather than an
    // out-of-bounds access.
    Parser parser(Command("prog").addArgument(Argument("needed")));
    auto result = parser.parse(argv({}));
    BOOST_REQUIRE(!result.isValid());

    BOOST_CHECK(!result.rawValue(0).has_value());
    BOOST_CHECK(result.command() != nullptr);
    BOOST_CHECK_EQUAL(capturedStderr([&] { BOOST_CHECK_EQUAL(result.invoke(-3), -3); }).empty(),
                      false);
    BOOST_CHECK(!result.errorText().empty());
    // The help text still renders. A program prints it together with the error report.
    BOOST_CHECK(has(result.helpText(), "Usage:"));
}

BOOST_AUTO_TEST_SUITE_END()
