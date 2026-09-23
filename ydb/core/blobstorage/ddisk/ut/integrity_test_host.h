#pragma once

#include <ydb/core/blobstorage/ddisk/integrity_manager.h>
#include <ydb/library/actors/async/continuation.h>
#include <ydb/library/actors/async/ut/common.h>

namespace NKikimr::NDDisk {
namespace NIntegrityTest {

// The fixture records ordinary pair submissions and awaitable writes/allocations, then
// delivers all completions on the actor thread.
class THost : public TIntegrityManager::IHost {
public:
    struct TAllocateIntegrityChunk {};
    struct TWriteIo {
        ui64 IoId;
        TChunkIdx ChunkIdx;
        ui32 OffsetInBytes;
        TRcBuf Data;
        TIntegrityManager::EWriteIoKind Kind;
    };
    struct TReadIo {
        ui64 IoId;
        TChunkIdx ChunkIdx;
        ui32 OffsetInBytes;
        ui32 Size;
    };
    using TAction = std::variant<TAllocateIntegrityChunk, TWriteIo, TReadIo>;
    NAsyncTest::TAsyncTestActor::TState State;
    NAsyncTest::TAsyncTestActorRuntime Runtime;
    NActors::IActor* ActorPtr = nullptr;
    NAsyncTest::TAsyncTestActorRuntime::TAsyncActorOperations Mailbox;
    std::vector<TAction> Submissions;
    size_t LaunchCount = 0;
    ui64 NextIo = 1;
    TChunkIdx ImmediateChunk = 0;
    bool ImmediateWrites = false;
    std::map<ui64, NActors::TAsyncContinuation<bool>> Writes;
    std::map<ui64, ui64> Reads;
    std::deque<NActors::TAsyncContinuation<TChunkIdx>> Allocations;
    std::vector<TChunkIdx> Returned;

    THost() : Mailbox(Runtime.StartAsyncActor(State, [this](auto* actor) -> NActors::async<void> {
        ActorPtr = actor;
        co_return;
    })) {}
    NActors::IActor& Actor() override { return *ActorPtr; }
    static void Run(NActors::IActor& actor, std::function<NActors::async<void>()> factory) {
        Y_UNUSED(actor);
        co_await factory();
    }
    void Launch(std::function<NActors::async<void>()> factory) override {
        ++LaunchCount;
        Run(Actor(), std::move(factory));
    }
    NActors::async<TChunkIdx> Allocate() override {
        if (ImmediateChunk) { co_return ImmediateChunk++; }
        co_return co_await NActors::WithAsyncContinuation<TChunkIdx>([this](auto continuation) {
            Allocations.push_back(std::move(continuation));
            Submissions.emplace_back(TAllocateIntegrityChunk{});
        });
    }
    void ReturnChunk(TChunkIdx chunk) override { Returned.push_back(chunk); }
    NActors::async<bool> Write(TChunkIdx chunk, ui32 offset, TRcBuf data,
            TIntegrityManager::EWriteIoKind kind) override {
        if (ImmediateWrites) { co_return true; }
        co_return co_await NActors::WithAsyncContinuation<bool>([&](auto continuation) {
            const ui64 id = NextIo++;
            Writes.emplace(id, std::move(continuation));
            Submissions.emplace_back(TWriteIo{id, chunk, offset, std::move(data), kind});
        });
    }
    void SubmitPairReads(std::vector<TIntegrityManager::TPairRead> reads) override {
        for (const auto& read : reads) {
            const ui64 id = NextIo++;
            Reads.emplace(id, read.Id);
            Submissions.emplace_back(TReadIo{id, read.ChunkIdx, read.OffsetInBytes, read.Size});
        }
    }
};

class TFixture : private THost, public TIntegrityManager {
public:
    using THost::TAllocateIntegrityChunk;
    using THost::TWriteIo;
    using THost::TReadIo;
    using THost::TAction;
    enum class EOperationKind { Read, Write };
    struct TOperationResult : TIntegrityManager::TOperationResult {
        ui64 OperationId;
        EOperationKind Kind;
    };
    std::vector<TDataChunkKey> Placed, Ready;
    std::vector<TOperationResult> Completed;
    ui64 NextOperation = 1;

    TFixture(ui64 size, ui64 ddisk, ui64 guid, ui64 cache = DefaultChecksumCacheBytes)
        : TIntegrityManager(static_cast<THost&>(*this), size, ddisk, guid, cache) {}
    ~TFixture() { Runtime.CleanupNode(); }

    std::vector<TAction> TakeActions() { return std::exchange(Submissions, {}); }
    bool HasActions() const { return !Submissions.empty(); }
    size_t GetHostLaunchCount() const { return LaunchCount; }
    std::vector<TDataChunkKey> TakePlacedKeys() { return std::exchange(Placed, {}); }
    std::vector<TOperationResult> TakeCompletedOperations() { return std::exchange(Completed, {}); }
    void UseSynchronousHost(TChunkIdx firstChunk) { ImmediateChunk = firstChunk; ImmediateWrites = true; }
    void InActor(std::function<void(NActors::IActor&)> callback) {
        Mailbox.RunSync([&] { callback(Actor()); });
    }
    void ObserveExtent(TIntegrityManager::TExtent extent, bool ready, std::optional<bool>& result) {
        Launch([this, extent, ready, &result]() -> NActors::async<void> {
            result = ready ? co_await extent.WaitReady(Actor()) : co_await extent.WaitPlaced(Actor());
        });
    }
    const std::vector<TChunkIdx>& ReturnedChunks() const { return Returned; }
    void OnDataChunkAllocated(TDataChunkKey key, TChunkIdx chunk) {
        Mailbox.RunSync([&] {
            auto extent = StartExtent(key, chunk);
            Launch([this, key, extent]() -> NActors::async<void> {
                if (co_await extent.WaitPlaced(Actor())) { Placed.push_back(key); }
                if (co_await extent.WaitReady(Actor())) { Ready.push_back(key); }
            });
        });
    }
    void OnIntegrityChunkAllocated(TChunkIdx chunk) {
        Mailbox.RunSync([&] {
            UNIT_ASSERT(!Allocations.empty());
            auto continuation = std::move(Allocations.front());
            Allocations.pop_front();
            continuation.Resume(chunk);
        });
    }
    std::vector<TDataChunkKey> OnIoCompleted(ui64 id, bool ok = true) {
        Ready.clear();
        Mailbox.RunSync([&] {
            auto continuation = std::move(Writes.at(id));
            Writes.erase(id);
            continuation.Resume(ok);
        });
        return std::exchange(Ready, {});
    }
    void OnReadIoCompleted(ui64 id, TRope data, bool ok = true) {
        Mailbox.RunSync([&] {
            TPairReadResult result{Reads.at(id), TIoResult{ok, std::move(data)}};
            Reads.erase(id);
            CompletePairReads(TConstArrayRef<TPairReadResult>(&result, 1));
        });
    }
    ui64 BeginBlocksWrite(TDataChunkKey key, ui32 offset, ui32 size, const std::vector<ui64>& checksums) {
        const ui64 id = NextOperation++;
        Mailbox.RunSync([&] { Observe(StartWrite(key, offset, size, checksums), id, EOperationKind::Write); });
        return id;
    }
    ui64 BeginChecksumRead(TDataChunkKey key, ui32 offset, ui32 size) {
        const ui64 id = NextOperation++;
        Mailbox.RunSync([&] { Observe(StartRead(key, offset, size), id, EOperationKind::Read); });
        return id;
    }
    void Observe(TOperation operation, ui64 id, EOperationKind kind) {
        Launch([this, operation, id, kind]() -> NActors::async<void> {
            auto result = co_await operation.Wait(Actor());
            Completed.push_back(TOperationResult{std::move(result), id, kind});
        });
    }
    void PrepareTabletChunksDeletion(ui64 tablet) {
        Mailbox.RunSync([&] { TIntegrityManager::PrepareTabletChunksDeletion(tablet); });
    }
    void CommitTabletChunksDeletion(ui64 tablet) {
        Mailbox.RunSync([&] { TIntegrityManager::CommitTabletChunksDeletion(tablet); });
    }
    std::vector<TChunkIdx> TakeReleasableIntegrityChunks() {
        std::vector<TChunkIdx> result;
        Mailbox.RunSync([&] { result = TIntegrityManager::TakeReleasableIntegrityChunks(); });
        return result;
    }
};
}
}
