// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_TYPE_ID_H
#define STDCORELIB_TYPE_ID_H

#include <atomic>
#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>

#include <stdcorelib/stdc_global.h>

/// \defgroup types Type identity and registries
///
/// stdc::type_id identifies a type without \c typeid and keeps the identity consistent across
/// shared library boundaries, which \c std::type_index does not guarantee.
/// stdc::StaticRegistry and stdc::DynamicRegistry provide the registries of a plugin system.

namespace stdc {

    /// \addtogroup types
    /// @{

    namespace detail {

        /// Returns the compiler's spelling of \a T, extracted from the signature of this function.
        ///
        /// Type identity is based on this text. Every module that uses \a T has its own copy of
        /// the other parts of this header, and the spelling is the only part that is guaranteed
        /// to be identical in all modules.
        ///
        /// \note The spelling is the output of the compiler, not a normalized form. MSVC writes
        ///       \c "struct Foo", and GCC writes \c "Foo". The difference has no effect within a
        ///       process, which never contains code from two compilers. However, the name is not
        ///       a portable key for storage in a file.
        template <class T>
        constexpr std::string_view type_name() {
#if defined(_MSC_VER)
            constexpr std::string_view signature = __FUNCSIG__;
            constexpr std::string_view opening = "type_name<";
            constexpr auto first = signature.find(opening) + opening.size();
            constexpr auto last = signature.rfind(">(void)");
#else
            constexpr std::string_view signature = __PRETTY_FUNCTION__;
            constexpr std::string_view opening = "T = ";
            constexpr auto first = signature.find(opening) + opening.size();
            // GCC lists the other template parameters after a semicolon. Clang does not.
            constexpr auto semicolon = signature.find(';', first);
            constexpr auto last =
                semicolon == std::string_view::npos ? signature.rfind(']') : semicolon;
#endif
            static_assert(first < last, "cannot read the type name out of this compiler");
            return signature.substr(first, last - first);
        }

        /// The registration of one type in one module. One entry exists per type and module.
        ///
        /// \c name is the identity. It is determined at compile time, and two entries denote the
        /// same type if and only if their names match.
        ///
        /// \c id caches the address that this process uses for the name. No address exists
        /// before the process creates its table, and none is required until ids from two modules
        /// are compared. The cache is therefore initially null, and the first comparison across
        /// modules fills it.
        struct type_entry {
            std::string_view name;
            std::atomic<const void *> id;
        };

        /// Registers \a entry and returns the address that this process uses for its name.
        ///
        /// The identity is an address rather than a number, because a number requires a
        /// counter, and a build in which two modules each hold a table has two counters that
        /// both start at one. An entry keeps the first value assigned to it. Entries numbered by
        /// different counters can therefore collide, and two unrelated types then compare equal.
        /// Addresses from different tables never collide.
        ///
        /// \note The table exists in exactly one place. This function therefore unifies the
        ///       modules that share one copy of the library. If stdcorelib is linked statically
        ///       into two modules, each module has its own table, and a type does not compare
        ///       equal across them. The result is then a false inequality rather than a false
        ///       equality.
        STDC_EXPORT const void *register_type_id(type_entry &entry);

        /// Returns the canonical address cached in \a entry, registering it on the first call.
        inline const void *resolve_type_id(type_entry &entry) {
            if (const void *cached = entry.id.load(std::memory_order_acquire)) {
                return cached;
            }
            return register_type_id(entry);
        }

        template <class T>
        type_entry &entry_of() {
            static type_entry entry{type_name<T>(), nullptr};
            return entry;
        }

    }

    /// The identity of a type, in a form that can be compared, stored, and passed between
    /// modules.
    ///
    /// A type_id provides the information of \c typeid without requiring RTTI and without
    /// depending on symbol merging by the loader. Two ids compare equal if they denote the same
    /// type, regardless of the modules that created them.
    ///
    /// \code
    ///   if (value.type() == type_id::of<Codec>()) {
    ///       ...
    ///   }
    /// \endcode
    ///
    /// \warning The type is used exactly as written. \c Derived and \c Base have different ids,
    ///          and no query determines whether one type derives from the other.
    ///
    /// \note Comparing two ids created in the same module is a pointer comparison. The first
    ///       comparison of ids from two modules queries a table in the library, once per type
    ///       and module, and caches the result.
    ///
    /// \sa any
    class type_id {
    public:
        /// Constructs an id that denotes no type. Two such ids compare equal.
        constexpr type_id() = default;

        /// Returns the id of \a T. Const and reference qualifiers are removed. \c int and
        /// \c const int& therefore have the same id.
        template <class T>
        static type_id of() {
            return type_id(&detail::entry_of<std::decay_t<T>>());
        }

        /// Returns the compiler's name of the type, or an empty view if the id denotes no type.
        ///
        /// The name is intended for diagnostics and logging. Because the spelling differs
        /// between compilers, the name is not suitable for storage and later lookup.
        std::string_view name() const noexcept {
            return _entry ? _entry->name : std::string_view();
        }

        explicit operator bool() const noexcept {
            return _entry != nullptr;
        }

        friend bool operator==(type_id LHS, type_id RHS) {
            if (LHS._entry == RHS._entry) {
                return true;
            }
            if (!LHS._entry || !RHS._entry) {
                return false;
            }
            return detail::resolve_type_id(*LHS._entry) == detail::resolve_type_id(*RHS._entry);
        }

        friend bool operator!=(type_id LHS, type_id RHS) {
            return !(LHS == RHS);
        }

    private:
        explicit type_id(detail::type_entry *entry) noexcept : _entry(entry) {
        }

        // Returns the address that represents this type. A hash must be computed from this
        // address, so that two ids that compare equal also have equal hashes.
        const void *canonical() const {
            return _entry ? detail::resolve_type_id(*_entry) : nullptr;
        }

        detail::type_entry *_entry = nullptr;

        friend struct std::hash<type_id>;
    };

    /// @}
}

template <>
struct std::hash<stdc::type_id> {
    size_t operator()(stdc::type_id id) const {
        return std::hash<const void *>()(id.canonical());
    }
};

#endif // STDCORELIB_TYPE_ID_H
