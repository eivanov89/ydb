#include <library/cpp/testing/unittest/registar.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

#include "kqp_shared_multitimer.h"

namespace {

using namespace NActors;

class TTimeoutUser : public TActorBootstrapped<TTimeoutUser> {
    using TBase = TActorBootstrapped<TTimeoutUser>;

public:
    TTimeoutUser() {
    }

    ~TTimeoutUser() {
    }

    void Bootstrap(const TActorContext& /*ctx*/) {
        Become(&TTimeoutUser::StateFunc);
    }

    void HandlePoison(TEvents::TEvPoisonPill::TPtr& ev, const TActorContext& ctx) {
        ctx.Send(ev->Sender, new TEvents::TEvPoisonTaken);
        Die(ctx);
    }

    STFUNC(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvPoisonPill, HandlePoison);
            default:
                Y_ABORT();
        }
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TSharedMultitimerTest) {
    Y_UNIT_TEST(Basic) {
        TTestBasicRuntime runtime;
        runtime.Initialize(NKikimr::TAppPrepare().Unwrap());
        TActorId worker = runtime.Register(new TTimeoutUser);
        TActorId edge = runtime.AllocateEdgeActor();
        runtime.Schedule(new IEventHandle(worker, edge, new TEvents::TEvPoisonPill), TDuration::Seconds(1));
        runtime.DispatchEvents();
        TAutoPtr<IEventHandle> handle;
        runtime.GrabEdgeEventRethrow<TEvents::TEvPoisonTaken>(handle);
    }
}
