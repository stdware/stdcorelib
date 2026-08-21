// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_STATICREGISTRY_H
#define STDCORELIB_STATICREGISTRY_H

#include <memory>
#include <string_view>

#include <stdcorelib/stdc_global.h>

namespace stdc {

    /// \addtogroup types
    /// @{

    /// Controls what a \c StaticRegistry constructs for one registered implementation.
    ///
    /// The default returns an owning pointer so derived implementations retain their dynamic
    /// type. Specialize this trait when \a T is a small descriptor that should be returned by
    /// value instead.
    template <class T>
    struct static_registry_traits {
        using result_type = std::unique_ptr<T>;

        template <class V>
        static result_type construct() {
            return std::make_unique<V>();
        }
    };

    /// A registry that fills itself before \c main, so an implementation is available merely by
    /// having been linked in.
    ///
    /// Every registration is a static object whose constructor links it into a list. Nothing
    /// calls an initialization function and nothing keeps a list of what to initialize, which is
    /// what makes it work across a shared library boundary: a plugin's own static object joins
    /// the host's list as the plugin is loaded.
    ///
    /// \code
    ///   // in the host
    ///   class Codec { public: virtual ~Codec() = default; };
    ///   using CodecRegistry = stdc::StaticRegistry<Codec>;
    ///   STDC_INSTANTIATE_STATIC_REGISTRY(Codec)   // once, in one .cpp
    ///
    ///   // in the host or in any plugin
    ///   static CodecRegistry::Add<FlacCodec> x("flac", "Free Lossless Audio Codec");
    ///
    ///   for (const auto &entry : CodecRegistry::entries()) {
    ///       if (entry.name() == wanted) {
    ///           return entry.instantiate();
    ///       }
    ///   }
    /// \endcode
    ///
    /// The list is the only structure that can be built this way. A map or a vector has a
    /// constructor of its own, so touching one from a static constructor races with its own
    /// initialization, while the head and tail pointers here are constant-initialized and are
    /// therefore in place before any of the registrations run.
    ///
    /// \note For a plugin to reach the host's list, the host has to export the symbols the macro
    ///       defines: link it with \c -rdynamic, or instantiate with the EXPORT form of the
    ///       macro on Windows. Without that the plugin quietly grows a list of its own and the
    ///       host never sees the registration.
    /// \warning Registration is not synchronized. Static initialization is single threaded, but
    ///          loading a shared library from two threads at once is not, so serialize that
    ///          yourself.
    /// \warning The names are not copied. Register with a literal, or with something that
    ///          outlives the program.
    ///
    /// \sa DynamicRegistry, for the entries that are not known at link time
    template <class T, class Traits = static_registry_traits<T>>
    class StaticRegistry {
    public:
        using type = T;
        using traits_type = Traits;
        using result_type = typename traits_type::result_type;

        /// One registered implementation: what it is called, and how to make one.
        class Entry {
        public:
            Entry(std::string_view name, std::string_view desc, result_type (*ctor)())
                : _name(name), _desc(desc), _ctor(ctor) {
            }

            std::string_view name() const {
                return _name;
            }
            std::string_view desc() const {
                return _desc;
            }

            /// Makes one. A fresh object every call, so the registry holds descriptions rather
            /// than instances.
            result_type instantiate() const {
                return _ctor();
            }

        private:
            std::string_view _name;
            std::string_view _desc;
            result_type (*_ctor)();
        };

        class Iterator;

        /// A link in the list. Lives inside the Add object that registered it, so the registry
        /// itself never allocates.
        class Node {
        public:
            explicit Node(const Entry &entry) : _entry(entry) {
            }

        private:
            friend class StaticRegistry;
            friend class Iterator;

            Node *_next = nullptr;
            const Entry &_entry;
        };

        class Iterator {
        public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Entry;
            using difference_type = std::ptrdiff_t;
            using pointer = const Entry *;
            using reference = const Entry &;

            explicit Iterator(const Node *node = nullptr) : _node(node) {
            }

            reference operator*() const {
                return _node->_entry;
            }
            pointer operator->() const {
                return &_node->_entry;
            }

            Iterator &operator++() {
                _node = _node->_next;
                return *this;
            }
            Iterator operator++(int) {
                Iterator r = *this;
                ++*this;
                return r;
            }

            bool operator==(const Iterator &RHS) const {
                return _node == RHS._node;
            }
            bool operator!=(const Iterator &RHS) const {
                return _node != RHS._node;
            }

        private:
            const Node *_node;
        };

        /// What entries() hands back, so the registry reads in a range-for.
        class Range {
        public:
            Iterator begin() const {
                return Iterator(_head);
            }
            Iterator end() const {
                return Iterator(nullptr);
            }
        };

        static Iterator begin() {
            return Iterator(_head);
        }
        static Iterator end() {
            return Iterator(nullptr);
        }
        static Range entries() {
            return Range();
        }

        /// Appends \a node, keeping registration order. Called by Add, and by a plugin against
        /// the host's copy of this symbol.
        static void add_node(Node *node) {
            if (_tail) {
                _tail->_next = node;
            } else {
                _head = node;
            }
            _tail = node;
        }

        /// Registers \a V, default constructed, for as long as this object lives. For a static
        /// object that is the whole program.
        ///
        /// \code
        ///   static CodecRegistry::Add<FlacCodec> x("flac", "Free Lossless Audio Codec");
        /// \endcode
        ///
        /// \sa AddFactory, for an implementation that is not default constructible
        template <class V>
        class Add {
        public:
            Add(std::string_view name, std::string_view desc)
                : _entry(name, desc, &construct), _node(_entry) {
                add_node(&_node);
            }

        private:
            static result_type construct() {
                return traits_type::template construct<V>();
            }

            Entry _entry;
            Node _node;

            STDC_DISABLE_COPY_MOVE(Add)
        };

        /// Registers something built the way \a factory says, for the implementations that take
        /// constructor arguments, come from a factory function, or are not one type at all.
        ///
        /// A lambda that captures nothing converts to the function pointer this takes, so the
        /// arguments are baked in at the call site with no allocation and nothing to spell out:
        ///
        /// \code
        ///   static CodecRegistry::AddFactory x(
        ///       "mp3", "MPEG Layer III",
        ///       []() -> std::unique_ptr<Codec> { return std::make_unique<Mp3Codec>(44100, 2); });
        /// \endcode
        ///
        /// \note The lambda has to name the base in its return type, \c std::unique_ptr<Codec>
        ///       above. One returning \c std::unique_ptr<Mp3Codec> is a different function type
        ///       and will not convert, even though the pointers themselves would.
        /// \note Only values known where the lambda is written can go in it. Capturing something
        ///       decided at run time makes a closure, which is no longer a function pointer.
        ///       DynamicRegistry holds a \c std::function and takes those.
        class AddFactory {
        public:
            AddFactory(std::string_view name, std::string_view desc, result_type (*factory)())
                : _entry(name, desc, factory), _node(_entry) {
                add_node(&_node);
            }

        private:
            Entry _entry;
            Node _node;

            STDC_DISABLE_COPY_MOVE(AddFactory)
        };

        StaticRegistry() = delete;

    private:
        // Declared here and defined by STDC_INSTANTIATE_STATIC_REGISTRY, so one module
        // owns the list and a plugin links against that one rather than growing a list of its
        // own. Both are null-initialized, which is a constant initializer, so they are already
        // in place when the static constructors that do the registering run.
        //
        // Two declarations rather than one: MSVC rejects several static members of a dllexport
        // class in a single declaration with C2487.
        static Node *_head;
        static Node *_tail;
    };

    /// @}
}

/// Defines the storage for a \c StaticRegistry over \a TYPE, decorated with \a EXPORT.
///
/// Put this in exactly one translation unit of the module that owns the registry, which in a
/// plugin arrangement is the host. \a EXPORT is what makes the list reachable from a plugin:
/// \c __declspec(dllexport) on Windows, or nothing on the platforms where \c -rdynamic on the
/// host is what does it.
/// Both of these have to sit where they can name stdc, which is the global scope or inside stdc
/// itself. Naming it rather than reopening it is what makes writing them anywhere else say so:
/// the compiler answers "in namespace X, which does not enclose namespace stdc" instead of
/// failing somewhere inside this header.
#define STDC_INSTANTIATE_STATIC_REGISTRY_EXPORT(TYPE, EXPORT)                                      \
    template <>                                                                                    \
    typename ::stdc::StaticRegistry<TYPE>::Node * ::stdc::StaticRegistry<TYPE>::_head = nullptr;   \
    template <>                                                                                    \
    typename ::stdc::StaticRegistry<TYPE>::Node * ::stdc::StaticRegistry<TYPE>::_tail = nullptr;   \
    template class EXPORT ::stdc::StaticRegistry<TYPE>;

/// Defines the storage for a \c StaticRegistry over \a TYPE, undecorated.
///
/// Enough for a registry that lives in one module. A host that means to accept registrations
/// from a plugin wants the EXPORT form above instead.
#define STDC_INSTANTIATE_STATIC_REGISTRY(TYPE) STDC_INSTANTIATE_STATIC_REGISTRY_EXPORT(TYPE, )

#endif // STDCORELIB_STATICREGISTRY_H
