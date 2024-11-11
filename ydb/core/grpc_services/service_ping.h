#pragma once

#include <memory>

namespace NKikimr::NGRpcService {

class IRequestOpCtx;
class IRequestNoOpCtx;
class IFacilityProvider;

void DoPing(std::unique_ptr<IRequestNoOpCtx> p, const IFacilityProvider& f);

} // namespace NKikimr::NGRpcService
