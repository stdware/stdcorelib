// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_DYNAMICREGISTRY_H
#define STDCORELIB_DYNAMICREGISTRY_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/vlarray.h>
#include <stdcorelib/scope_guard.h>

namespace stdc {

    /// \addtogroup types
    /// @{

    namespace detail {

        template <class Map, class = void>
        struct has_append : std::false_type {};

        template <class Map>
        struct has_append<Map, std::void_t<decltype(std::declval<Map &>().append(
                                   std::declval<const typename Map::key_type &>(),
                                   std::declval<typename Map::mapped_type>()))>> : std::true_type {
        };

    }

    /// Controls what a \c DynamicRegistry factory returns and how an absent result is represented.
    ///
    /// The default returns an owning pointer. A specialization is required if \a T is returned by
    /// value or uses another ownership type. \c construct() is required only by
    /// DynamicRegistry::Add.
    template <class T>
    struct dynamic_registry_traits {
        using result_type = std::unique_ptr<T>;

        static result_type empty() {
            return nullptr;
        }

        template <class V>
        static result_type construct() {
            return std::make_unique<V>();
        }
    };

    /// A registry that is filled at run time, searched by name, and observable through listeners.
    ///
    /// The class complements StaticRegistry for entries that a program discovers only at run
    /// time, such as entries requested by a configuration file, found in a plugin directory, or
    /// defined by a script. An entry is registered by a call or by an object that the program
    /// creates, rather than by a static object, so that the program determines the order.
    ///
    /// The owner of the registry, such as an application or a host of plugins, creates the
    /// registry and passes it to the code that registers entries. No registry exists per process,
    /// so that a test creates a registry of its own with a known initial state.
    ///
    /// \code
    ///   stdc::DynamicRegistry<Codec> codecs;
    ///   codecs.add("flac", "Free Lossless Audio Codec",
    ///              [] { return std::unique_ptr<Codec>(new FlacCodec()); });
    ///
    ///   if (auto entry = codecs.find("flac")) {
    ///       auto codec = entry->instantiate();
    ///   }
    /// \endcode
    ///
    /// \a Map determines the order of entries(). With \c std::map, the default, the entries are
    /// sorted by name. With stdc::linked_map, the entries are in the order of registration.
    /// \a Map is instantiated as <tt>Map<std::string, EntryPointer></tt>.
    ///
    /// \note The class is thread-safe. add(), remove() and the lookups may be called from any
    ///       thread. Entries are returned as shared_ptr, so that an entry remains valid for the
    ///       caller even if another thread removes it meanwhile.
    /// \warning remove_listener() must not be called from a listener callback.
    ///
    /// \sa StaticRegistry, AddFactory, Add
    template <class T, class Traits = dynamic_registry_traits<T>,
              template <class...> class Map = std::map>
    class DynamicRegistry {
    public:
        using type = T;
        using traits_type = Traits;
        using result_type = typename traits_type::result_type;

        /// The function that creates an instance. The type is a \c std::function rather than a
        /// function pointer, because an entry discovered at run time usually carries data, such
        /// as the library from which it was loaded.
        using Factory = std::function<result_type()>;

        /// One registered implementation. Unlike an entry of StaticRegistry, it owns its name,
        /// because no string literal exists to refer to.
        class Entry {
        public:
            Entry(std::string name, std::string desc, Factory factory)
                : _name(std::move(name)), _desc(std::move(desc)), _factory(std::move(factory)) {
            }

            const std::string &name() const {
                return _name;
            }
            const std::string &desc() const {
                return _desc;
            }

            /// Creates an instance, or returns the empty result of the traits if no factory
            /// exists.
            result_type instantiate() const {
                return _factory ? _factory() : traits_type::empty();
            }

        private:
            std::string _name;
            std::string _desc;
            Factory _factory;
        };

        using EntryPointer = std::shared_ptr<const Entry>;

        /// The interface notified when entries are added or removed. add_listener() installs a
        /// listener.
        class Listener {
        public:
            virtual ~Listener() = default;

            virtual void entry_added(const EntryPointer &entry) {
                (void) entry;
            }
            virtual void entry_removed(const EntryPointer &entry) {
                (void) entry;
            }
        };

        /// Registers an entry that \a factory creates, for the lifetime of this object. Unlike
        /// StaticRegistry::AddFactory, the factory may be a closure.
        ///
        /// The destructor removes the entry. A plugin holds these objects and destroys them
        /// before it is unloaded, because the factory of an entry is code of the plugin. Assigning
        /// an empty object removes the entry earlier.
        ///
        /// The object is safe if the registry is destroyed first: the destructor then removes
        /// nothing. The object and the registry must not be destroyed concurrently on two
        /// threads.
        ///
        /// \code
        ///   // a member of the plugin, assigned when the plugin is loaded
        ///   mp3 = stdc::DynamicRegistry<Codec>::AddFactory(
        ///       codecs, "mp3", "MPEG Layer III",
        ///       [rate] { return std::unique_ptr<Codec>(new Mp3Codec(rate)); });
        ///
        ///   // before the plugin is unloaded
        ///   mp3 = {};
        /// \endcode
        ///
        /// \sa Add
        class AddFactory {
        public:
            /// Constructs an object that registers nothing.
            AddFactory() = default;

            /// Registers \a name in \a registry, as add() does. If the name is already
            /// registered, the object registers nothing, and entry() returns null.
            AddFactory(DynamicRegistry &registry, std::string name, std::string desc,
                       Factory factory)
                : _registry(registry._self),
                  _entry(registry.insert(std::move(name), std::move(desc), std::move(factory))) {
            }

            ~AddFactory() {
                reset();
            }

            AddFactory(AddFactory &&RHS) noexcept
                : _registry(std::move(RHS._registry)), _entry(std::move(RHS._entry)) {
            }

            AddFactory &operator=(AddFactory &&RHS) noexcept {
                if (this != &RHS) {
                    reset();
                    _registry = std::move(RHS._registry);
                    _entry = std::move(RHS._entry);
                }
                return *this;
            }

            /// Returns the registered entry, or null if the object registers nothing.
            const EntryPointer &entry() const {
                return _entry;
            }

        private:
            void reset() {
                if (const auto registry = _registry.lock(); registry && _entry) {
                    (*registry)->erase(_entry->name(), _entry.get());
                }
                _registry.reset();
                _entry.reset();
            }

            std::weak_ptr<DynamicRegistry *> _registry;
            EntryPointer _entry;

            STDC_DISABLE_COPY(AddFactory)
        };

        /// Registers \a V, constructed by \c construct<V>() of the traits, for the lifetime of
        /// this object, as StaticRegistry::Add does. The object behaves as AddFactory otherwise.
        ///
        /// \code
        ///   flac = stdc::DynamicRegistry<Codec>::Add<FlacCodec>(codecs, "flac",
        ///                                                       "Free Lossless Audio Codec");
        /// \endcode
        template <class V>
        class Add : public AddFactory {
        public:
            /// Constructs an object that registers nothing.
            Add() = default;

            /// Registers \a name in \a registry. If the name is already registered, the object
            /// registers nothing, and entry() returns null.
            Add(DynamicRegistry &registry, std::string name, std::string desc)
                : AddFactory(registry, std::move(name), std::move(desc), &construct) {
            }

        private:
            static result_type construct() {
                return traits_type::template construct<V>();
            }
        };

        DynamicRegistry() = default;

        /// Destroys the registry. An object of AddFactory or Add that still refers to the
        /// registry then removes nothing.
        ~DynamicRegistry() = default;

        /// Registers \a name.
        ///
        /// \retval false the name is already registered, and the registry is unchanged
        bool add(std::string name, std::string desc, Factory factory) {
            return insert(std::move(name), std::move(desc), std::move(factory)) != nullptr;
        }

        /// Removes \a name.
        ///
        /// \retval false no entry with this name exists
        /// \note An instance already created by instantiate() is not affected. The removal only
        ///       prevents the creation of new instances.
        ///
        /// \warning <b>A plugin must remove its entries before it is unloaded.</b> The registered
        ///          factory is code inside the plugin, and the registry can outlive the plugin. A
        ///          remaining entry therefore calls into memory that is no longer mapped. An
        ///          AddFactory object removes its entry on destruction.
        bool remove(std::string_view name) {
            return erase(name, nullptr);
        }

        /// Returns the entry registered under \a name, or null.
        EntryPointer find(std::string_view name) const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            auto it = _entries.find(std::string(name));
            return it == _entries.end() ? EntryPointer() : it->second;
        }

        /// Creates an instance directly, or returns the empty result of the traits if \a name is
        /// not registered.
        result_type instantiate(std::string_view name) const {
            auto entry = find(name);
            return entry ? entry->instantiate() : traits_type::empty();
        }

        /// Returns every entry, in the order that \a Map determines. The result is a snapshot,
        /// so that iterating it is safe while another thread registers entries.
        std::vector<EntryPointer> entries() const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            std::vector<EntryPointer> result;
            result.reserve(_entries.size());
            for (const auto &pair : _entries) {
                result.push_back(pair.second);
            }
            return result;
        }

        size_t size() const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            return _entries.size();
        }

        /// Removes every entry without notifying the listeners. The function is intended mainly
        /// for a test that requires a known initial state.
        void clear() {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            _entries.clear();
        }

        /// \name Listeners
        /// @{

        /// Adds \a listener, which the registry does not own. The listener must outlive the
        /// registry or be removed first. Adding the same listener twice has no effect.
        void add_listener(Listener *listener) {
            if (!listener) {
                return;
            }
            std::unique_lock<std::shared_mutex> lock(_mutex);
            if (std::find(_listeners.begin(), _listeners.end(), listener) == _listeners.end()) {
                _listeners.push_back(listener);
            }
        }

        void remove_listener(Listener *listener) {
            std::unique_lock<std::shared_mutex> lock(_mutex);
            _listeners.erase(std::remove(_listeners.begin(), _listeners.end(), listener),
                             _listeners.end());
            lock.unlock();
            std::unique_lock<std::mutex> notificationLock(_notificationMutex);
            _notificationsFinished.wait(notificationLock,
                                        [this] { return _activeNotifications == 0; });
        }

        /// @}

    private:
        // Few programs install listeners, and a program that installs listeners usually installs
        // one or two. Four pointers are stored inline. They hold the snapshot that insert() and
        // erase() take in order to call the listeners outside the lock, so that the common case
        // allocates no memory.
        static constexpr size_t PreallocatedListeners = 4;
        using ListenerList = vlarray<Listener *, PreallocatedListeners>;
        using EntryMap = Map<std::string, EntryPointer>;

        // Adds an entry and notifies the listeners. Returns the entry, or null if the name is
        // already registered.
        EntryPointer insert(std::string name, std::string desc, Factory factory) {
            auto entry =
                std::make_shared<const Entry>(std::move(name), std::move(desc), std::move(factory));
            ListenerList listeners;
            {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                if (_entries.find(entry->name()) != _entries.end()) {
                    return nullptr;
                }
                if constexpr (detail::has_append<EntryMap>::value) {
                    _entries.append(entry->name(), entry);
                } else {
                    _entries.emplace(entry->name(), entry);
                }
                listeners = _listeners;
                if (!listeners.empty()) {
                    ++_activeNotifications;
                }
            }
            notify(listeners, entry, &Listener::entry_added);
            return entry;
        }

        // Removes the entry of name and notifies the listeners. If expected is not null, the
        // entry is removed only if it is expected. An AddFactory object passes its own entry, so
        // that the removal does not affect an entry that another caller registered under the same
        // name after a remove().
        bool erase(std::string_view name, const Entry *expected) {
            EntryPointer entry;
            ListenerList listeners;
            {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                auto it = _entries.find(std::string(name));
                if (it == _entries.end() || (expected && it->second.get() != expected)) {
                    return false;
                }
                entry = it->second;
                _entries.erase(it);
                listeners = _listeners;
                if (!listeners.empty()) {
                    ++_activeNotifications;
                }
            }
            notify(listeners, entry, &Listener::entry_removed);
            return true;
        }

        void notify(const ListenerList &listeners, const EntryPointer &entry,
                    void (Listener::*callback)(const EntryPointer &)) {
            if (listeners.empty()) {
                return;
            }
            auto finished = make_scope_guard([this] { notificationFinished(); });
            for (Listener *listener : listeners) {
                (listener->*callback)(entry);
            }
        }

        // Called by the guard of a batch of callbacks after the batch ends.
        //
        // The count is decremented while _notificationMutex is held, not only atomically, which
        // is the reason for this function. remove_listener() waits for the count through a
        // condition variable, and the predicate of a condition variable must be changed while the
        // mutex of the waiter is held. A decrement outside the mutex can occur between the moment
        // at which the waiter reads the count and the moment at which it blocks. notify_all()
        // then wakes no thread, and no further notification follows. std::atomic does not prevent
        // this, because it makes the write atomic, not the interval between reading and blocking.
        void notificationFinished() {
            {
                std::lock_guard<std::mutex> lock(_notificationMutex);
                --_activeNotifications;
            }
            _notificationsFinished.notify_all();
        }

        mutable std::shared_mutex _mutex;
        EntryMap _entries;
        ListenerList _listeners;
        std::atomic<size_t> _activeNotifications{0};
        std::mutex _notificationMutex;
        std::condition_variable _notificationsFinished;

        // The AddFactory objects refer to the registry through this pointer. It expires with the
        // registry, so that an AddFactory object destroyed later removes nothing.
        std::shared_ptr<DynamicRegistry *> _self = std::make_shared<DynamicRegistry *>(this);

        STDC_DISABLE_COPY_MOVE(DynamicRegistry)
    };

    /// @}

}

#endif // STDCORELIB_DYNAMICREGISTRY_H
