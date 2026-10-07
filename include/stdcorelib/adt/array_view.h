// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_ARRAY_VIEW_H
#define STDCORELIB_ARRAY_VIEW_H

#include <array>
#include <optional>
#include <type_traits>
#include <vector>
#include <cassert>

/// \defgroup containers Containers and views
///
/// stdc::array_view is a read-only view of any contiguous container, so that one parameter
/// replaces a set of overloads. stdc::vlarray stores its first N elements inline and allocates no
/// heap memory while it is small. stdc::linked_map preserves insertion order on top of
/// \c std::unordered_map or \c std::map. stdc::aligned_allocator provides storage with a fixed
/// minimum alignment. stdc::any stores a value of any type and returns it without RTTI.
///
/// \code
///     stdc::vlarray<int, 16> v;   // no allocation before the seventeenth element
/// \endcode

namespace stdc {

    /// \addtogroup containers
    /// @{

    /// A read-only view of a contiguous array, similar to \c std::span<const \c T> of C++20.
    ///
    /// The view converts implicitly from a \c std::vector, a \c std::array, a C array, a pointer
    /// and a length, or a single object. A function can therefore accept an array_view parameter
    /// instead of one overload per container.
    ///
    /// \warning The view never owns the array, and the array must outlive the view. A view bound
    ///          to a temporary dangles at the end of the full expression.
    template <class T>
    class array_view {
    public:
        using value_type = T;
        using pointer = value_type *;
        using const_pointer = const value_type *;
        using reference = value_type &;
        using const_reference = const value_type &;
        using iterator = const_pointer;
        using const_iterator = const_pointer;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;
        using size_type = size_t;
        using difference_type = ptrdiff_t;

    public:
        array_view() = default;

        array_view(std::nullopt_t) {
        }

        array_view(const T &item) : _data(&item), _size(1) {
        }

        constexpr array_view(const T *data, size_t length) : _data(data), _size(length) {
        }

        constexpr array_view(const T *begin, const T *end) : _data(begin), _size(end - begin) {
            assert(begin <= end);
        }

        template <template <class, class...> class V, class... A>
        array_view(const V<T, A...> &vec) : _data(vec.data()), _size(vec.size()) {
        }

        template <size_t N>
        constexpr array_view(const std::array<T, N> &Arr) : _data(Arr.data()), _size(N) {
        }

        template <size_t N>
        constexpr array_view(const T (&Arr)[N]) : _data(Arr), _size(N) {
        }

#if defined(__GNUC__) && __GNUC__ >= 9
// Suppresses the GCC warning in this constructor, which produces a large number of messages. The
// class documentation states that a view does not extend the lifetime of its array.
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Winit-list-lifetime"
#endif
        constexpr array_view(std::initializer_list<T> vec)
            : _data(vec.begin() == vec.end() ? (T *) nullptr : vec.begin()), _size(vec.size()) {
        }
#if defined(__GNUC__) && __GNUC__ >= 9
#  pragma GCC diagnostic pop
#endif

        template <typename T1>
        array_view(const array_view<T1 *> &RHS,
                   std::enable_if_t<std::is_convertible<T1 *const *, T const *>::value> * = nullptr)
            : _data(RHS.data()), _size(RHS.size()) {
        }

    public:
        iterator begin() const {
            return _data;
        }
        iterator end() const {
            return _data + _size;
        }
        reverse_iterator rbegin() const {
            return reverse_iterator(end());
        }
        reverse_iterator rend() const {
            return reverse_iterator(begin());
        }
        bool empty() const {
            return _size == 0;
        }
        const T *data() const {
            return _data;
        }
        size_t size() const {
            return _size;
        }

    public:
        const T &front() const {
            assert(!empty());
            return _data[0];
        }
        const T &back() const {
            assert(!empty());
            return _data[_size - 1];
        }
        bool equals(const array_view &RHS) const {
            if (_size != RHS._size)
                return false;
            return std::equal(begin(), end(), RHS.begin());
        }

        /// \name Slicing
        /// @{

        /// Returns a view of the \a j elements that follow the first \a i elements.
        ///
        /// \pre <tt>i + j <= size()</tt>
        array_view<T> slice(size_t i, size_t j) const {
            assert(i + j <= size() && "Invalid specifier");
            return array_view<T>(data() + i, j);
        }

        /// Returns a view of the elements that follow the first \a i elements.
        array_view<T> slice(size_t i) const {
            return drop_front(i);
        }

        /// Returns a view without the first \a i elements.
        ///
        /// \pre <tt>i <= size()</tt>
        array_view<T> drop_front(size_t i = 1) const {
            assert(size() >= i && "Dropping more elements than exist");
            return slice(i, size() - i);
        }

        /// Returns a view without the last \a i elements.
        ///
        /// \pre <tt>i <= size()</tt>
        array_view<T> drop_back(size_t i = 1) const {
            assert(size() >= i && "Dropping more elements than exist");
            return slice(0, size() - i);
        }

        /// Returns a view of the first \a i elements, or of all elements if fewer exist.
        array_view<T> take_front(size_t i = 1) const {
            if (i >= size())
                return *this;
            return drop_back(size() - i);
        }

        /// Returns a view of the last \a i elements, or of all elements if fewer exist.
        array_view<T> take_back(size_t i = 1) const {
            if (i >= size())
                return *this;
            return drop_front(size() - i);
        }

        /// @}

        /// \name Operator overloads
        /// @{

        const T &operator[](size_t index) const {
            assert(index < _size && "Invalid index!");
            return _data[index];
        }

        /// These operators are deleted, because assigning a temporary leaves the view dangling.
        ///
        /// The declaration is constrained so that <tt>view = {}</tt> still selects the move
        /// assignment operator.
        template <typename T1>
        std::enable_if_t<std::is_same<T1, T>::value, array_view<T>> &
            operator=(T1 &&Temporary) = delete;

        /// \overload
        template <typename T1>
        std::enable_if_t<std::is_same<T1, T>::value, array_view<T>> &
            operator=(std::initializer_list<T1>) = delete;

        /// @}

        /// Returns a copy of the elements, which the caller owns.
        std::vector<T> vec() const {
            return std::vector<T>(_data, _data + _size);
        }

    private:
        const T *_data = nullptr;
        size_type _size = 0;
    };


    namespace detail {

        // The comparisons with a container are constrained to exclude array_view itself. Since
        // C++17 relaxed the matching of template template parameters, V binds to array_view with
        // an empty pack. Without the constraint, an array_view on both sides matches the
        // container overload as exactly as the array_view overload, and partial ordering cannot
        // distinguish them. MSVC selects one overload, and Clang reports an ambiguity.
        template <class V, class T>
        using enable_if_not_array_view = std::enable_if_t<!std::is_same_v<V, array_view<T>>, int>;

    }

    template <typename T>
    inline bool operator==(array_view<T> LHS, array_view<T> RHS) {
        return LHS.equals(RHS);
    }

    template <template <class, class...> class V, typename T, class... A,
              detail::enable_if_not_array_view<V<T, A...>, T> = 0>
    inline bool operator==(const V<T, A...> &LHS, array_view<T> RHS) {
        return array_view<T>(LHS).equals(RHS);
    }

    template <template <class, class...> class V, typename T, class... A,
              detail::enable_if_not_array_view<V<T, A...>, T> = 0>
    inline bool operator==(array_view<T> LHS, const V<T, A...> &RHS) {
        return LHS.equals(array_view<T>(RHS));
    }

    template <typename T>
    inline bool operator!=(array_view<T> LHS, array_view<T> RHS) {
        return !(LHS == RHS);
    }

    template <template <class, class...> class V, typename T, class... A,
              detail::enable_if_not_array_view<V<T, A...>, T> = 0>
    inline bool operator!=(const V<T, A...> &LHS, array_view<T> RHS) {
        return !(LHS == RHS);
    }

    template <template <class, class...> class V, typename T, class... A,
              detail::enable_if_not_array_view<V<T, A...>, T> = 0>
    inline bool operator!=(array_view<T> LHS, const V<T, A...> &RHS) {
        return !(LHS == RHS);
    }

    /// @}
}

#endif // STDCORELIB_ARRAY_VIEW_H
