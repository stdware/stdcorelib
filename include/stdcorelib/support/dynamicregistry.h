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
#include <vector>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/type_id.h>
#include <stdcorelib/adt/vlarray.h>
#include <stdcorelib/scope_guard.h>

namespace stdc {

    /// \addtogroup types
    /// @{

    namespace detail {

        // Returns the single object that this process keeps under name. create constructs the
        // object on the first request. The source file explains why this is a function rather
        // than a static variable.
        STDC_EXPORT void *shared_instance(std::string_view name, void *(*create)());

    }

    /// Controls what a \c DynamicRegistry factory returns and how an absent result is represented.
    ///
    /// The default returns an owning pointer. A specialization is required if \a T is returned by
    /// value or uses another ownership type.
    template <class T>
    struct dynamic_registry_traits {
        using result_type = std::unique_ptr<T>;

        static result_type empty() {
            return nullptr;
        }
    };

    /// A registry that is filled at run time, searched by name, and observable through listeners.
    ///
    /// The class complements StaticRegistry for entries that a program discovers only at run
    /// time, such as entries requested by a configuration file, found in a plugin directory, or
    /// defined by a script. Registration is a function call rather than a static object, so that
    /// the program determines the order.
    ///
    /// \code
    ///   auto &reg = stdc::DynamicRegistry<Codec>::instance();
    ///   reg.add("flac", "Free Lossless Audio Codec",
    ///           [] { return std::unique_ptr<Codec>(new FlacCodec()); });
    ///
    ///   if (auto entry = reg.find("flac")) {
    ///       auto codec = entry->instantiate();
    ///   }
    /// \endcode
    ///
    /// The registry is constructed on first use rather than during static initialization.
    /// Therefore, nothing depends on the initialization order of the translation units, and one
    /// registry exists per process rather than per module.
    ///
    /// \note The class is thread-safe. add(), remove() and the lookups may be called from any
    ///       thread. Entries are returned as shared_ptr, so that an entry remains valid for the
    ///       caller even if another thread removes it meanwhile.
    /// \warning remove_listener() must not be called from a listener callback.
    ///
    /// \sa StaticRegistry, instance()
    template <class T, class Traits = dynamic_registry_traits<T>>
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

        /// Returns the registry for \a T, which is constructed on first use.
        ///
        /// One registry exists per process, not per module. The static variable here caches a
        /// pointer, and the table inside stdcorelib owns the object. A plugin therefore registers
        /// in the registry that the host searches.
        ///
        /// \note This holds within the reach of one copy of stdcorelib. If two modules each link
        ///       stdcorelib statically, each module has its own table and therefore its own
        ///       registry.
        /// \note The registry is never destroyed. A plugin can therefore use it without becoming
        ///       responsible for its destruction. The obligations of a plugin are described at
        ///       remove().
        /// \sa remove()
        static DynamicRegistry &instance() {
            static auto *self = static_cast<DynamicRegistry *>(
                detail::shared_instance(detail::type_name<DynamicRegistry>(),
                                        [] { return static_cast<void *>(new DynamicRegistry()); }));
            return *self;
        }

        /// Registers \a name.
        ///
        /// \retval false the name is already registered, and the registry is unchanged
        bool add(std::string name, std::string desc, Factory factory) {
            auto entry =
                std::make_shared<const Entry>(std::move(name), std::move(desc), std::move(factory));
            ListenerList listeners;
            {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                if (_entries.find(entry->name()) != _entries.end()) {
                    return false;
                }
                _entries.emplace(entry->name(), entry);
                listeners = _listeners;
                if (!listeners.empty()) {
                    ++_activeNotifications;
                }
            }
            auto finished = make_scope_guard([this, notify = !listeners.empty()] {
                if (notify) {
                    notificationFinished();
                }
            });
            for (Listener *listener : listeners) {
                listener->entry_added(entry);
            }
            return true;
        }

        /// Removes \a name.
        ///
        /// \retval false no entry with this name exists
        /// \note An instance already created by instantiate() is not affected. The removal only
        ///       prevents the creation of new instances.
        ///
        /// \warning <b>A plugin must remove its entries before it is unloaded.</b> The registered
        ///          factory is code inside the plugin, and the registry outlives the plugin. A
        ///          remaining entry therefore calls into memory that is no longer mapped. Nothing
        ///          detects this condition, because the entry is indistinguishable from other
        ///          entries until it is instantiated.
        ///
        /// \code
        ///   // in the plugin, before it is unloaded
        ///   DynamicRegistry<Codec>::instance().remove("flac");
        /// \endcode
        ///
        /// A plugin does not release the registry itself. The registry is shared with the host
        /// and with every other plugin and is never destroyed. The entries are therefore the only
        /// part of the registry that a plugin owns.
        bool remove(std::string_view name) {
            EntryPointer entry;
            ListenerList listeners;
            {
                std::unique_lock<std::shared_mutex> lock(_mutex);
                auto it = _entries.find(name);
                if (it == _entries.end()) {
                    return false;
                }
                entry = it->second;
                _entries.erase(it);
                listeners = _listeners;
                if (!listeners.empty()) {
                    ++_activeNotifications;
                }
            }
            auto finished = make_scope_guard([this, notify = !listeners.empty()] {
                if (notify) {
                    notificationFinished();
                }
            });
            for (Listener *listener : listeners) {
                listener->entry_removed(entry);
            }
            return true;
        }

        /// Returns the entry registered under \a name, or null.
        EntryPointer find(std::string_view name) const {
            std::shared_lock<std::shared_mutex> lock(_mutex);
            auto it = _entries.find(name);
            return it == _entries.end() ? EntryPointer() : it->second;
        }

        /// Creates an instance directly, or returns the empty result of the traits if \a name is
        /// not registered.
        result_type instantiate(std::string_view name) const {
            auto entry = find(name);
            return entry ? entry->instantiate() : traits_type::empty();
        }

        /// Returns every entry, sorted by name. The result is a snapshot, so that iterating it is
        /// safe while another thread registers entries.
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

        /// Removes every entry. The function is intended mainly for a test that requires a known
        /// initial state.
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
        DynamicRegistry() = default;

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

        // Few programs install listeners, and a program that installs listeners usually installs
        // one or two. Four pointers are stored inline. They hold the snapshot that add() and
        // remove() take in order to call the listeners outside the lock, so that the common case
        // allocates no memory.
        static constexpr size_t PreallocatedListeners = 4;
        using ListenerList = vlarray<Listener *, PreallocatedListeners>;

        // std::less<> allows a lookup with a string_view without constructing a string.
        std::map<std::string, EntryPointer, std::less<>> _entries;
        ListenerList _listeners;
        std::atomic<size_t> _activeNotifications{0};
        std::mutex _notificationMutex;
        std::condition_variable _notificationsFinished;

        STDC_DISABLE_COPY_MOVE(DynamicRegistry)
    };

    /// @}
}

#endif // STDCORELIB_DYNAMICREGISTRY_H
