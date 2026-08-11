// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_LINKED_MAP_H
#define STDCORELIB_LINKED_MAP_H

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <list>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <stdcorelib/stdc_global.h>

#ifdef QT_CORE_LIB
#  include <QList>
#  include <QVector>
#endif

namespace stdc {

    /// \addtogroup containers
    /// @{

    namespace detail {

        template <class>
        struct dependent_false : std::false_type {};

        template <class T, class = void>
        struct has_reserve : std::false_type {};

        template <class T>
        struct has_reserve<T, std::void_t<decltype(std::declval<T &>().reserve(
                                  std::declval<typename T::size_type>()))>> : std::true_type {};

        template <class T, class = void>
        struct has_capacity : std::false_type {};

        template <class T>
        struct has_capacity<T, std::void_t<decltype(std::declval<const T &>().capacity())>>
            : std::true_type {};

        template <class T>
        struct linked_map_index_traits {
            static_assert(dependent_false<T>::value,
                          "linked_map supports std::map and std::unordered_map indexes");
        };

        template <class K, class V, class Compare, class Allocator>
        struct linked_map_index_traits<std::map<K, V, Compare, Allocator>> {
            template <class Mapped>
            using allocator_for = typename std::allocator_traits<Allocator>::template rebind_alloc<
                std::pair<const K, Mapped>>;

            template <class Mapped>
            using rebind = std::map<K, Mapped, Compare, allocator_for<Mapped>>;

            template <class Mapped>
            static rebind<Mapped> make(const Allocator &allocator) {
                return rebind<Mapped>(Compare(), allocator_for<Mapped>(allocator));
            }

            template <class Mapped>
            static rebind<Mapped> copy_configuration(const rebind<Mapped> &other,
                                                     const Allocator &allocator) {
                return rebind<Mapped>(other.key_comp(), allocator_for<Mapped>(allocator));
            }
        };

        template <class K, class V, class Hash, class KeyEqual, class Allocator>
        struct linked_map_index_traits<std::unordered_map<K, V, Hash, KeyEqual, Allocator>> {
            template <class Mapped>
            using allocator_for = typename std::allocator_traits<Allocator>::template rebind_alloc<
                std::pair<const K, Mapped>>;

            template <class Mapped>
            using rebind = std::unordered_map<K, Mapped, Hash, KeyEqual, allocator_for<Mapped>>;

            template <class Mapped>
            static rebind<Mapped> make(const Allocator &allocator) {
                return rebind<Mapped>(0, Hash(), KeyEqual(), allocator_for<Mapped>(allocator));
            }

            template <class Mapped>
            static rebind<Mapped> copy_configuration(const rebind<Mapped> &other,
                                                     const Allocator &allocator) {
                rebind<Mapped> result(other.bucket_count(), other.hash_function(), other.key_eq(),
                                      allocator_for<Mapped>(allocator));
                result.max_load_factor(other.max_load_factor());
                return result;
            }
        };

    }

    /// An associative container that preserves insertion order.
    ///
    /// Values live in a list so their order and iterators remain stable. A \c std::map or
    /// \c std::unordered_map indexes the list by key. The index stores its own copy of each key.
    template <class K, class V, template <class, class, class...> class Map = std::unordered_map,
              class... Mods>
    class linked_map {
    public:
        using key_type = K;
        using mapped_type = V;
        using value_type = std::pair<const K, V>;

    private:
        using configured_map = Map<K, V, Mods...>;
        using index_traits = detail::linked_map_index_traits<configured_map>;
        using allocator_traits = std::allocator_traits<typename configured_map::allocator_type>;

    public:
        using list_allocator_type = typename allocator_traits::template rebind_alloc<value_type>;
        using list_type = std::list<value_type, list_allocator_type>;

        using allocator_type = typename configured_map::allocator_type;
        using iterator = typename list_type::iterator;
        using const_iterator = typename list_type::const_iterator;
        using reverse_iterator = typename list_type::reverse_iterator;
        using const_reverse_iterator = typename list_type::const_reverse_iterator;
        using size_type = typename list_type::size_type;
        using difference_type = typename list_type::difference_type;
        using reference = value_type &;
        using const_reference = const value_type &;
        using pointer = typename list_type::pointer;
        using const_pointer = typename list_type::const_pointer;

    public:
        using map_type = typename index_traits::template rebind<iterator>;

        linked_map() : linked_map(allocator_type()) {
        }

        explicit linked_map(const allocator_type &allocator)
            : _list(list_allocator_type(allocator)),
              _map(index_traits::template make<iterator>(allocator)) {
        }

        linked_map(const linked_map &other)
            : linked_map(other, allocator_traits::select_on_container_copy_construction(
                                    other.get_allocator())) {
        }

        linked_map(linked_map &&other) = default;

        linked_map &operator=(const linked_map &other) {
            if (this == &other) {
                return *this;
            }
            linked_map replacement(other, get_allocator());
            swap(replacement);
            return *this;
        }

        linked_map &
            operator=(linked_map &&other) noexcept(allocator_traits::is_always_equal::value &&
                                                   std::is_nothrow_move_assignable_v<map_type>) {
            if (this == &other) {
                return *this;
            }

            if constexpr (allocator_traits::is_always_equal::value) {
                move_from_equal_allocator(other);
            } else if (get_allocator() == other.get_allocator()) {
                move_from_equal_allocator(other);
            } else {
                linked_map replacement(empty_copy, other, get_allocator());
                for (auto &item : other._list) {
                    replacement.append(item.first, std::move(item.second));
                }
                swap(replacement);
                other.clear();
            }
            return *this;
        }

        linked_map(std::initializer_list<value_type> list) : linked_map(list.begin(), list.end()) {
        }

        template <class InputIterator>
        linked_map(InputIterator first, InputIterator last) : linked_map() {
            for (; first != last; ++first) {
                auto &&item = *first;
                append(item.first, std::forward<decltype((item.second))>(item.second));
            }
        }

        bool operator==(const linked_map &other) const {
            return _list == other._list;
        }

        bool operator!=(const linked_map &other) const {
            return !(*this == other);
        }

        void swap(linked_map &other) noexcept(
            noexcept(std::declval<list_type &>().swap(std::declval<list_type &>())) &&
            noexcept(std::declval<map_type &>().swap(std::declval<map_type &>()))) {
            assert(allocator_traits::propagate_on_container_swap::value ||
                   get_allocator() == other.get_allocator());
            _map.swap(other._map);
            _list.swap(other._list);
        }

        std::pair<iterator, bool> append(const K &key, const V &value) {
            return emplace_impl(_list.end(), key, value);
        }

        std::pair<iterator, bool> append(const K &key, V &&value) {
            return emplace_impl(_list.end(), key, std::move(value));
        }

        std::pair<iterator, bool> prepend(const K &key, const V &value) {
            return emplace_impl(_list.begin(), key, value);
        }

        std::pair<iterator, bool> prepend(const K &key, V &&value) {
            return emplace_impl(_list.begin(), key, std::move(value));
        }

        std::pair<iterator, bool> insert(const_iterator position, const K &key, const V &value) {
            return emplace_impl(position, key, value);
        }

        std::pair<iterator, bool> insert(const_iterator position, const K &key, V &&value) {
            return emplace_impl(position, key, std::move(value));
        }

        template <class... Args>
        std::pair<iterator, bool> try_emplace(const K &key, Args &&...args) {
            return emplace_impl(_list.end(), key, std::forward<Args>(args)...);
        }

        V &operator[](const K &key) {
            return try_emplace(key).first->second;
        }

        bool remove(const K &key) {
            auto found = _map.find(key);
            if (found == _map.end()) {
                return false;
            }
            auto position = found->second;
            _map.erase(found);
            _list.erase(position);
            return true;
        }

        size_type erase(const K &key) {
            return remove(key) ? 1 : 0;
        }

        iterator erase(iterator position) {
            return erase(const_iterator(position));
        }

        iterator erase(const_iterator position) {
            assert(position != _list.end());
            auto found = _map.find(position->first);
            assert(found != _map.end() && const_iterator(found->second) == position);
            _map.erase(found);
            return _list.erase(position);
        }

        iterator find(const K &key) {
            auto found = _map.find(key);
            return found == _map.end() ? end() : found->second;
        }

        const_iterator find(const K &key) const {
            auto found = _map.find(key);
            return found == _map.end() ? cend() : const_iterator(found->second);
        }

        V value(const K &key) const {
            auto found = _map.find(key);
            return found == _map.end() ? V() : found->second->second;
        }

        V value(const K &key, const V &defaultValue) const {
            auto found = _map.find(key);
            return found == _map.end() ? defaultValue : found->second->second;
        }

        iterator begin() noexcept {
            return _list.begin();
        }

        const_iterator begin() const noexcept {
            return _list.begin();
        }

        const_iterator cbegin() const noexcept {
            return _list.cbegin();
        }

        iterator end() noexcept {
            return _list.end();
        }

        const_iterator end() const noexcept {
            return _list.end();
        }

        const_iterator cend() const noexcept {
            return _list.cend();
        }

        reverse_iterator rbegin() noexcept {
            return _list.rbegin();
        }

        const_reverse_iterator rbegin() const noexcept {
            return _list.rbegin();
        }

        const_reverse_iterator crbegin() const noexcept {
            return _list.crbegin();
        }

        reverse_iterator rend() noexcept {
            return _list.rend();
        }

        const_reverse_iterator rend() const noexcept {
            return _list.rend();
        }

        const_reverse_iterator crend() const noexcept {
            return _list.crend();
        }

        bool contains(const K &key) const {
            return _map.find(key) != _map.end();
        }

        size_type size() const noexcept {
            return _list.size();
        }

        bool empty() const noexcept {
            return _list.empty();
        }

        void clear() noexcept {
            _map.clear();
            _list.clear();
        }

        allocator_type get_allocator() const noexcept {
            return allocator_type(_list.get_allocator());
        }

        std::vector<K> keys() const {
            std::vector<K> result;
            result.reserve(_list.size());
            for (const auto &item : _list) {
                result.push_back(item.first);
            }
            return result;
        }

#ifdef QT_CORE_LIB
        QList<K> keys_qlist() const {
            QList<K> result;
            result.reserve(_list.size());
            for (const auto &item : _list) {
                result.push_back(item.first);
            }
            return result;
        }

        QVector<K> keys_qvector() const {
            QVector<K> result;
            result.reserve(_list.size());
            for (const auto &item : _list) {
                result.push_back(item.first);
            }
            return result;
        }
#endif

        std::vector<V> values() const {
            std::vector<V> result;
            result.reserve(_list.size());
            for (const auto &item : _list) {
                result.push_back(item.second);
            }
            return result;
        }

#ifdef QT_CORE_LIB
        QList<V> values_qlist() const {
            QList<V> result;
            result.reserve(_list.size());
            for (const auto &item : _list) {
                result.push_back(item.second);
            }
            return result;
        }

        QVector<V> values_qvector() const {
            QVector<V> result;
            result.reserve(_list.size());
            for (const auto &item : _list) {
                result.push_back(item.second);
            }
            return result;
        }
#endif

        size_type capacity() const {
            if constexpr (detail::has_capacity<map_type>::value) {
                return static_cast<size_type>(_map.capacity());
            } else {
                static_assert(detail::dependent_false<map_type>::value,
                              "linked_map::capacity() is not supported by the index type");
                return 0;
            }
        }

        void reserve(size_type size) {
            if constexpr (detail::has_reserve<map_type>::value) {
                _map.reserve(size);
            } else {
                static_assert(detail::dependent_false<map_type>::value,
                              "linked_map::reserve() is not supported by the index type");
            }
        }

    private:
        struct empty_copy_t {};
        static constexpr empty_copy_t empty_copy{};

        linked_map(empty_copy_t, const linked_map &other, const allocator_type &allocator)
            : _list(list_allocator_type(allocator)),
              _map(index_traits::template copy_configuration<iterator>(other._map, allocator)) {
        }

        linked_map(const linked_map &other, const allocator_type &allocator)
            : linked_map(empty_copy, other, allocator) {
            for (const auto &item : other._list) {
                append(item.first, item.second);
            }
        }

        template <class... Args>
        std::pair<iterator, bool> emplace_impl(const_iterator position, const K &key,
                                               Args &&...args) {
            auto indexed = _map.emplace(key, _list.end());
            if (!indexed.second) {
                return {indexed.first->second, false};
            }

#ifdef STDC_HAS_EXCEPTIONS
            try {
#endif
                auto inserted =
                    _list.emplace(position, std::piecewise_construct, std::forward_as_tuple(key),
                                  std::forward_as_tuple(std::forward<Args>(args)...));
                indexed.first->second = inserted;
                return {inserted, true};
#ifdef STDC_HAS_EXCEPTIONS
            } catch (...) {
                _map.erase(indexed.first);
                throw;
            }
#endif
        }

        void move_from_equal_allocator(linked_map &other) {
            _map = std::move(other._map);
            _list.clear();
            _list.splice(_list.end(), other._list);
        }

        list_type _list;
        map_type _map;
    };

    /// @}
}

#endif // STDCORELIB_LINKED_MAP_H
