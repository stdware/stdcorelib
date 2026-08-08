// SPDX-License-Identifier: MIT

#include <stdcorelib/adt/vlarray.h>

#include <string>
#include <type_traits>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace stdc;

BOOST_AUTO_TEST_SUITE(test_vlarray)

namespace {

    // Counts live instances so leaks and double-destructions show up as a non-zero g_count.
    class Counted {
    public:
        Counted(int i = 0) : i(i) {
            g_count++;
        }
        Counted(const Counted &other) : Counted(other.i) {
        }
        Counted(Counted &&other) noexcept : Counted(other.i) {
            other.i = -1;
        }
        Counted &operator=(const Counted &other) {
            i = other.i;
            return *this;
        }
        Counted &operator=(Counted &&other) noexcept {
            i = other.i;
            other.i = -1;
            return *this;
        }
        ~Counted() {
            g_count--;
        }

        bool operator==(const Counted &other) const {
            return i == other.i;
        }

        int i;

        static inline int g_count = 0;
    };

    struct ThrowingMove {
        ThrowingMove() = default;
        ThrowingMove(const ThrowingMove &) = default;
        ThrowingMove(ThrowingMove &&) noexcept(false) {
        }
        ThrowingMove &operator=(const ThrowingMove &) = default;
        ThrowingMove &operator=(ThrowingMove &&) noexcept(false) {
            return *this;
        }
    };

    static_assert(std::is_nothrow_move_constructible_v<vlarray<Counted, 4>>);
    static_assert(std::is_nothrow_move_assignable_v<vlarray<Counted, 4>>);
    static_assert(!std::is_nothrow_move_constructible_v<vlarray<ThrowingMove, 4>>);
    static_assert(!std::is_nothrow_move_assignable_v<vlarray<ThrowingMove, 4>>);

}

BOOST_AUTO_TEST_CASE(test_construct) {
    // default
    {
        vlarray<Counted, 4> v;
        BOOST_CHECK(v.empty());
        BOOST_CHECK_EQUAL(v.size(), 0u);
        BOOST_CHECK_EQUAL(v.capacity(), 4u);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // size
    {
        vlarray<Counted, 4> v(3);
        BOOST_CHECK_EQUAL(v.size(), 3u);
        BOOST_CHECK_EQUAL(v[2].i, 0);
        BOOST_CHECK_EQUAL(Counted::g_count, 3);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // size and value
    {
        vlarray<Counted, 2> v(5, Counted(7));
        BOOST_CHECK_EQUAL(v.size(), 5u);
        BOOST_CHECK_EQUAL(v[4].i, 7);
        BOOST_CHECK_EQUAL(Counted::g_count, 5);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // initializer list
    {
        vlarray<Counted, 4> v = {0, 1, 2, 3};
        BOOST_CHECK_EQUAL(v.size(), 4u);
        BOOST_CHECK_EQUAL(v[2].i, 2);
        BOOST_CHECK_EQUAL(Counted::g_count, 4);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // iterator range
    {
        std::vector<int> src = {1, 2, 3};
        vlarray<int, 2> v(src.begin(), src.end());
        BOOST_CHECK_EQUAL(v.size(), 3u);
        BOOST_CHECK_EQUAL(v[1], 2);
    }
}

BOOST_AUTO_TEST_CASE(test_copy_move) {
    // copy construct
    {
        vlarray<Counted, 4> v0 = {0, 1, 2, 3};
        vlarray<Counted, 4> v(v0);
        BOOST_CHECK_EQUAL(v[2].i, 2);
        BOOST_CHECK_EQUAL(v0[2].i, 2); // source untouched
        BOOST_CHECK_EQUAL(Counted::g_count, 8);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // move construct, inline (elements are moved one by one)
    {
        vlarray<Counted, 4> v0 = {0, 1, 2, 3};
        vlarray<Counted, 4> v(std::move(v0));
        BOOST_CHECK_EQUAL(v.size(), 4u);
        BOOST_CHECK_EQUAL(v[2].i, 2);
        BOOST_CHECK_EQUAL(v0.size(), 0u);
        BOOST_CHECK_EQUAL(Counted::g_count, 4);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // move construct, heap (the buffer is stolen outright)
    {
        vlarray<Counted, 2> v0 = {0, 1, 2, 3};
        auto *data = v0.data();
        vlarray<Counted, 2> v(std::move(v0));
        BOOST_CHECK_EQUAL(v.data(), data);
        BOOST_CHECK_EQUAL(v[2].i, 2);
        BOOST_CHECK_EQUAL(v0.size(), 0u);
        BOOST_CHECK_EQUAL(Counted::g_count, 4);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // copy assign
    {
        vlarray<Counted, 4> v0 = {0, 1, 2, 3};
        vlarray<Counted, 4> v = {9};
        v = v0;
        BOOST_CHECK_EQUAL(v.size(), 4u);
        BOOST_CHECK_EQUAL(v[2].i, 2);
        BOOST_CHECK_EQUAL(Counted::g_count, 8);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // move assign
    {
        vlarray<Counted, 4> v0 = {0, 1, 2, 3};
        vlarray<Counted, 4> v = {9};
        v = std::move(v0);
        BOOST_CHECK_EQUAL(v.size(), 4u);
        BOOST_CHECK_EQUAL(v[2].i, 2);
        BOOST_CHECK_EQUAL(Counted::g_count, 4);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // self assign
    {
        vlarray<Counted, 4> v = {0, 1, 2};
        v = v;
        BOOST_CHECK_EQUAL(v.size(), 3u);
        BOOST_CHECK_EQUAL(v[1].i, 1);
        BOOST_CHECK_EQUAL(Counted::g_count, 3);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);
}

// A vlarray of any inline size binds to the same base reference.
BOOST_AUTO_TEST_CASE(test_cross_size) {
    const auto &sum = [](const vlarray_base<int> &v) {
        int n = 0;
        for (int i : v) {
            n += i;
        }
        return n;
    };

    vlarray<int, 2> v2 = {1, 2, 3, 4};
    vlarray<int, 64> v64 = {1, 2, 3, 4};
    BOOST_CHECK_EQUAL(sum(v2), 10);
    BOOST_CHECK_EQUAL(sum(v64), 10);

    // construct and assign across inline sizes
    vlarray<int, 8> v8(v2);
    BOOST_CHECK(v8 == v64);

    v8 = v64;
    BOOST_CHECK_EQUAL(sum(v8), 10);

    vlarray<int, 1> v1 = {5};
    BOOST_CHECK(v1 != v8);
}

BOOST_AUTO_TEST_CASE(test_grow) {
    vlarray<Counted, 4> v;
    const auto *inline_data = v.data();

    // stays inline up to N
    for (int i = 0; i < 4; ++i) {
        v.push_back(i);
    }
    BOOST_CHECK_EQUAL(v.data(), inline_data);
    BOOST_CHECK_EQUAL(v.capacity(), 4u);

    // the next push moves to the heap
    v.push_back(4);
    BOOST_CHECK(v.data() != inline_data);
    BOOST_CHECK(v.capacity() >= 5u);
    BOOST_CHECK_EQUAL(Counted::g_count, 5);
    for (int i = 0; i < 5; ++i) {
        BOOST_CHECK_EQUAL(v[i].i, i);
    }

    // emplace_back with an argument that aliases an existing element
    v.reserve(v.size());
    v.emplace_back(v[0]);
    BOOST_CHECK_EQUAL(v.back().i, 0);

    v.clear();
    BOOST_CHECK(v.empty());
    BOOST_CHECK_EQUAL(Counted::g_count, 0);
}

BOOST_AUTO_TEST_CASE(test_resize) {
    vlarray<Counted, 4> v = {0, 1, 2, 3};
    BOOST_CHECK_EQUAL(Counted::g_count, 4);

    // inline shrink
    v.resize(2);
    BOOST_CHECK_EQUAL(v.size(), 2u);
    BOOST_CHECK_EQUAL(Counted::g_count, 2);

    // inline expand
    v.resize(4);
    BOOST_CHECK_EQUAL(v[3].i, 0);
    BOOST_CHECK_EQUAL(Counted::g_count, 4);

    // expand onto the heap
    v.resize(20);
    BOOST_CHECK_EQUAL(v[1].i, 1); // original elements survive the relocation
    BOOST_CHECK_EQUAL(v[15].i, 0);
    BOOST_CHECK_EQUAL(Counted::g_count, 20);

    // expand with a value
    v.resize(25, Counted(8));
    BOOST_CHECK_EQUAL(v[24].i, 8);
    BOOST_CHECK_EQUAL(Counted::g_count, 25);

    // shrink back below N: the capacity stays, the elements are destroyed
    v.resize(2);
    BOOST_CHECK_EQUAL(v.size(), 2u);
    BOOST_CHECK_EQUAL(Counted::g_count, 2);
}

BOOST_AUTO_TEST_CASE(test_insert_erase) {
    vlarray<int, 4> v = {0, 1, 2, 3};

    // single value
    v.insert(v.begin(), -1);
    BOOST_CHECK_EQUAL(v[0], -1);
    BOOST_CHECK_EQUAL(v[1], 0);
    BOOST_CHECK_EQUAL(v.size(), 5u);

    // a value that lives inside the array
    v.insert(v.begin(), v[4]);
    BOOST_CHECK_EQUAL(v[0], 3);
    BOOST_CHECK_EQUAL(v.size(), 6u);

    // count and value
    v.insert(v.end(), 3, 7);
    BOOST_CHECK_EQUAL(v.size(), 9u);
    BOOST_CHECK_EQUAL(v.back(), 7);

    // range
    std::vector<int> src = {8, 9};
    v.insert(v.begin(), src.begin(), src.end());
    BOOST_CHECK_EQUAL(v[0], 8);
    BOOST_CHECK_EQUAL(v[1], 9);
    BOOST_CHECK_EQUAL(v.size(), 11u);

    // erase one
    v.erase(v.begin());
    BOOST_CHECK_EQUAL(v[0], 9);
    BOOST_CHECK_EQUAL(v.size(), 10u);

    // erase a range
    v.erase(v.begin(), v.begin() + 3);
    BOOST_CHECK_EQUAL(v.size(), 7u);

    v.pop_back();
    BOOST_CHECK_EQUAL(v.size(), 6u);

    // element lifetime through insert/erase
    {
        vlarray<Counted, 2> c = {0, 1};
        c.insert(c.begin() + 1, 5);
        BOOST_CHECK_EQUAL(c[1].i, 5);
        BOOST_CHECK_EQUAL(Counted::g_count, 3);
        c.erase(c.begin());
        BOOST_CHECK_EQUAL(Counted::g_count, 2);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);
}

BOOST_AUTO_TEST_CASE(test_swap) {
    // both inline
    {
        vlarray<Counted, 8> a = {0, 1, 2};
        vlarray<Counted, 8> b = {9};
        a.swap(b);
        BOOST_CHECK_EQUAL(a.size(), 1u);
        BOOST_CHECK_EQUAL(a[0].i, 9);
        BOOST_CHECK_EQUAL(b.size(), 3u);
        BOOST_CHECK_EQUAL(b[2].i, 2);
        BOOST_CHECK_EQUAL(Counted::g_count, 4);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // both on the heap: the buffers are traded
    {
        vlarray<Counted, 1> a = {0, 1, 2};
        vlarray<Counted, 1> b = {7, 8};
        auto *adata = a.data();
        auto *bdata = b.data();
        swap(a, b);
        BOOST_CHECK_EQUAL(a.data(), bdata);
        BOOST_CHECK_EQUAL(b.data(), adata);
        BOOST_CHECK_EQUAL(a.size(), 2u);
        BOOST_CHECK_EQUAL(b.size(), 3u);
        BOOST_CHECK_EQUAL(Counted::g_count, 5);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);

    // one inline, one on the heap
    {
        vlarray<Counted, 8> a = {0, 1, 2};
        vlarray<Counted, 1> b = {7, 8};
        a.swap(b);
        BOOST_CHECK_EQUAL(a.size(), 2u);
        BOOST_CHECK_EQUAL(a[1].i, 8);
        BOOST_CHECK_EQUAL(b.size(), 3u);
        BOOST_CHECK_EQUAL(b[2].i, 2);
        BOOST_CHECK_EQUAL(Counted::g_count, 5);
    }
    BOOST_CHECK_EQUAL(Counted::g_count, 0);
}

BOOST_AUTO_TEST_CASE(test_nontrivial_element) {
    vlarray<std::string, 2> v;
    v.push_back("hello");
    v.emplace_back("world");
    v.push_back("!"); // grows onto the heap
    BOOST_CHECK_EQUAL(v.size(), 3u);
    BOOST_CHECK_EQUAL(v[0], "hello");
    BOOST_CHECK_EQUAL(v[1], "world");
    BOOST_CHECK_EQUAL(v.back(), "!");

    vlarray<std::string, 2> v2 = std::move(v);
    BOOST_CHECK_EQUAL(v2[0], "hello");
}

// The copy and move constructors build the new array from other.get_allocator(), so this had
// been running the whole time with nothing asserting what it hands back.
BOOST_AUTO_TEST_CASE(test_the_allocator_can_be_read_back) {
    vlarray<int, 4> array;
    array.push_back(1);
    array.push_back(2);

    // The reference is to the array's own allocator rather than a copy, so it stays the same one
    // across a call that grows past the inline buffer.
    const auto *before = &array.get_allocator();
    for (int i = 0; i < 32; ++i) {
        array.push_back(i);
    }
    BOOST_CHECK_EQUAL(before, &array.get_allocator());

    // A copy is built through it and answers with one of its own.
    vlarray<int, 4> copy = array;
    BOOST_CHECK(&copy.get_allocator() != &array.get_allocator());
    BOOST_CHECK_EQUAL(copy.size(), array.size());
}

BOOST_AUTO_TEST_SUITE_END()
