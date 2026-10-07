// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_ANY_H
#define STDCORELIB_ANY_H

#include <cstddef>
#include <exception>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/type_id.h>

namespace stdc {

    /// \addtogroup containers
    /// @{

    namespace detail {

        /// The number of bytes that an any stores without allocating heap memory.
        ///
        /// Two pointers accommodate the typical values of an unknown type: a number, a flag, a
        /// pointer, or a small structure of these. A wider value is stored on the heap, and the
        /// storage holds a pointer to it.
        constexpr size_t any_buffer_size = 2 * sizeof(void *);

        union any_storage {
            void *heap;
            // The union is deliberately not over-aligned. Aligning it to max_align_t rounds the
            // size of every any up to that alignment, whereas a type that requires more than
            // pointer alignment can be stored on the heap.
            alignas(void *) unsigned char buffer[any_buffer_size];
        };

        /// Indicates whether a \a T is stored in the buffer rather than on the heap.
        ///
        /// Moving an any moves the contents of its buffer. If a type can throw while being moved,
        /// that operation can throw as well. Such a type is therefore stored on the heap, where a
        /// move transfers a pointer and cannot fail.
        template <class T>
        constexpr bool any_fits_inline =
            sizeof(T) <= any_buffer_size && alignof(T) <= alignof(void *) &&
            std::is_nothrow_move_constructible_v<T>;

        template <class T, bool Inline = any_fits_inline<T>>
        struct any_handler;

        template <class T>
        struct any_handler<T, true> {
            template <class Arg>
            static void construct(any_storage &s, Arg &&arg) {
                ::new (static_cast<void *>(s.buffer)) T(std::forward<Arg>(arg));
            }
            static T *value(any_storage &s) noexcept {
                return std::launder(reinterpret_cast<T *>(s.buffer));
            }
            static void destroy(any_storage &s) noexcept {
                value(s)->~T();
            }
            static void copy(const any_storage &from, any_storage &to) {
                construct(to, *value(const_cast<any_storage &>(from)));
            }
            static void move(any_storage &from, any_storage &to) noexcept {
                construct(to, std::move(*value(from)));
                destroy(from);
            }
        };

        template <class T>
        struct any_handler<T, false> {
            template <class Arg>
            static void construct(any_storage &s, Arg &&arg) {
                s.heap = new T(std::forward<Arg>(arg));
            }
            static T *value(any_storage &s) noexcept {
                return static_cast<T *>(s.heap);
            }
            static void destroy(any_storage &s) noexcept {
                delete static_cast<T *>(s.heap);
            }
            static void copy(const any_storage &from, any_storage &to) {
                to.heap = new T(*static_cast<const T *>(from.heap));
            }
            static void move(any_storage &from, any_storage &to) noexcept {
                to.heap = from.heap; // the value remains on the heap, and only the pointer moves
                from.heap = nullptr;
            }
        };

        /// Returns the type_id of \a T. The function is named rather than written as a lambda in
        /// the table below, because MSVC excludes the conversion of a lambda to a function
        /// pointer from constant initialization, and the table then requires a guard.
        template <class T>
        type_id type_of() {
            return type_id::of<T>();
        }

        /// The operations that an any requires for the type of the stored value.
        ///
        /// One table exists per type and module. An any refers to the table through a pointer
        /// that is null if and only if the any is empty.
        ///
        /// The table has no entry for accessing the value. The type is a template argument of
        /// any_cast, which therefore calls any_handler<T>::value() directly.
        struct any_vtable {
            type_id (*type)();
            void (*destroy)(any_storage &) noexcept;
            void (*copy)(const any_storage &, any_storage &);
            void (*move)(any_storage &, any_storage &) noexcept;
        };

        template <class T>
        const any_vtable &vtable_of() noexcept {
            static const any_vtable table{
                &type_of<T>,
                &any_handler<T>::destroy,
                &any_handler<T>::copy,
                &any_handler<T>::move,
            };
            return table;
        }

    }

    class any;

    template <class T>
    const T *any_cast(const any *value) noexcept;

    template <class T>
    T *any_cast(any *value) noexcept;

    /// Stores a value of any copy-constructible type together with the identity of that type.
    ///
    /// The type is identified by the compiler's name for it rather than by \c typeid. The class
    /// therefore works without RTTI, and a value keeps its identity across a shared library
    /// boundary, at which a scheme that compares addresses fails. \c std::any solves the same
    /// problem in the standard library and is suitable if neither property is required.
    ///
    /// \code
    ///   any value = std::string("text");
    ///   if (auto *s = any_cast<std::string>(&value)) {
    ///       use(*s);
    ///   }
    /// \endcode
    ///
    /// \warning A value can be read only as its exact type. A value stored as \c Derived cannot
    ///          be read as \c Base. Const and reference qualifiers are removed when the value is
    ///          stored. \c int and \c const \c int& are therefore the same type here.
    ///
    /// \note A value of at most two pointers whose move cannot throw is stored inside the any.
    ///       Any other value is stored on the heap. In both cases the object consists of one
    ///       buffer and one pointer, which keeps a container of any objects small.
    ///
    /// \sa any_cast()
    class any {
    public:
        any() noexcept = default;

        ~any() {
            reset();
        }

        any(const any &RHS) {
            if (RHS._vtable) {
                RHS._vtable->copy(RHS._storage, _storage);
                _vtable = RHS._vtable;
            }
        }

        any(any &&RHS) noexcept {
            adopt(RHS);
        }

        /// Constructs an any that holds \a value, which may be of any type other than \c any.
        ///
        /// The constructor is disabled for \c any, so that copying selects the copy constructor.
        /// It is also disabled for every type to which \c any converts, because the compiler
        /// otherwise recurses while determining whether that type is copy-constructible.
        template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, any> &&
                                                !std::is_convertible_v<any, std::decay_t<T>> &&
                                                std::is_copy_constructible_v<std::decay_t<T>>,
                                            int> = 0>
        any(T &&value) {
            detail::any_handler<std::decay_t<T>>::construct(_storage, std::forward<T>(value));
            _vtable = &detail::vtable_of<std::decay_t<T>>();
        }

        any &operator=(any RHS) noexcept {
            swap(RHS);
            return *this;
        }

        inline bool has_value() const noexcept {
            return _vtable != nullptr;
        }

        inline void reset() noexcept {
            if (_vtable) {
                _vtable->destroy(_storage);
                _vtable = nullptr;
            }
        }

        /// \note This is not a pointer swap. A value stored in the buffer must be moved. The swap
        ///       therefore consists of three moves.
        ///
        /// ##QUESTION: Would separate inline and heap paths make this materially cheaper without
        /// complicating the storage invariants?
        ///
        /// \note The function is implemented directly rather than through the assignment
        ///       operator, because the assignment operator calls this function.
        void swap(any &RHS) noexcept {
            if (this == &RHS) {
                return;
            }
            any temp;
            temp.adopt(*this);
            adopt(RHS);
            RHS.adopt(temp);
        }

        /// Returns whether the stored value is a \a T.
        template <class T>
        bool holds() const {
            return _vtable && _vtable->type() == type_id::of<T>();
        }

        /// Returns the type of the stored value, or a default \c type_id if no value is stored.
        /// type_id::name() returns the compiler's spelling of the type.
        ///
        /// \sa type_id::name()
        inline type_id type() const {
            return _vtable ? _vtable->type() : type_id();
        }

    private:
        // Moves the value out of from and leaves from empty. The function is correct only if
        // this any holds no value.
        void adopt(any &from) noexcept {
            if (from._vtable) {
                from._vtable->move(from._storage, _storage);
                _vtable = from._vtable;
                from._vtable = nullptr;
            }
        }

        detail::any_storage _storage{};
        const detail::any_vtable *_vtable = nullptr;

        template <class T>
        friend const T *any_cast(const any *value) noexcept;
        template <class T>
        friend T *any_cast(any *value) noexcept;
    };

    /// \name Value access
    ///
    /// The pointer forms return \c nullptr if the type does not match and are the preferred
    /// forms. The value forms are a convenience for a caller that already knows the type.
    /// @{

    template <class T>
    const T *any_cast(const any *value) noexcept {
        using U = std::decay_t<T>;
        if (!value || !value->holds<U>()) {
            return nullptr;
        }
        return detail::any_handler<U>::value(const_cast<detail::any_storage &>(value->_storage));
    }

    template <class T>
    T *any_cast(any *value) noexcept {
        using U = std::decay_t<T>;
        if (!value || !value->holds<U>()) {
            return nullptr;
        }
        return detail::any_handler<U>::value(value->_storage);
    }

#ifdef STDC_HAS_EXCEPTIONS

    class bad_any_cast : public std::exception {
    public:
        inline const char *what() const noexcept override {
            return "stdc::bad_any_cast";
        }
    };

    /// \throws bad_any_cast if \a value does not hold a \a T
    template <class T>
    T any_cast(const any &value) {
        const auto *held = any_cast<std::remove_cv_t<std::remove_reference_t<T>>>(&value);
        if (!held) {
            throw bad_any_cast();
        }
        return static_cast<T>(*held);
    }

    /// \overload
    template <class T>
    T any_cast(any &value) {
        auto *held = any_cast<std::remove_cv_t<std::remove_reference_t<T>>>(&value);
        if (!held) {
            throw bad_any_cast();
        }
        return static_cast<T>(*held);
    }

#endif

    /// @}

    inline void swap(any &LHS, any &RHS) noexcept {
        LHS.swap(RHS);
    }

    /// @}
}

#endif // STDCORELIB_ANY_H
