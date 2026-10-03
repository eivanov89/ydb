#pragma once

#include "ddisk_actor.h"

#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_data.h>

#include <util/generic/overloaded.h>
#include <ydb/core/util/stlog.h>

#include <cerrno>
#include <optional>

namespace NKikimr::NDDisk {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TDirectIoOpBase
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Direct I/O operation context passed through io_uring.
// Allocated via TDDiskActor::AllocateOp (pool-backed) or new,
// recycled back to the pool via SelfRecycle / TDDiskActor::ReturnOp.
class TDDiskActor::TDirectIoOpBase : public NPDisk::TUringOperationBase {
public:
    explicit TDirectIoOpBase(TDDiskActor& actor);

    virtual ~TDirectIoOpBase();

    // IO uring callbacks
    virtual void OnComplete(NActors::TActorSystem* actorSystem) noexcept override final;
    virtual void OnDrop(NActors::TActorSystem* actorSystem) noexcept override final;

    // reply should not access raw uring result field – use just status and data if status OK
    virtual void Reply(
        NActors::TActorSystem* actorSystem, NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
        TString reason = {}) noexcept = 0;
    ui32 RetryCount = 0;
    static constexpr ui32 MaxResubmissions = 20;
    virtual bool IsRestoreIo() const noexcept { return false; }
    virtual bool IsIntegrityIo() const noexcept { return false; }
    virtual bool IsChunkFormatIo() const noexcept { return false; }

    bool IsCriticalDDiskIo() const noexcept { return IsIntegrityIo() || IsChunkFormatIo(); }

    // Selective retries may submit fewer bytes than the original logical request.
    virtual ui64 GetAccountingSize() const noexcept {
        return GetTotalSize();
    }

    virtual void ClearForRecycle() noexcept;

    void PrepareWrite(TRope&& data, ui64 offset, TChunkIdx chunkIdx, ui32 chunkOffset);
    void PrepareRead(size_t size, ui64 offset, TChunkIdx chunkIdx, ui32 chunkOffset);

    void Reinit(const IEventHandle* ev = nullptr);

    void SetCookie(ui64 cookie) { Cookie = cookie; }
    ui64 GetCookie() const { return Cookie; }
    void SetCompletionCookie(ui64 cookie) {
        CompletionCookie = cookie;
    }

    ui64 GetCompletionCookie() const {
        return CompletionCookie;
    }

    const TActorId& GetDDiskId() const { return DDiskId; }
    const TActorId& GetOriginalRequester() const { return OriginalRequester; }
    const TActorId& GetInterconnectSession() const { return InterconnectSession; }

    TRope ExtractData();
    TReadPayload ExtractReadPayload();

    double TimePassed() const;

public:
    // methods to use when we fallback to PDisk instead of direct I/O

    TChunkIdx GetChunkIdx() const { return ChunkIdx; }
    ui32 GetChunkOffset() const { return ChunkOffsetInBytes; }

    using NPDisk::TUringOperationBase::SetResult;

    void SetResult(i64 result, TRope&& data);

protected:
    TDDiskActor& Actor;
    const TActorId DDiskId;

    virtual void SelfRecycle() noexcept { delete this; }
    virtual bool PrepareRetry() noexcept;


private:
    class TCompletionGuard;
    void AccountShortIo() noexcept;

    NHPTimer::STime StartTs;

    TActorId OriginalRequester;
    TActorId InterconnectSession;

    ui64 Cookie = 0;

    // PDisk fallback data
    TChunkIdx ChunkIdx = 0;
    ui32 ChunkOffsetInBytes = 0;

    ui64 CompletionCookie = 0;
    TRcBuf AlignedDataHolder;
    std::optional<TRope> Data;

};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TDDiskIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Data-path operation of one read, write, or metadata pair load. The shared callback
// owns its result slots even when forced teardown destroys the requesting frame.
class TDDiskActor::TDDiskIoOp final : public TDDiskActor::TDirectIoOpBase {
public:
    explicit TDDiskIoOp(TDDiskActor& actor)
        : TDirectIoOpBase(actor)
    {}

    void Reply(
        NActors::TActorSystem* actorSystem, NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
        TString reason = {}) noexcept override;

    void ClearForRecycle() noexcept override;
    void SelfRecycle() noexcept override;

    void Reinit(const IEventHandle* ev = nullptr) {
        TDirectIoOpBase::Reinit(ev);
        Callback.reset();
        PairIndex.reset();
        Critical = false;
    }

    void SetCallback(std::shared_ptr<TBatchedIOAwaiter> callback,
            std::optional<size_t> pairIndex = std::nullopt)
    {
        Callback = std::move(callback);
        PairIndex = pairIndex;
    }

    // Metadata pair loads share the integrity retry and fail-stop policy.
    void SetCritical() {
        Critical = true;
    }

    bool IsIntegrityIo() const noexcept override {
        return Critical;
    }

private:
    std::shared_ptr<TBatchedIOAwaiter> Callback;
    std::optional<size_t> PairIndex;
    bool Critical = false;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TPersistentBufferPartIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TDDiskActor::TPersistentBufferPartIoOp final : public TDDiskActor::TDirectIoOpBase {
public:
    explicit TPersistentBufferPartIoOp(TDDiskActor& actor)
        : TDirectIoOpBase(actor)
    {}

    void Reply(
        NActors::TActorSystem* actorSystem, NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
        TString reason = {}) noexcept override;

    void ClearForRecycle() noexcept override;
    void SelfRecycle() noexcept override;

    void SetPartCookie(ui64 partCookie) {
        PartCookie = partCookie;
    }

    void SetIsErase(bool isErase) {
        IsErase = isErase;
    }

    bool IsRestoreIo() const noexcept override { return IsRestore; }

    void SetIsRestore(bool isRestore) {
        IsRestore = isRestore;
    }

private:
    ui64 PartCookie = 0;
    bool IsErase = false;
    bool IsRestore = false;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TIntegrityIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Executes one integrity host read/write and posts TEvPrivate::TEvIntegrityIoResult
// back to the actor.
class TDDiskActor::TIntegrityIoOp final : public TDDiskActor::TDirectIoOpBase {
public:
    explicit TIntegrityIoOp(TDDiskActor& actor)
        : TDirectIoOpBase(actor)
    {}

    void Reply(
        NActors::TActorSystem* actorSystem, NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
        TString reason = {}) noexcept override;
    bool IsIntegrityIo() const noexcept override { return true; }

    void ClearForRecycle() noexcept override;
    void SelfRecycle() noexcept override;


};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TChunkFormatIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TDDiskActor::TChunkFormatIoOp final : public TDDiskActor::TDirectIoOpBase {
public:
    explicit TChunkFormatIoOp(TDDiskActor& actor)
        : TDirectIoOpBase(actor)
    {}

    void Reply(
        NActors::TActorSystem* actorSystem, NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
        TString reason = {}) noexcept override;
    bool IsChunkFormatIo() const noexcept override { return true; }

    void SetFormatRange(TChunkIdx chunkIdx, ui32 offsetInBytes, ui32 size) {
        ChunkIdx = chunkIdx;
        OffsetInBytes = offsetInBytes;
        Size = size;
    }

private:
    TChunkIdx ChunkIdx = 0;
    ui32 OffsetInBytes = 0;
    ui32 Size = 0;
};

} // namespace NKikimr::NDDisk
