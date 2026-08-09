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

    /// \a command's own names against \a inherited, then the same for what is under it, carrying
    /// whatever it declares recursive down with it.
    inline bool unambiguous_under(const Command &command, std::vector<const Option *> inherited,
                                  bool ignoreOptionCase, bool ignoreCommandCase) {
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

    /// Whether every command in \a command's tree can tell its own names apart.
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
    inline bool names_are_unambiguous(const Command &command, bool ignoreOptionCase = false,
                                      bool ignoreCommandCase = false) {
        std::vector<const Option *> inherited;
        return unambiguous_under(command, inherited, ignoreOptionCase, ignoreCommandCase);
    }

}

#endif // STDCORELIB_COMMANDLINE_P_H
