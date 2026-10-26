#include <ydb/apps/bridge_skipper/runtime/resources.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <util/datetime/base.h>
#include <util/folder/path.h>
#include <util/folder/tempdir.h>
#include <util/stream/file.h>
#include <limits>

namespace NBridgeSkipper {
namespace {

class TManualClock final : public ITimeProvider {
public:
    std::atomic<ui64> Microseconds{TInstant::Seconds(100).MicroSeconds()};

    TInstant Now() override {
        return TInstant::MicroSeconds(Microseconds.load());
    }
};

void RegisterClock() {
    TSkipperClock::RegisterProvider(MakeIntrusive<TManualClock>());
}

struct TResourceFixture {
    TTempDir Directory;
    TConfig Config;
    std::shared_ptr<TRuntimeResources> Resources;

    TResourceFixture() {
        RegisterClock();
        Config = ParseConfig("schema_version: 1\ncluster_id: cluster\nydb_endpoints: [grpc://localhost:2135]\n");
        Resources = std::make_shared<TRuntimeResources>(Config, Directory.Name(), "member");
    }

    ~TResourceFixture() {
        Resources->CompletionGate->Close();
        Resources->StopStorage();
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TSkipperProcessResources) {
    Y_UNIT_TEST(LocalStopIsImmediateAndStaleReleaseCannotClearNewerStop) {
        RegisterClock();
        TLocalStopLatch latch("process");
        UNIT_ASSERT(!latch.IsStopped());
        const auto first = latch.Stop("first", "body-one", "emergency");
        UNIT_ASSERT(latch.IsStopped());
        const auto next = latch.Stop("next", "body-two", "new emergency");
        UNIT_ASSERT_VALUES_EQUAL(next.Revision, first.Revision + 1);
        UNIT_ASSERT(!latch.Release(first.Revision));
        UNIT_ASSERT(latch.IsStopped());
        UNIT_ASSERT(latch.Release(next.Revision));
        UNIT_ASSERT(!latch.IsStopped());
    }

    Y_UNIT_TEST(StopDeduplicationRetainsOriginalReceiptAndRejectsChangedBody) {
        RegisterClock();
        TLocalStopLatch latch("process");
        const auto first = latch.Stop("request", "hash", "first reason");
        latch.SetPersistence(first.Revision, true);
        const auto duplicate = latch.Stop("request", "hash", "first reason");
        UNIT_ASSERT_VALUES_EQUAL(duplicate.Revision, first.Revision);
        UNIT_ASSERT(duplicate.Durable);
        const auto conflict = latch.Stop("request", "changed-hash", "different reason");
        UNIT_ASSERT_VALUES_EQUAL(conflict.Error, "REQUEST_ID_REUSED");
        UNIT_ASSERT_VALUES_EQUAL(latch.Current().Revision, first.Revision);
    }

    Y_UNIT_TEST(OldMarkerCompletionCannotChangeNewStopPersistence) {
        RegisterClock();
        TLocalStopLatch latch("process");
        const auto old = latch.Stop("old", "old-hash", "old");
        const auto current = latch.Stop("current", "current-hash", "current");
        latch.SetPersistence(old.Revision, true);
        UNIT_ASSERT(!latch.Current().Durable);
        UNIT_ASSERT(latch.Current().Pending);
        latch.SetPersistence(current.Revision, false, "VOLATILE");
        UNIT_ASSERT(latch.IsStopped());
        UNIT_ASSERT_VALUES_EQUAL(latch.Current().Error, "VOLATILE");
        UNIT_ASSERT(latch.Lookup("old")->Durable);
    }

    Y_UNIT_TEST(ProcessIncarnationChangesAndDurableMembershipPersists) {
        TResourceFixture fixture;
        const auto process = fixture.Resources->ProcessId();
        const auto membership = fixture.Resources->InitialState().MembershipIncarnation;
        fixture.Resources->StopStorage();
        auto restarted = std::make_shared<TRuntimeResources>(fixture.Config, fixture.Directory.Name(), "member");
        UNIT_ASSERT(restarted->ProcessId() != process);
        UNIT_ASSERT_VALUES_EQUAL(restarted->InitialState().MembershipIncarnation, membership);
        restarted->StopStorage();
    }

    Y_UNIT_TEST(StopMarkerIsLoadedBeforeAnyAdmissionAndReleaseIsExplicit) {
        TResourceFixture fixture;
        const auto receipt = fixture.Resources->StopLocal("stop", {}, "stop-hash", "maintenance");
        UNIT_ASSERT(receipt.Stopped);
        fixture.Resources->StopStorage(); // Drains marker fsync without sleeping.
        auto restarted = std::make_shared<TRuntimeResources>(fixture.Config, fixture.Directory.Name(), "member");
        const auto stop = restarted->StopLatch->Current();
        UNIT_ASSERT(stop.Stopped);
        UNIT_ASSERT(stop.Durable);
        UNIT_ASSERT_VALUES_EQUAL(restarted->ReleaseLocal("wrong", fixture.Resources->ProcessId(), stop.Revision).Error, "STALE_LOCAL_STOP");
        restarted->ReleaseLocal("release", restarted->ProcessId(), stop.Revision, {}, "release-hash", "finished");
        restarted->StopStorage();
        UNIT_ASSERT(!restarted->StopLatch->IsStopped());
        UNIT_ASSERT(!restarted->LookupLocalReceipt("release")->Stopped);
    }

    Y_UNIT_TEST(ReleaseReplayPrecedesCurrentRevisionAndChangedBodyConflicts) {
        TResourceFixture fixture;
        const auto stop = fixture.Resources->StopLocal("stop", {}, "stop-hash", "maintenance");
        fixture.Resources->ReleaseLocal("release", fixture.Resources->ProcessId(), stop.Revision, {}, "release-hash", "done");
        fixture.Resources->StopStorage();
        const auto replay = fixture.Resources->ReleaseLocal("release", fixture.Resources->ProcessId(), stop.Revision, {}, "release-hash", "done");
        UNIT_ASSERT(!replay.Stopped);
        UNIT_ASSERT(replay.Durable);
        const auto reused = fixture.Resources->ReleaseLocal("release", fixture.Resources->ProcessId(), stop.Revision, {}, "different-hash", "changed");
        UNIT_ASSERT_VALUES_EQUAL(reused.Error, "REQUEST_ID_REUSED");
    }

    Y_UNIT_TEST(SuccessfulReleaseAfterVolatileStopReportsNoError) {
        TResourceFixture fixture;
        TFsPath(fixture.Directory.Name() + "/local-stop.tmp").MkDir();
        const auto stop = fixture.Resources->StopLocal("stop", {}, "hash", "emergency");
        while (fixture.Resources->StopLatch->Current().Pending) {
            Sleep(TDuration::MilliSeconds(1));
        }
        UNIT_ASSERT_VALUES_EQUAL(fixture.Resources->StopLatch->Current().Error, "STOP_MARKER_VOLATILE");
        const auto release = fixture.Resources->ReleaseLocal("release", fixture.Resources->ProcessId(), stop.Revision, {}, "release-hash", "done");
        UNIT_ASSERT(release.Error.empty());
        fixture.Resources->StopStorage();
        const auto completed = fixture.Resources->LookupLocalReceipt("release");
        UNIT_ASSERT(completed);
        UNIT_ASSERT(!completed->Stopped);
        UNIT_ASSERT(completed->Durable);
        UNIT_ASSERT_VALUES_EQUAL(completed->Error, "");
    }

    Y_UNIT_TEST(DuplicatePendingStopNeverWritesMarkerAfterRelease) {
        TResourceFixture fixture;
        const auto stop = fixture.Resources->StopLocal("stop", {}, "hash", "emergency");
        fixture.Resources->ReleaseLocal("release", fixture.Resources->ProcessId(), stop.Revision, {}, "release-hash", "done");
        fixture.Resources->StopLocal("stop", {}, "hash", "emergency");
        fixture.Resources->StopStorage();
        auto restarted = std::make_shared<TRuntimeResources>(fixture.Config, fixture.Directory.Name(), "member");
        UNIT_ASSERT(!restarted->StopLatch->IsStopped());
        restarted->StopStorage();
    }

    Y_UNIT_TEST(SubmissionCreditsAreBoundedAndShutdownClosesAdmission) {
        TResourceFixture fixture;
        for (size_t i = 0; i < 256; ++i) {
            UNIT_ASSERT(fixture.Resources->AcquireSubmission());
        }
        UNIT_ASSERT(!fixture.Resources->AcquireSubmission());
        fixture.Resources->ReleaseSubmission();
        UNIT_ASSERT(fixture.Resources->AcquireSubmission());
        fixture.Resources->CloseAdmission();
        UNIT_ASSERT(!fixture.Resources->AcquireSubmission());
        for (size_t i = 0; i < 256; ++i) {
            fixture.Resources->ReleaseSubmission();
        }
    }

    Y_UNIT_TEST(MalformedExistingStopMarkerRefusesStartup) {
        TResourceFixture fixture;
        fixture.Resources->StopStorage();
        TFileOutput(fixture.Directory.Name() + "/local-stop").Write("not-a-revision");
        UNIT_ASSERT_EXCEPTION(std::make_shared<TRuntimeResources>(fixture.Config, fixture.Directory.Name(), "member"), yexception);
    }

    Y_UNIT_TEST(RuntimeJsonRingBoundsRecordsFiltersSeverityAndOmitsSecretKeys) {
        TResourceFixture fixture;
        const auto before = fixture.Resources->ReadLogs(0, 200);
        fixture.Resources->Log("debug_hidden", {{"request_id", "request"}}, "debug");
        UNIT_ASSERT_VALUES_EQUAL(fixture.Resources->ReadLogs(0, 200).Next, before.Next);
        fixture.Resources->Log("safe", {{"request_id", "request"}, {"token", "secret-material"}});
        const auto page = fixture.Resources->ReadLogs(before.Next, 200);
        UNIT_ASSERT_VALUES_EQUAL(page.Lines.size(), 1);
        UNIT_ASSERT(page.Lines.front().Contains("request_id"));
        UNIT_ASSERT(!page.Lines.front().Contains("secret-material"));
        for (size_t i = 0; i < 1001; ++i) {
            fixture.Resources->Log("overflow");
        }
        const auto overflow = fixture.Resources->ReadLogs(1, 200);
        UNIT_ASSERT(overflow.Lost);
        UNIT_ASSERT(overflow.HasMore);
        UNIT_ASSERT_VALUES_EQUAL(overflow.Lines.size(), 200);
    }

    Y_UNIT_TEST(RepeatedDiagnosticsAreLimitedButTransitionsRemainAndValuesRedacted) {
        TResourceFixture fixture;
        const auto before = fixture.Resources->ReadLogs(0, 200).Next;
        fixture.Resources->Log("read_rpc_result", {{"method", "Diagnostic"}, {"status", "TIMEOUT"}, {"endpoint", "grpc://node:2135"}});
        fixture.Resources->Log("read_rpc_result", {{"method", "Diagnostic"}, {"status", "TIMEOUT"}, {"endpoint", "grpc://node:2135"}});
        UNIT_ASSERT_VALUES_EQUAL(fixture.Resources->ReadLogs(before, 200).Lines.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Resources->Counters->GetCounter("skipper_log_suppressed_total", true)->Val(), 1);
        fixture.Resources->Log("mutation_rpc_result", {{"attempt_id", "one"}});
        fixture.Resources->Log("mutation_rpc_result", {{"attempt_id", "two"}});
        fixture.Resources->Log("safe", {{"endpoint", "Bearer credential-secret"}});
        const auto page = fixture.Resources->ReadLogs(before, 200);
        UNIT_ASSERT_VALUES_EQUAL(page.Lines.size(), 4);
        UNIT_ASSERT(!page.Lines.back().Contains("credential-secret"));
        UNIT_ASSERT(fixture.Resources->ReadLogs(std::numeric_limits<ui64>::max(), 200).Reset);
    }
}

} // namespace NBridgeSkipper
