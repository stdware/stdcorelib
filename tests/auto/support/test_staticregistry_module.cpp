// SPDX-License-Identifier: MIT

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <stdcorelib/platform/windows/stdc_windows.h>
#else
#  include <dlfcn.h>
#endif

#include <stdcorelib/support/sharedlibrary.h>

#include "../plugins/test_staticregistry_hub.h"

#include <boost/test/unit_test.hpp>

namespace fs = std::filesystem;

using namespace stdc;

namespace {

    const char ModulePath[] = TEST_STATICREGISTRY_PLUGIN_PATH;

    fs::path module_path() {
        return fs::path(ModulePath);
    }

    // Asked of the loader rather than of the object that was supposed to have let go. glibc keeps
    // a library that defines a unique symbol, and dlclose reports success either way, so what the
    // registry did and what the loader did are two different questions.
    bool still_loaded(const fs::path &path) {
#ifdef _WIN32
        return ::GetModuleHandleW(path.c_str()) != nullptr;
#else
        void *handle = ::dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);
        if (handle) {
            ::dlclose(handle);
        }
        return handle != nullptr;
#endif
    }

    std::vector<std::string> names() {
        std::vector<std::string> v;
        for (const auto &entry : HubRegistry::entries()) {
            v.push_back(std::string(entry.name()));
        }
        return v;
    }

    bool has(const std::vector<std::string> &v, const std::string &name) {
        return std::find(v.begin(), v.end(), name) != v.end();
    }

    class HostWidget : public HubWidget {
    public:
        int tag() const override {
            return 1;
        }
    };

    using Registration = std::optional<HubRegistry::Add<HostWidget>>;

}

BOOST_AUTO_TEST_SUITE(test_staticregistry_module)

// One list, not two. A module reaching a list of its own would pass every check made from
// inside it and be invisible from here, so both sides are asked how much they can see.
BOOST_AUTO_TEST_CASE(test_module_joins_the_hosts_list) {
    BOOST_REQUIRE(fs::exists(module_path()));

    const auto before = names();
    BOOST_REQUIRE(!has(before, TEST_STATICREGISTRY_PLUGIN_ENTRY));

    SharedLibrary lib;
    BOOST_REQUIRE(lib.open(module_path()));

    const auto after = names();
    BOOST_REQUIRE_EQUAL(after.size(), before.size() + 1);
    BOOST_CHECK_EQUAL(after.back(), TEST_STATICREGISTRY_PLUGIN_ENTRY);

    auto size_from_module =
        reinterpret_cast<size_t (*)()>(lib.resolve(TEST_STATICREGISTRY_PLUGIN_SIZE_SYMBOL));
    BOOST_REQUIRE(size_from_module);
    BOOST_CHECK_EQUAL(size_from_module(), after.size());

    // What it registered is usable from here. The object is destroyed while the module is still
    // mapped, since its destructor lives there.
    for (const auto &entry : HubRegistry::entries()) {
        if (entry.name() == TEST_STATICREGISTRY_PLUGIN_ENTRY) {
            auto widget = entry.instantiate();
            BOOST_REQUIRE(widget);
            BOOST_CHECK_EQUAL(widget->tag(), 7);
        }
    }

    BOOST_REQUIRE(lib.close());
}

// The point of the exercise. Nothing tells the registry the module is going, its destructor is
// what takes the entry out.
BOOST_AUTO_TEST_CASE(test_unloading_takes_the_entry_back_out) {
    BOOST_REQUIRE(fs::exists(module_path()));

    Registration first, second, third;
    first.emplace("host-first", "");

    SharedLibrary lib;
    BOOST_REQUIRE(lib.open(module_path()));

    // Registered after the module, so its entry sits between two the host holds and leaving the
    // list means mending both directions rather than moving the head or the tail.
    second.emplace("host-second", "");

    const auto loaded = names();
    BOOST_REQUIRE(has(loaded, TEST_STATICREGISTRY_PLUGIN_ENTRY));

    BOOST_REQUIRE(lib.close());

    if (still_loaded(module_path())) {
        BOOST_TEST_MESSAGE("the loader kept the module, so there was no unload to observe");
        return;
    }

    const auto unloaded = names();
    BOOST_CHECK(!has(unloaded, TEST_STATICREGISTRY_PLUGIN_ENTRY));
    BOOST_REQUIRE_EQUAL(unloaded.size(), loaded.size() - 1);
    BOOST_CHECK(has(unloaded, "host-first"));
    BOOST_CHECK(has(unloaded, "host-second"));

    // The list is still whole in both directions: what comes next lands after the last of these
    // rather than after a node in a module that is no longer mapped.
    third.emplace("host-third", "");
    BOOST_CHECK_EQUAL(names().back(), "host-third");
}

BOOST_AUTO_TEST_SUITE_END()
