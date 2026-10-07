// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_VERSIONNUMBER_H
#define STDCORELIB_VERSIONNUMBER_H

#include <string>
#include <array>
#include <iostream>
#include <optional>

#include <stdcorelib/stdc_global.h>

namespace stdc {

    /// \addtogroup utility
    /// @{

    class STDC_EXPORT VersionNumber {
    public:
        VersionNumber();
        explicit VersionNumber(int major, int minor = 0, int patch = 0, int tweak = 0);

        /// Parses \a s as a version.
        ///
        /// A version consists of one to four components separated by dots. Each component
        /// consists of digits only and must fit in an \c int. Leading zeros do not change the
        /// value. \c 01.02 is therefore the version 1.2.
        ///
        /// \return the version, or \c std::nullopt if \a s is not a version
        /// \note Invalid input is rejected as a whole rather than read up to the first invalid
        ///       character. \c abc is therefore not the version 0, and \c 1.2.3.4.5 is not the
        ///       version 1.2.3.4.
        static std::optional<VersionNumber> fromString(const std::string_view &s);

    public:
        inline int major() const {
            return m_numbers[0];
        }

        inline int minor() const {
            return m_numbers[1];
        }

        inline int patch() const {
            return m_numbers[2];
        }

        inline int tweak() const {
            return m_numbers[3];
        }

        std::string toString() const;
        bool isEmpty() const;

        bool operator==(const VersionNumber &RHS) const;
        bool operator!=(const VersionNumber &RHS) const;
        bool operator<(const VersionNumber &RHS) const;
        bool operator>(const VersionNumber &RHS) const;
        bool operator<=(const VersionNumber &RHS) const;
        bool operator>=(const VersionNumber &RHS) const;

    private:
        std::array<int, 4> m_numbers;
    };

    /// @}

}

namespace std {

    /// \addtogroup utility
    /// @{

    template <>
    struct STDC_EXPORT hash<stdc::VersionNumber> {
        size_t operator()(const stdc::VersionNumber &key) const;
    };

    inline ostream &operator<<(std::ostream &out, const stdc::VersionNumber &c) {
        out << "VersionNumber(" << c.toString() << ")";
        return out;
    }

    /// @}

}

#endif // STDCORELIB_VERSIONNUMBER_H
