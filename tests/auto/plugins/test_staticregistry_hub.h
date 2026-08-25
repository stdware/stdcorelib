// SPDX-License-Identifier: MIT

#ifndef STDC_TEST_STATICREGISTRY_HUB_H
#define STDC_TEST_STATICREGISTRY_HUB_H

#include <stdcorelib/support/staticregistry.h>

#ifdef TEST_STATICREGISTRY_HUB_LIBRARY
#  define TEST_STATICREGISTRY_HUB_API STDC_DECL_EXPORT
#else
#  define TEST_STATICREGISTRY_HUB_API STDC_DECL_IMPORT
#endif

/// The type both sides name, so that both mean the same registry.
struct HubWidget {
    virtual ~HubWidget() = default;
    virtual int tag() const = 0;
};

#ifdef _MSC_VER
// The list lives in this library, and MSVC reaches exported data only through the import table,
// so the reference has to be declared as imported. MinGW is left out on purpose: the declaration
// also suppresses local instantiation of Entry, Node, Iterator and add_node, which GCC does not
// export from a class attribute, and the module then fails to link against any of them.
extern template class TEST_STATICREGISTRY_HUB_API stdc::StaticRegistry<HubWidget>;
#endif

using HubRegistry = stdc::StaticRegistry<HubWidget>;

/// The name a loaded module registers under, spelled once for both sides.
#define TEST_STATICREGISTRY_PLUGIN_ENTRY "module-widget"

/// The symbol the module exports, answering how many entries it can see. One list or two is the
/// question, and only the module can say what it sees.
#define TEST_STATICREGISTRY_PLUGIN_SIZE_SYMBOL "staticregistry_plugin_size"

#endif // STDC_TEST_STATICREGISTRY_HUB_H
