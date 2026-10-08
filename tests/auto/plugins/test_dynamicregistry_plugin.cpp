// SPDX-License-Identifier: MIT

#include "test_dynamicregistry_plugin.h"

namespace {

    struct PluginWidgetImpl : PluginWidget {
        explicit PluginWidgetImpl(int t) : _tag(t) {
        }
        int tag() const override {
            return _tag;
        }
        int _tag;
    };

}

TEST_DYNAMICREGISTRY_PLUGIN_API PluginWidgetRegistry::AddFactory *
    registry_plugin_register(PluginWidgetRegistry *registry, const char *name, int tag) {
    return new PluginWidgetRegistry::AddFactory(*registry, name, "registered by the plugin", [tag] {
        return std::unique_ptr<PluginWidget>(new PluginWidgetImpl(tag));
    });
}

TEST_DYNAMICREGISTRY_PLUGIN_API void
    registry_plugin_unregister(PluginWidgetRegistry::AddFactory *registration) {
    delete registration;
}
