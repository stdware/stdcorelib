// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_COMMANDLINE_P_H
#define STDCORELIB_COMMANDLINE_P_H

#include <string_view>
#include <vector>

#include <stdcorelib/str.h>
#include <stdcorelib/support/commandline.h>

namespace stdc::cli::detail {

    /// Whether two declared names are the same name, which under a case-insensitive parse they
    /// are without being the same text.
    inline bool same_name(std::string_view a, std::string_view b, bool ignore_case) {
        return ignore_case ? str::strcasecmp(a, b) == 0 : a == b;
    }

    /// The spellings of \a option that a value may be stuck to, which is what the parser tries
    /// once an exact lookup has failed. Mirrors what readOption() will accept.
    ///
    /// One token carries one value, so the option has to want exactly one and have to have it.
    /// An argument that takes more is left out too: a value written against the spelling says
    /// this is the value, and reading on from there would take the next token as well.
    inline std::vector<std::string_view> sticky_spellings(const Option &option) {
        std::vector<std::string_view> res;
        if (option.shortMatch() == Option::NoShortMatch || option.arguments().size() != 1 ||
            !option.arguments().front().isRequired() ||
            option.arguments().front().arity() != Argument::Single) {
            return res;
        }
        for (const auto &spelling : option.tokens()) {
            if (option.shortMatch() != Option::ShortMatchAll && spelling.size() != 2) {
                continue;
            }
            if (option.shortMatch() == Option::ShortMatchSingleLetter &&
                !(spelling.size() > 1 && ((spelling[1] >= 'a' && spelling[1] <= 'z') ||
                                          (spelling[1] >= 'A' && spelling[1] <= 'Z')))) {
                continue;
            }
            res.push_back(spelling);
        }
        return res;
    }

    /// Whether \a command can be given both what its own arguments want and what its options
    /// want.
    ///
    /// A Remainder argument takes the rest of the line from where it starts, so every option has
    /// to be written before it. An option whose own argument is greedy reads on until the next
    /// declared option or the end, so it has to be written where something stops it. Put the two
    /// in one command and there is no order that works: the option first eats the arguments, the
    /// arguments first turn the option into one of their values, and only a third option written
    /// between them saves it.
    ///
    inline bool arguments_can_be_reached(const Command &command,
                                         const std::vector<const Option *> &inherited) {
        bool has_remainder = false;
        for (const auto &argument : command.arguments()) {
            if (argument.arity() == Argument::Remainder) {
                has_remainder = true;
                break;
            }
        }
        if (!has_remainder) {
            return true;
        }
        const auto &greedy = [](const Option &option) {
            for (const auto &argument : option.arguments()) {
                if (argument.arity() != Argument::Single) {
                    return true;
                }
            }
            return false;
        };
        for (const auto &option : command.options()) {
            if (greedy(option)) {
                return false;
            }
        }
        for (const auto *option : inherited) {
            if (greedy(*option)) {
                return false;
            }
        }
        return true;
    }

    /// \a command's own names against \a inherited, then the same for what is under it, carrying
    /// whatever it declares recursive down with it.
    inline bool unambiguous_under(const Command &command, std::vector<const Option *> inherited,
                                  bool ignoreOptionCase, bool ignoreCommandCase) {
        if (!arguments_can_be_reached(command, inherited)) {
            return false;
        }

        std::vector<std::string_view> spellings;
        const auto &take = [&spellings, ignoreOptionCase](const Option &option) {
            for (const auto &spelling : option.tokens()) {
                for (const auto &taken : spellings) {
                    if (same_name(taken, spelling, ignoreOptionCase)) {
                        return false;
                    }
                }
                spellings.push_back(spelling);
            }
            return true;
        };
        for (const auto &option : command.options()) {
            if (!take(option)) {
                return false;
            }
        }
        for (const auto *option : inherited) {
            if (!take(*option)) {
                return false;
            }
        }

        // A value stuck to a spelling is matched by walking the options and taking the first
        // whose spelling the token starts with. Where one such spelling is the start of another,
        // -D and -Da with -Dabc, which is taken depends on the order they were declared in and
        // neither is more right than the other.
        std::vector<std::string_view> sticky;
        const auto &takeSticky = [&sticky, ignoreOptionCase](const Option &option) {
            for (auto spelling : sticky_spellings(option)) {
                for (const auto &taken : sticky) {
                    const auto &shorter = taken.size() < spelling.size() ? taken : spelling;
                    const auto &longer = taken.size() < spelling.size() ? spelling : taken;
                    if (same_name(shorter, longer.substr(0, shorter.size()), ignoreOptionCase)) {
                        return false;
                    }
                }
                sticky.push_back(spelling);
            }
            return true;
        };
        for (const auto &option : command.options()) {
            if (!takeSticky(option)) {
                return false;
            }
        }
        for (const auto *option : inherited) {
            if (!takeSticky(*option)) {
                return false;
            }
        }

        std::vector<std::string_view> names;
        for (const auto &sub : command.commands()) {
            for (const auto &taken : names) {
                if (same_name(taken, sub.name(), ignoreCommandCase)) {
                    return false;
                }
            }
            names.push_back(sub.name());
        }

        for (const auto &option : command.options()) {
            if (option.isRecursive()) {
                inherited.push_back(&option);
            }
        }
        for (const auto &sub : command.commands()) {
            if (!unambiguous_under(sub, inherited, ignoreOptionCase, ignoreCommandCase)) {
                return false;
            }
        }
        return true;
    }

    /// Whether every command in \a command's tree can be parsed at all.
    ///
    /// What is in scope at a command is its own options plus the recursive ones above it, and
    /// they are looked up by spelling, so two of them answering to one spelling has no right
    /// answer. The pieces cannot see this as they are added: a command knows nothing about the
    /// ancestors it will end up under, so the whole tree is asked once it is one.
    ///
    /// \a ignoreOptionCase and \a ignoreCommandCase are the options the line will be read with,
    /// since under those two spellings differing only in case are one spelling. Parser asserts
    /// this without them where it is handed a tree and with them where it parses.
    ///
    /// Here rather than beside canFollow() and canJoin() because nothing public calls it. Those
    /// are asserted by the setters in the header itself and have to be where the setters are.
    inline bool tree_can_be_parsed(const Command &command, bool ignoreOptionCase = false,
                                   bool ignoreCommandCase = false) {
        std::vector<const Option *> inherited;
        return unambiguous_under(command, inherited, ignoreOptionCase, ignoreCommandCase);
    }

}

#endif // STDCORELIB_COMMANDLINE_P_H
