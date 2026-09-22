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

    virtual void ClearForRecycle() noexcept;

    void PrepareWrite(TRope&& data, ui64 offset, TChunkIdx chunkIdx, ui32 chunkOffset);
    void PrepareRead(size_t size, ui64 offset, TChunkIdx chunkIdx, ui32 chunkOffset);

    void Reinit(const IEventHandle* ev = nullptr);

    void SetSpan(NWilson::TSpan&& span) { Span = std::move(span); }
    NWilson::TSpan& GetSpan() { return Span; }
    NWilson::TSpan ExtractSpan() { return std::move(Span); }

    void SetCookie(ui64 cookie) { Cookie = cookie; }
    ui64 GetCookie() const { return Cookie; }
    void SetCompletionCookie(ui64 cookie) { CompletionCookie = cookie; }
    ui64 GetCompletionCookie() const { return CompletionCookie; }

    const TActorId& GetDDiskId() const { return DDiskId; }
    const TActorId& GetOriginalRequester() const { return OriginalRequester; }
    const TActorId& GetInterconnectSession() const { return InterconnectSession; }

    TRope ExtractData();

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

    NWilson::TSpan Span;

    ui64 CompletionCookie = 0;
    TRcBuf AlignedDataHolder;
    std::optional<TRope> Data;

};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TDDiskIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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

    void SetChunkKey(ui64 tabletId, ui64 vChunkIndex) {
        TabletId = tabletId;
        VChunkIndex = vChunkIndex;
        HasChunkKey = true;
    }

private:
    ui64 TabletId = 0;
    ui64 VChunkIndex = 0;
    bool HasChunkKey = false;
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

class TDDiskActor::TInternalSyncWriteOp final : public TDDiskActor::TDirectIoOpBase {
public:
    explicit TInternalSyncWriteOp(TDDiskActor& actor)
        : TDirectIoOpBase(actor)
    {}

    void Reply(
        NActors::TActorSystem* actorSystem, NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
        TString reason = {}) noexcept override;

    void ClearForRecycle() noexcept override;
    void SelfRecycle() noexcept override;


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
