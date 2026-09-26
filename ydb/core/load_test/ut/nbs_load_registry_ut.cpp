#include <ydb/core/load_test/nbs_load_registry.h>
#include <library/cpp/testing/unittest/registar.h>

using namespace NKikimr::NNbsDbgLike;

Y_UNIT_TEST_SUITE(NbsLoadRegistry) {
    Y_UNIT_TEST(RetryKeepsPlacementAndRejectsChangedInputs) {
        TRunRegistry registry;
        TRunRegistry::TControl request;
        request.SetRequestId("id");
        request.SetDatabase("/Root");
        request.MutableLoad()->MutableNbsDbgLikeLoad()->AddTargets()->SetTabletId(42);
        TString error;
        auto* first = registry.Admit(request, TInstant::Seconds(100), error);
        UNIT_ASSERT(first);
        first->Result.MutableEffectiveConfig()->MutableNbsDbgLikeLoad()->MutableTargets(0)->SetNodeId(5);
        auto* retry = registry.Admit(request, TInstant::Seconds(200), error);
        UNIT_ASSERT(first == retry);
        UNIT_ASSERT_VALUES_EQUAL(retry->Result.GetEffectiveConfig().GetNbsDbgLikeLoad().GetTargets(0).GetNodeId(), 5);
        request.MutableLoad()->MutableNbsDbgLikeLoad()->MutableTargets(0)->SetNodeId(0);
        UNIT_ASSERT(!registry.Admit(request, TInstant::Seconds(200), error));
        request.MutableLoad()->MutableNbsDbgLikeLoad()->MutableTargets(0)->SetNodeId(6);
        UNIT_ASSERT(!registry.Admit(request, TInstant::Seconds(200), error));
        UNIT_ASSERT(!error.empty());
    }

    Y_UNIT_TEST(CapacityAndRetentionDoNotEvictActiveOrRecentResults) {
        TRunRegistry registry;
        TString error;
        for (ui32 i = 0; i < 1024; ++i) {
            TRunRegistry::TControl request;
            request.SetRequestId(ToString(i));
            UNIT_ASSERT(registry.Admit(request, TInstant::Seconds(100), error));
        }
        TRunRegistry::TControl next;
        next.SetRequestId("next");
        UNIT_ASSERT(!registry.Admit(next, TInstant::Seconds(100) + TDuration::Hours(48), error));
        auto& done = registry.Runs.at("0");
        done.Result.SetState(TRunRegistry::TResult::FAILED);
        done.Result.SetFinishedAtMs(TInstant::Seconds(200).MilliSeconds());
        UNIT_ASSERT(!registry.Admit(next, TInstant::Seconds(199) + TDuration::Hours(24), error));
        UNIT_ASSERT(registry.Admit(next, TInstant::Seconds(200) + TDuration::Hours(24), error));
        UNIT_ASSERT(!registry.Runs.contains("0"));
        UNIT_ASSERT_VALUES_EQUAL(registry.Runs.size(), 1024);
    }

    Y_UNIT_TEST(IncarnationChangesAcrossRestart) {
        TRunRegistry first;
        TRunRegistry second;
        UNIT_ASSERT(first.Incarnation != second.Incarnation);
    }
}
