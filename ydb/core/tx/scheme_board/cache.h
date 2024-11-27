#pragma once

#include "defs.h"

#include <ydb/core/tx/scheme_cache/scheme_cache.h>

namespace NKikimr {

IActor* CreateSchemeBoardSchemeCache(NSchemeCache::TSchemeCacheConfig* config);

namespace NSchemeBoard {

// Hacky prototype

void SetSchemeBoardLocalServiceCache(IActor* actor);

// TODO: use underlying Request without wrapping it
std::unique_ptr<TEvTxProxySchemeCache::TEvNavigateKeySetResult> NavigateKeySet(
        std::unique_ptr<TEvTxProxySchemeCache::TEvNavigateKeySet> request);

std::unique_ptr<TEvTxProxySchemeCache::TEvResolveKeySetResult> ResolveKeySet(
        std::unique_ptr<TEvTxProxySchemeCache::TEvResolveKeySet> request);

} // NSchemeBoard
} // NKikimr
