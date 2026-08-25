// SPDX-License-Identifier: MIT

#include "test_staticregistry_hub.h"

// The library that owns the list. The test binary and the loaded module both link it, which is
// the arrangement a plugin host is in: neither of them owns the registry, a third thing does.
STDC_INSTANTIATE_STATIC_REGISTRY_EXPORT(HubWidget, TEST_STATICREGISTRY_HUB_API)
