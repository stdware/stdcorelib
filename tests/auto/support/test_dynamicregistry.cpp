// SPDX-License-Identifier: MIT

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <stdcorelib/adt/linked_map.h>
#include <stdcorelib/support/dynamicregistry.h>
#include <stdcorelib/support/sharedlibrary.h>

#include "../plugins/test_dynamicregistry_plugin.h"

#include <boost/test/unit_test.hpp>

using namespace stdc;

namespace {

    struct Widget {
        virtual ~Widget() = default;
        int tag = 0;
    };

    struct Button : Widget {
        Button() {
            tag = 5;
        }
    };

    struct Descriptor {
        int tag;
    };

}

namespace stdc {

    template <>
    struct dynamic_registry_traits<Descriptor> {
        using result_type = Descriptor;

        static result_type empty() {
            return {-1};
        }
    };

}

namespace {

    using WidgetRegistry = DynamicRegistry<Widget>;

    WidgetRegistry::Factory factoryOf(int tag) {
        return [tag] {
            auto w = std::unique_ptr<Widget>(new Widget());
            w->tag = tag;
            return w;
        };
    }

    template <class Registry>
    std::vector<std::string> namesOf(const Registry &registry) {
        std::vector<std::string> names;
        for (const auto &entry : registry.entries()) {
            names.push_back(entry->name());
        }
        return names;
    }

    class CountingListener : public WidgetRegistry::Listener {
    public:
        void entry_added(const WidgetRegistry::EntryPointer &entry) override {
            added.push_back(entry->name());
        }
        void entry_removed(const WidgetRegistry::EntryPointer &entry) override {
            removed.push_back(entry->name());
        }
        std::vector<std::string> added, removed;
    };

    // A listener that overrides neither callback. The inherited callbacks do nothing, and the
    // registry calls them nevertheless.
    class SilentListener : public WidgetRegistry::Listener {};

}

BOOST_AUTO_TEST_SUITE(test_dynamicregistry)

BOOST_AUTO_TEST_CASE(test_add_find_remove) {
    WidgetRegistry reg;
    BOOST_CHECK(reg.add("alpha", "alpha widget", factoryOf(1)));
    BOOST_CHECK_EQUAL(reg.size(), 1u);

    // A name is registered only once.
    BOOST_CHECK(!reg.add("alpha", "again", factoryOf(2)));
    BOOST_CHECK_EQUAL(reg.size(), 1u);

    auto entry = reg.find("alpha");
    BOOST_REQUIRE(entry);
    BOOST_CHECK_EQUAL(entry->name(), "alpha");
    BOOST_CHECK_EQUAL(entry->desc(), "alpha widget");
    BOOST_CHECK_EQUAL(entry->instantiate()->tag, 1);

    auto direct = reg.instantiate("alpha");
    BOOST_REQUIRE(direct);
    BOOST_CHECK_EQUAL(direct->tag, 1);

    BOOST_CHECK(!reg.find("nothing"));
    BOOST_CHECK(!reg.instantiate("nothing"));

    BOOST_CHECK(reg.remove("alpha"));
    BOOST_CHECK(!reg.remove("alpha"));
    BOOST_CHECK(!reg.find("alpha"));

    // An entry that the caller holds remains valid after its removal.
    BOOST_CHECK_EQUAL(entry->instantiate()->tag, 1);
}

// Two registries of the same type are independent.
BOOST_AUTO_TEST_CASE(test_registries_are_independent) {
    WidgetRegistry first;
    WidgetRegistry second;
    BOOST_CHECK(first.add("alpha", {}, factoryOf(1)));
    BOOST_CHECK(second.add("alpha", {}, factoryOf(2)));
    BOOST_CHECK_EQUAL(first.instantiate("alpha")->tag, 1);
    BOOST_CHECK_EQUAL(second.instantiate("alpha")->tag, 2);
    BOOST_CHECK(first.remove("alpha"));
    BOOST_CHECK(second.find("alpha"));
}

BOOST_AUTO_TEST_CASE(test_value_result) {
    using DescriptorRegistry = DynamicRegistry<Descriptor>;
    DescriptorRegistry reg;

    static_assert(std::is_same_v<DescriptorRegistry::result_type, Descriptor>);
    BOOST_CHECK_EQUAL(reg.instantiate("missing").tag, -1);

    BOOST_REQUIRE(reg.add("answer", "", []() -> Descriptor { return {42}; }));
    BOOST_CHECK_EQUAL(reg.find("answer")->instantiate().tag, 42);
    BOOST_CHECK_EQUAL(reg.instantiate("answer").tag, 42);
}

// std::map sorts the entries by name, and stdc::linked_map keeps the order of registration.
BOOST_AUTO_TEST_CASE(test_the_map_determines_the_order) {
    WidgetRegistry sorted;
    DynamicRegistry<Widget, dynamic_registry_traits<Widget>, linked_map> ordered;
    for (const char *name : {"gamma", "alpha", "beta"}) {
        sorted.add(name, {}, factoryOf(0));
        ordered.add(name, {}, factoryOf(0));
    }
    BOOST_CHECK(namesOf(sorted) == (std::vector<std::string>{"alpha", "beta", "gamma"}));
    BOOST_CHECK(namesOf(ordered) == (std::vector<std::string>{"gamma", "alpha", "beta"}));

    // The other operations behave the same with either map.
    BOOST_CHECK(!ordered.add("alpha", {}, factoryOf(1)));
    BOOST_CHECK(ordered.remove("alpha"));
    BOOST_CHECK(namesOf(ordered) == (std::vector<std::string>{"gamma", "beta"}));
    BOOST_CHECK(ordered.add("alpha", {}, factoryOf(1)));
    BOOST_CHECK(namesOf(ordered) == (std::vector<std::string>{"gamma", "beta", "alpha"}));
    BOOST_CHECK_EQUAL(ordered.instantiate("alpha")->tag, 1);
}

BOOST_AUTO_TEST_CASE(test_listeners) {
    WidgetRegistry reg;
    CountingListener listener;
    reg.add_listener(&listener);
    reg.add_listener(&listener); // a second addition has no effect

    reg.add("one", {}, factoryOf(1));
    reg.remove("one");

    BOOST_REQUIRE_EQUAL(listener.added.size(), 1u);
    BOOST_CHECK_EQUAL(listener.added[0], "one");
    BOOST_REQUIRE_EQUAL(listener.removed.size(), 1u);
    BOOST_CHECK_EQUAL(listener.removed[0], "one");

    reg.remove_listener(&listener);
    reg.add("two", {}, factoryOf(2));
    BOOST_CHECK_EQUAL(listener.added.size(), 1u);
}

BOOST_AUTO_TEST_CASE(test_removing_listener_waits_for_callbacks) {
    WidgetRegistry reg;

    class BlockingListener : public WidgetRegistry::Listener {
    public:
        void entry_added(const WidgetRegistry::EntryPointer &) override {
            entered.set_value();
            release.get_future().wait();
        }

        std::promise<void> entered;
        std::promise<void> release;
    } listener;

    auto entered = listener.entered.get_future();
    reg.add_listener(&listener);
    std::thread notifier([&] { reg.add("blocked", {}, factoryOf(1)); });
    entered.wait();

    auto removed = std::async(std::launch::async, [&] { reg.remove_listener(&listener); });
    BOOST_CHECK(removed.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

    listener.release.set_value();
    notifier.join();
    removed.get();
}

// Both callbacks are virtual functions with a body rather than pure virtual functions, so that a
// listener may override neither of them.
BOOST_AUTO_TEST_CASE(test_a_listener_may_override_neither_half) {
    WidgetRegistry reg;
    SilentListener silent;
    reg.add_listener(&silent);

    reg.add("three", {}, factoryOf(3));
    BOOST_CHECK_EQUAL(reg.size(), 1u);
    BOOST_CHECK(reg.remove("three"));
    BOOST_CHECK_EQUAL(reg.size(), 0u);

    reg.remove_listener(&silent);
}

// Registrations from several threads at once result in every entry, without duplicates.
BOOST_AUTO_TEST_CASE(test_is_thread_safe) {
    WidgetRegistry reg;

    constexpr int Threads = 8;
    constexpr int PerThread = 50;

    std::vector<std::thread> workers;
    for (int t = 0; t < Threads; ++t) {
        workers.emplace_back([t, PerThread, &reg] {
            for (int i = 0; i < PerThread; ++i) {
                auto name = std::to_string(t) + "." + std::to_string(i);
                reg.add(name, {}, factoryOf(0));
                std::ignore = reg.find(name);
                std::ignore = reg.entries();
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }

    BOOST_CHECK_EQUAL(reg.size(), size_t(Threads * PerThread));
}

// An AddFactory object adds its entry and notifies the listeners, removes the entry on
// destruction, and registers nothing if the name is taken.
BOOST_AUTO_TEST_CASE(test_an_add_factory_object_removes_its_entry) {
    WidgetRegistry reg;
    CountingListener listener;
    reg.add_listener(&listener);
    {
        WidgetRegistry::AddFactory registration(reg, "alpha", "alpha widget", factoryOf(1));
        BOOST_REQUIRE(registration.entry());
        BOOST_CHECK(reg.find("alpha") == registration.entry());

        WidgetRegistry::AddFactory duplicate(reg, "alpha", {}, factoryOf(2));
        BOOST_CHECK(!duplicate.entry());
        BOOST_CHECK_EQUAL(reg.instantiate("alpha")->tag, 1);
    }
    BOOST_CHECK(!reg.find("alpha"));
    BOOST_CHECK(listener.added == (std::vector<std::string>{"alpha"}));
    BOOST_CHECK(listener.removed == (std::vector<std::string>{"alpha"}));
    reg.remove_listener(&listener);

    // A moved object removes the entry once, and assigning an empty object removes it at once.
    WidgetRegistry::AddFactory moved;
    BOOST_CHECK(!moved.entry());
    {
        WidgetRegistry::AddFactory registration(reg, "beta", {}, factoryOf(1));
        moved = std::move(registration);
    }
    BOOST_REQUIRE(moved.entry());
    BOOST_CHECK(reg.find("beta") == moved.entry());
    moved = {};
    BOOST_CHECK(!moved.entry());
    BOOST_CHECK(!reg.find("beta"));
}

// An Add object registers its type, constructed by the traits, and removes the entry on
// destruction.
BOOST_AUTO_TEST_CASE(test_an_add_object_registers_its_type) {
    WidgetRegistry reg;
    {
        WidgetRegistry::Add<Button> registration(reg, "button", "a button");
        BOOST_REQUIRE(registration.entry());
        BOOST_CHECK(reg.find("button") == registration.entry());
        BOOST_CHECK_EQUAL(reg.find("button")->desc(), "a button");
        const auto button = reg.instantiate("button");
        BOOST_REQUIRE(button);
        BOOST_CHECK_EQUAL(button->tag, 5);
        BOOST_CHECK(!(WidgetRegistry::Add<Button>(reg, "button", {}).entry()));

        WidgetRegistry::Add<Button> moved;
        moved = std::move(registration);
        BOOST_CHECK(reg.find("button") == moved.entry());
    }
    BOOST_CHECK(!reg.find("button"));
}

// The destruction of an AddFactory object whose name was removed and registered again leaves the
// new entry in place.
BOOST_AUTO_TEST_CASE(test_an_add_factory_object_spares_an_entry_registered_again) {
    WidgetRegistry reg;
    auto registration =
        std::make_unique<WidgetRegistry::AddFactory>(reg, "alpha", "", factoryOf(1));
    BOOST_CHECK(reg.remove("alpha"));
    BOOST_CHECK(reg.add("alpha", {}, factoryOf(2)));

    registration.reset();
    BOOST_REQUIRE(reg.find("alpha"));
    BOOST_CHECK_EQUAL(reg.instantiate("alpha")->tag, 2);
}

// An AddFactory object may outlive its registry, and its destruction then removes nothing.
BOOST_AUTO_TEST_CASE(test_an_add_factory_object_outlives_its_registry) {
    std::unique_ptr<WidgetRegistry::AddFactory> registration;
    {
        WidgetRegistry reg;
        registration = std::make_unique<WidgetRegistry::AddFactory>(reg, "alpha", "", factoryOf(1));
        BOOST_CHECK(reg.find("alpha") == registration->entry());
    }
    registration.reset();
}

#ifdef TEST_DYNAMICREGISTRY_PLUGIN_PATH

// A plugin registers into a registry of the host that the host passes to it. The template code
// of the registry runs in both modules on the same object, and the factory of the entry is code
// of the plugin.
BOOST_AUTO_TEST_CASE(test_a_plugin_registers_in_a_registry_of_the_host) {
    SharedLibrary plugin;
    BOOST_REQUIRE_MESSAGE(plugin.open(TEST_DYNAMICREGISTRY_PLUGIN_PATH), plugin.errorMessage());

    auto registerWidget =
        reinterpret_cast<PluginWidgetRegistry::AddFactory *(*) (PluginWidgetRegistry *,
                                                                const char *, int)>(
            plugin.resolve("registry_plugin_register"));
    auto unregisterWidget = reinterpret_cast<void (*)(PluginWidgetRegistry::AddFactory *)>(
        plugin.resolve("registry_plugin_unregister"));
    BOOST_REQUIRE(registerWidget && unregisterWidget);

    PluginWidgetRegistry here;
    const auto registration = registerWidget(&here, "from-the-plugin", 7);
    BOOST_REQUIRE(registration);

    auto entry = here.find("from-the-plugin");
    BOOST_REQUIRE(entry);
    BOOST_CHECK(entry == registration->entry());
    BOOST_CHECK_EQUAL(entry->desc(), "registered by the plugin");
    auto widget = entry->instantiate();
    BOOST_REQUIRE(widget);
    BOOST_CHECK_EQUAL(widget->tag(), 7);

    unregisterWidget(registration);
    BOOST_CHECK(!here.find("from-the-plugin"));
}

#endif // TEST_DYNAMICREGISTRY_PLUGIN_PATH

BOOST_AUTO_TEST_SUITE_END()
