// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_VLA_H
#define STDCORELIB_VLA_H

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#  define STDC_ALLOCA(size) _alloca(size)
#elif defined(__GNUC__) || defined(__clang__)
#  define STDC_ALLOCA(size) alloca(size)
#endif

#ifdef STDC_ALLOCA

/// Allocates an uninitialized buffer of \a SIZE elements of \a TYPE on the stack, and declares
/// \a NAME as a pointer to it.
///
/// \warning The storage is released when the enclosing function returns, not at the end of the
///          enclosing scope. A loop that allocates in every iteration therefore enlarges the stack
///          frame in every iteration. Because nothing releases the storage earlier, the caller
///          must bound the size.
#  define STDC_VLA_ALLOC(TYPE, NAME, SIZE) TYPE *NAME = (TYPE *) STDC_ALLOCA((SIZE) * sizeof(TYPE))

namespace stdc::vla::detail {

    template <class T>
    struct ScopeGuard {
        inline ScopeGuard(T *buf, size_t size) : buf_(buf), size_(size) {
            auto buf_end = buf + size;
            for (auto p = buf; p < buf_end; ++p) {
                new (p) T();
            }
        }
        inline ~ScopeGuard() {
            auto buf_end = buf_ + size_;
            for (auto p = buf_; p < buf_end; ++p) {
                p->~T();
            }
        }

    private:
        T *buf_;
        size_t size_;
    };

}

/// Allocates like STDC_VLA_ALLOC(), for a type that requires construction. A guard in the same
/// scope default-constructs the elements and destroys them at the end of the scope.
///
/// \sa STDC_VLA_ALLOC()
#  define STDC_VLA_NEW(TYPE, NAME, SIZE)                                                           \
      const size_t NAME##_vla_size_ = (SIZE);                                                      \
      STDC_VLA_ALLOC(TYPE, NAME, NAME##_vla_size_);                                                \
      ::stdc::vla::detail::ScopeGuard<TYPE> NAME##_vla_guard_(NAME, NAME##_vla_size_);

#endif

#endif // STDCORELIB_VLA_H
