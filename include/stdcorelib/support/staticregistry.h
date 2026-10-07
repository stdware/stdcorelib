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
    /// The default returns an owning pointer, so that derived implementations retain their
    /// dynamic type. A specialization is required if \a T is a small descriptor that is returned
    /// by value.
    template <class T>
    struct static_registry_traits {
        using result_type = std::unique_ptr<T>;

        template <class V>
        static result_type construct() {
            return std::make_unique<V>();
        }
    };

    /// A registry that is filled before \c main, so that an implementation is available as soon
    /// as it is linked.
    ///
    /// Every registration object inserts itself into a list and removes itself when it is
    /// destroyed. No initialization function is called, and no list of objects to initialize
    /// exists. The registry therefore works across a shared library boundary: the static
    /// object of a plugin joins the list of the host when the plugin is loaded and leaves it
    /// when the plugin is unloaded.
    ///
    /// \code
    ///   // in the host
    ///   class Codec { public: virtual ~Codec() = default; };
    ///   using CodecRegistry = stdc::StaticRegistry<Codec>;
    ///   STDC_STATIC_REGISTRY(Codec)   // once, in one .cpp
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
    /// A linked list is the only structure that can be built this way. A map or a vector has its
    /// own constructor, and accessing it from a static constructor races with its initialization.
    /// The storage accessor initializes its two pointers before returning them to a
    /// registration.
    ///
    /// \note For a plugin to reach the list of the host, the registry is declared with
    ///       \c STDC_DECLARE_EXPORTED_STATIC_REGISTRY and defined with \c STDC_STATIC_REGISTRY.
    ///       An executable host must also export its symbols, for example by linking with
    ///       \c -rdynamic on ELF platforms.
    /// \note A registration is removed when the object that holds it is destroyed. For a plugin,
    ///       this happens only if the loader actually unloads the plugin. A platform loader may
    ///       retain a module even after it reports a successful close.
    /// \warning Neither insertion into the list nor removal from it is synchronized, and a
    ///          removal races with any iteration. Loading and unloading a shared library is not
    ///          necessarily single-threaded. The program must therefore serialize these
    ///          operations.
    /// \warning Removal from the list does not make a copy of an entry safe to keep. Its name and
    ///          constructor are located in the module that registered it, as is every object that
    ///          \c instantiate() created. All of them must be destroyed before the module is
    ///          unloaded.
    /// \warning The names are not copied. A name must be a string literal or another string
    ///          that outlives the program.
    ///
    /// \sa DynamicRegistry
    template <class T, class Traits = static_registry_traits<T>>
    class StaticRegistry {
    public:
        using type = T;
        using traits_type = Traits;
        using result_type = typename traits_type::result_type;

        /// One registered implementation, consisting of its name and its constructor.
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

            /// Creates an instance. Every call creates a new object, because the registry holds
            /// descriptions rather than instances.
            result_type instantiate() const {
                return _ctor();
            }

        private:
            std::string_view _name;
            std::string_view _desc;
            result_type (*_ctor)();
        };

        class Iterator;

        /// A node of the list. The node is stored inside the Add object that registered it, so
        /// that the registry itself never allocates memory.
        ///
        /// The list is doubly linked, so that a registration can remove itself in constant time
        /// without searching the list for its predecessor.
        class Node {
        public:
            explicit Node(const Entry &entry) : _entry(entry) {
            }

        private:
            friend class StaticRegistry;
            friend class Iterator;

            Node *_next = nullptr;
            Node *_prev = nullptr;
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

        /// The return type of entries(), which allows the registry to be used in a range-based
        /// for loop.
        class Range {
        public:
            Iterator begin() const {
                return StaticRegistry::begin();
            }
            Iterator end() const {
                return Iterator(nullptr);
            }
        };

        static Iterator begin() {
            return Iterator(storage().head);
        }
        static Iterator end() {
            return Iterator(nullptr);
        }
        static Range entries() {
            return Range();
        }

        /// Appends \a node and preserves the registration order. Add calls this function, and a
        /// plugin calls the copy of this symbol in the host.
        static void add_node(Node *node) {
            auto &data = storage();
            node->_prev = data.tail;
            if (data.tail) {
                data.tail->_next = node;
            } else {
                data.head = node;
            }
            data.tail = node;
        }

        /// Removes \a node. A registration calls this function when it is destroyed.
        static void remove_node(Node *node) {
            auto &data = storage();
            if (node->_prev) {
                node->_prev->_next = node->_next;
            } else {
                data.head = node->_next;
            }
            if (node->_next) {
                node->_next->_prev = node->_prev;
            } else {
                data.tail = node->_prev;
            }
            node->_next = nullptr;
            node->_prev = nullptr;
        }

        /// Registers \a V, constructed by its default constructor, for the lifetime of this
        /// object. For a static object, the lifetime is the whole program.
        ///
        /// \code
        ///   static CodecRegistry::Add<FlacCodec> x("flac", "Free Lossless Audio Codec");
        /// \endcode
        ///
        /// AddFactory registers an implementation that is not default-constructible.
        ///
        /// \sa AddFactory
        template <class V>
        class Add {
        public:
            Add(std::string_view name, std::string_view desc)
                : _entry(name, desc, &construct), _node(_entry) {
                add_node(&_node);
            }

            ~Add() {
                remove_node(&_node);
            }

        private:
            static result_type construct() {
                return traits_type::template construct<V>();
            }

            Entry _entry;
            Node _node;

            STDC_DISABLE_COPY_MOVE(Add)
        };

        /// Registers an implementation that \a factory constructs. This form is intended for
        /// implementations that require constructor arguments, that a factory function creates,
        /// or that are not a single type.
        ///
        /// A lambda without captures converts to the function pointer that the constructor
        /// accepts. The arguments are therefore fixed at the call site without allocation and
        /// without a separate type:
        ///
        /// \code
        ///   static CodecRegistry::AddFactory x(
        ///       "mp3", "MPEG Layer III",
        ///       []() -> std::unique_ptr<Codec> { return std::make_unique<Mp3Codec>(44100, 2); });
        /// \endcode
        ///
        /// \note The lambda must name the base class in its return type, as
        ///       \c std::unique_ptr<Codec> above. A lambda that returns
        ///       \c std::unique_ptr<Mp3Codec> has a different function type and does not convert,
        ///       although the pointers themselves convert.
        /// \note The lambda can contain only values that are known where it is written. A capture
        ///       of a value determined at run time makes a closure, which does not convert to a
        ///       function pointer. DynamicRegistry stores a \c std::function and accepts closures.
        class AddFactory {
        public:
            AddFactory(std::string_view name, std::string_view desc, result_type (*factory)())
                : _entry(name, desc, factory), _node(_entry) {
                add_node(&_node);
            }

            ~AddFactory() {
                remove_node(&_node);
            }

        private:
            Entry _entry;
            Node _node;

            STDC_DISABLE_COPY_MOVE(AddFactory)
        };

        StaticRegistry() = delete;

    private:
        struct Storage {
            Node *head = nullptr;
            Node *tail = nullptr;
        };

        static Storage &storage();
    };

    /// @}
}

/// Declares the exported storage for a \c StaticRegistry over \a TYPE.
///
/// The declaration belongs in a public header, after any specialization of
/// \c static_registry_traits for \a TYPE. \c STDC_STATIC_REGISTRY belongs in one translation unit
/// of the module that owns the registry. \a EXPORT must select export while that module is built
/// and import while it is used.
#define STDC_DECLARE_EXPORTED_STATIC_REGISTRY(TYPE, EXPORT)                                        \
    template <>                                                                                    \
    EXPORT                                                                                         \
        typename ::stdc::StaticRegistry<TYPE>::Storage & ::stdc::StaticRegistry<TYPE>::storage();

/// Declares the storage for a \c StaticRegistry over \a TYPE without an export decoration.
#define STDC_DECLARE_STATIC_REGISTRY(TYPE) STDC_DECLARE_EXPORTED_STATIC_REGISTRY(TYPE, )

/// Defines the storage for a \c StaticRegistry over \a TYPE.
///
/// The definition belongs in exactly one translation unit of the module that owns the registry.
/// If registrations can come from another module, the corresponding declaration belongs in a
/// public header. The macro must be used at global scope.
#define STDC_STATIC_REGISTRY(TYPE)                                                                 \
    template <>                                                                                    \
    typename ::stdc::StaticRegistry<TYPE>::Storage & ::stdc::StaticRegistry<TYPE>::storage() {     \
        static Storage storage;                                                                    \
        return storage;                                                                            \
    }

#endif // STDCORELIB_STATICREGISTRY_H
