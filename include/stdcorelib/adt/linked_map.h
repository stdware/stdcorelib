// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_LINKED_MAP_H
#define STDCORELIB_LINKED_MAP_H

#include <list>
#include <vector>
#include <unordered_map>
#include <utility>
#include <cstddef>

#ifdef QT_CORE_LIB
#  include <QList>
#  include <QVector>
#endif

namespace stdc {

    /// \addtogroup containers
    /// @{

    template <class K, class V, template <class, class, class...> class Map = std::unordered_map,
              class... Mods>
    class linked_map {
    private:
        std::list<std::pair<const K, V>, typename Map<K, V>::allocator_type> m_list;
        Map<K, typename decltype(m_list)::iterator, Mods...> m_map;

    public:
        using ListType = decltype(m_list);
        using MapType = decltype(m_map);

        using key_type = K;
        using mapped_type = V;
        using value_type = typename ListType::value_type;
        using size_type = size_t;
        using difference_type = typename ListType::difference_type;
        using allocator_type = typename MapType::allocator_type;
        using reference = value_type &;
        using const_reference = const value_type &;
        using pointer = typename ListType::pointer;
        using const_pointer = typename ListType::const_pointer;

        linked_map() = default;

        linked_map(const linked_map &other) {
            for (const auto &item : other.m_list) {
                append(item.first, item.second);
            }
        }

        linked_map(linked_map &&other) noexcept {
            m_list = std::move(other.m_list);
            m_map = std::move(other.m_map);
        }

        linked_map &operator=(const linked_map &other) {
            if (this == &other) {
                return *this;
            }
            clear();
            for (const auto &item : other.m_list) {
                append(item.first, item.second);
            }
            return *this;
        }

        linked_map &operator=(linked_map &&other) noexcept {
            if (this == &other) {
                return *this;
            }
            m_list = std::move(other.m_list);
            m_map = std::move(other.m_map);
            return *this;
        }

        linked_map(std::initializer_list<std::pair<K, V>> list)
            : linked_map(list.begin(), list.end()) {
        }

        template <typename InputIterator>
        linked_map(InputIterator f, InputIterator l) {
            for (; f != l; ++f)
                append(f->first, f->second);
        }

        inline bool operator==(const linked_map &other) const {
            return m_list == other.m_list;
        }

        inline bool operator!=(const linked_map &other) const {
            return m_list != other.m_list;
        }

        void swap(linked_map &other) noexcept {
            std::swap(m_list, other.m_list);
            std::swap(m_map, other.m_map);
        }

        // clang-format off
        class iterator {
        public:
            iterator() = default;

            using iterator_category = typename ListType::iterator::iterator_category;
            using value_type        = typename ListType::value_type;
            using difference_type   = typename ListType::difference_type;
            using pointer           = typename ListType::pointer;
            using reference         = value_type &;

            inline std::pair<const K, V> &operator*() const { return *i; }
            inline std::pair<const K, V> *operator->() const { return &(*i); }
            inline bool operator==(const iterator &o) const { return i == o.i; }
            inline bool operator!=(const iterator &o) const { return i != o.i; }
            inline iterator &operator++() { i++; return *this; }
            inline iterator operator++(int) { iterator r = *this; i++; return r; }
            inline iterator &operator--() { i--; return *this; }
            inline iterator operator--(int) { iterator r = *this; i--; return r; }

            inline const K &key() const { return i->first; }
            inline V &value() const { return i->second; }

        private:
            explicit iterator(const typename ListType::iterator &i) : i(i) {
            }
            typename ListType::iterator i;
            friend class linked_map;
            friend class const_iterator;
        };

        class const_iterator {
        public:
            using iterator_category = typename ListType::const_iterator::iterator_category;
            using value_type        = typename ListType::value_type;
            using difference_type   = typename ListType::difference_type;
            using pointer           = typename ListType::const_pointer;
            using reference         = const value_type &;

            const_iterator() = default;
            const_iterator(const iterator &it) : i(it.i) {
            }
            inline const std::pair<const K, V> &operator*() const { return *i; }
            inline const std::pair<const K, V> *operator->() const { return &(*i); }
            inline bool operator==(const const_iterator &o) const { return i == o.i; }
            inline bool operator!=(const const_iterator &o) const { return i != o.i; }
            inline const_iterator &operator++() { i++; return *this; }
            inline const_iterator operator++(int) { const_iterator r = *this; i++; return r; }
            inline const_iterator &operator--() { i--; return *this; }
            inline const_iterator operator--(int) { const_iterator r = *this; i--; return r; }

            inline const K &key() const { return i->first; }
            inline const V &value() const { return i->second; }

        private:
            explicit const_iterator(const typename ListType::const_iterator &i) : i(i) {
            }
            typename ListType::const_iterator i;
            friend class linked_map;
        };
        // clang-format on

        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        std::pair<iterator, bool> append(const K &key, const V &val) {
            return insert_impl(m_list.end(), key, val);
        }

        std::pair<iterator, bool> append(const K &key, V &&val) {
            return insert_impl(m_list.end(), key, std::forward<V>(val));
        }

        std::pair<iterator, bool> prepend(const K &key, const V &val) {
            return insert_impl(m_list.begin(), key, val);
        }

        std::pair<iterator, bool> prepend(const K &key, V &&val) {
            return insert_impl(m_list.begin(), key, std::forward<V>(val));
        }

        std::pair<iterator, bool> insert(const const_iterator &it, const K &key, const V &val) {
            return insert_impl(it.i, key, val);
        }

        std::pair<iterator, bool> insert(const const_iterator &it, const K &key, V &&val) {
            return insert_impl(it.i, key, std::forward<V>(val));
        }

        V &operator[](const K &key) {
            return append(key, V{}).first->second;
        }

        bool remove(const K &key) {
            auto it = m_map.find(key);
            if (it == m_map.end()) {
                return false;
            }
            m_list.erase(it->second);
            m_map.erase(it);
            return true;
        }

        size_t erase(const K &key) {
            return remove(key) ? 1 : 0;
        }

        iterator erase(const iterator &it) {
            return erase(const_iterator(it));
        }

        iterator erase(const const_iterator &it) {
            auto it2 = m_map.find(it.key());
            if (it2 == m_map.end()) {
                return iterator();
            }
            auto res = std::next(it2->second);
            m_list.erase(it2->second);
            m_map.erase(it2);
            return iterator(res);
        }

        iterator find(const K &key) {
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                return iterator(it->second);
            }
            return end();
        }

        const_iterator find(const K &key) const {
            auto it = m_map.find(key);
            if (it != m_map.cend()) {
                return const_iterator(it->second);
            }
            return cend();
        }

        V value(const K &key, const V &defaultValue = V{}) const {
            auto it = m_map.find(key);
            if (it != m_map.end()) {
                return (*it->second).second;
            }
            return defaultValue;
        }

        // clang-format off
        inline iterator begin() { return iterator(m_list.begin()); }
        inline const_iterator begin() const { return const_iterator(m_list.cbegin()); }
        inline const_iterator cbegin() const { return const_iterator(m_list.cbegin()); }
        inline iterator end() { return iterator(m_list.end()); }
        inline const_iterator end() const { return const_iterator(m_list.cend()); }
        inline const_iterator cend() const { return const_iterator(m_list.cend()); }
        reverse_iterator rbegin() { return reverse_iterator(end()); }
        reverse_iterator rend() { return reverse_iterator(begin()); }
        const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
        const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
        const_reverse_iterator crbegin() const { return const_reverse_iterator(end()); }
        const_reverse_iterator crend() const { return const_reverse_iterator(begin()); }
        inline bool contains(const K &key) const { return m_map.find(key) != m_map.end(); }
        inline size_t size() const { return m_list.size(); }
        inline bool empty() const { return m_list.empty(); }
        inline void clear() { m_list.clear(); m_map.clear(); }
        // clang-format on

        std::vector<K> keys() const {
            std::vector<K> res;
            res.reserve(m_list.size());
            for (const auto &item : std::as_const(m_list)) {
                res.push_back(item.first);
            }
            return res;
        }
#ifdef QT_CORE_LIB
        QList<K> keys_qlist() const {
            QList<K> res;
            res.reserve(m_list.size());
            for (const auto &item : std::as_const(m_list)) {
                res.push_back(item.first);
            }
            return res;
        }
        QVector<K> keys_qvector() const {
            QVector<K> res;
            res.reserve(m_list.size());
            for (const auto &item : std::as_const(m_list)) {
                res.push_back(item.first);
            }
            return res;
        }
#endif
        std::vector<V> values() const {
            std::vector<V> res;
            res.reserve(m_list.size());
            for (const auto &item : std::as_const(m_list)) {
                res.push_back(item.second);
            }
            return res;
        }
#ifdef QT_CORE_LIB
        QList<V> values_qlist() const {
            QList<V> res;
            res.reserve(m_list.size());
            for (const auto &item : std::as_const(m_list)) {
                res.push_back(item.second);
            }
            return res;
        }
        QVector<V> values_qvector() const {
            QVector<V> res;
            res.reserve(m_list.size());
            for (const auto &item : std::as_const(m_list)) {
                res.push_back(item.second);
            }
            return res;
        }
#endif
        size_t capacity() const {
            return m_map.capacity();
        }

        void reserve(size_t size) {
            m_map.reserve(size);
        }

    private:
        std::pair<iterator, bool> insert_impl(typename ListType::const_iterator it, const K &key,
                                              const V &val) {
            auto res = m_map.insert(std::make_pair(key, m_list.end()));
            auto &org_it = res.first->second;
            if (res.second) {
                // key doesn't exist
                auto new_it = m_list.emplace(it, key, val);
                org_it = new_it;
                return std::make_pair(iterator(new_it), true);
            }
            return std::make_pair(iterator(org_it), false);
        }

        std::pair<iterator, bool> insert_impl(typename ListType::const_iterator it, const K &key,
                                              V &&val) {
            auto res = m_map.insert(std::make_pair(key, m_list.end()));
            auto &org_it = res.first->second;
            if (res.second) {
                // key doesn't exist
                auto new_it = m_list.emplace(it, key, std::forward<V>(val));
                org_it = new_it;
                return std::make_pair(iterator(new_it), true);
            }
            return std::make_pair(iterator(org_it), false);
        }
    };

    /// @}
}

#endif // STDCORELIB_LINKED_MAP_H
