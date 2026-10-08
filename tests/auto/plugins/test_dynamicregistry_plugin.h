// SPDX-License-Identifier: MIT

#ifndef STDC_TEST_DYNAMICREGISTRY_PLUGIN_H
#define STDC_TEST_DYNAMICREGISTRY_PLUGIN_H

#include <stdcorelib/support/dynamicregistry.h>

#define TEST_DYNAMICREGISTRY_PLUGIN_API extern "C" STDC_DECL_EXPORT

/// The type that the plugin registers. Both modules include this declaration, so that both use
/// the same registry type.
struct PluginWidget {
    virtual ~PluginWidget() = default;
    virtual int tag() const = 0;
};

using PluginWidgetRegistry = stdc::DynamicRegistry<PluginWidget>;

/// Registers an entry in \a registry from inside the plugin. The caller passes the returned
/// object to registry_plugin_unregister().
TEST_DYNAMICREGISTRY_PLUGIN_API PluginWidgetRegistry::AddFactory *
    registry_plugin_register(PluginWidgetRegistry *registry, const char *name, int tag);

/// Destroys \a registration, which removes its entry, inside the plugin.
TEST_DYNAMICREGISTRY_PLUGIN_API void
    registry_plugin_unregister(PluginWidgetRegistry::AddFactory *registration);

#endif // STDC_TEST_DYNAMICREGISTRY_PLUGIN_H
