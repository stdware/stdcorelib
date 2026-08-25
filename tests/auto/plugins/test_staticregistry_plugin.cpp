// SPDX-License-Identifier: MIT

#include <cstddef>

#include "test_staticregistry_hub.h"

namespace {

    class ModuleWidget : public HubWidget {
    public:
        int tag() const override {
            return 7;
        }
    };

    // The whole feature. Loading this module is the registration and unloading it is the
    // removal, with nothing on either side calling an initialization function.
    HubRegistry::Add<ModuleWidget> entry(TEST_STATICREGISTRY_PLUGIN_ENTRY,
                                         "registered by a module");

}

extern "C" STDC_DECL_EXPORT size_t staticregistry_plugin_size() {
    size_t n = 0;
    for (const auto &e : HubRegistry::entries()) {
        (void) e;
        ++n;
    }
    return n;
}
