#include "ddisk_actor.h"
#include "ddisk_checksums.h"
#include "direct_io_op.h"

#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_data.h>

#include <ydb/core/util/hp_timer_helpers.h>
#include <ydb/core/util/stlog.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/services/services.pb.h>

#include <util/generic/overloaded.h>
#include <util/stream/format.h>

#include <cerrno>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::BS_DDISK

namespace NKikimr::NDDisk {

static constexpr size_t MaxRwCount = 0x7ffff000ULL; // INT_MAX & PAGE_MASK on 4K pages, ~ 2 GiB
static constexpr size_t MinBlockSize = 4096;

using TReplyStatus = NKikimrBlobStorage::NDDisk::TReplyStatus;

namespace {

// a poor error mapping (we can't map io_uring errors 1:1 to our errors)
TReplyStatus::E UringErrorToStatus(i64 result, NPDisk::TUringOperationBase::EOperationType opType) {
    const int err = static_cast<int>(-result);
    switch (err) {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
        case ENOSPC:
        case ENOMEM:
            return TReplyStatus::OVERLOADED;
        case EINVAL:
            return TReplyStatus::INCORRECT_REQUEST;
        case EIO:
            return opType == NPDisk::TUringOperationBase::EREAD
                ? TReplyStatus::LOST_DATA
                : TReplyStatus::ERROR;
        default:
            return TReplyStatus::ERROR;
    }
}

} // anonymous

// Keep the actor reference independently of the operation: recycling or publishing
// a retry can immediately transfer the operation to another thread.
class TDDiskActor::TDirectIoOpBase::TCompletionGuard {
    TDDiskActor& Actor;
    NActors::TActorSystem* const ActorSystem;
    TDirectIoOpBase* Op;

public:
    TCompletionGuard(TDirectIoOpBase* op, NActors::TActorSystem* actorSystem)
        : Actor(op->Actor)
        , ActorSystem(actorSystem)
        , Op(op)
    {}

    ~TCompletionGuard() {
        if (Op) {
            Op->SelfRecycle();
        }
        Actor.OnDirectIODone(ActorSystem);
    }

    std::unique_ptr<TDirectIoOpBase> Release() {
        return std::unique_ptr<TDirectIoOpBase>(std::exchange(Op, nullptr));
    }

    TCompletionGuard(const TCompletionGuard&) = delete;
    TCompletionGuard& operator=(const TCompletionGuard&) = delete;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TDirectIoOpBase
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TDDiskActor::TDirectIoOpBase::TDirectIoOpBase(TDDiskActor& actor)
    : Actor(actor)
    , DDiskId(actor.SelfId())
    , StartTs(HPNow())
{}

TDDiskActor::TDirectIoOpBase::~TDirectIoOpBase() = default;

void TDDiskActor::TDirectIoOpBase::OnComplete(NActors::TActorSystem* actorSystem) noexcept
{
    TCompletionGuard guard(this, actorSystem);

    const auto opType = GetOperationType();
    const i64 result = GetResult();
    const double requestTimeMs = TimePassed();
    AccountShortIo();

    // Integrity overload retries retain all buffers. A read-parts operation
    // rearms only failed metadata parts and keeps its completed data untouched.
    // Defer logical Done() until retries finish or fail terminally.
    if (Y_UNLIKELY(PrepareRetry())) {
        ++RetryCount;
        auto ev = std::make_unique<TDDiskActor::TEvPrivate::TEvRetryIO>(guard.Release());
        actorSystem->Send(new IEventHandle(DDiskId, {}, ev.release()));
        return;
    }

    switch (opType) {
    case TUringOperationBase::EREAD:
        Actor.Counters.DirectIO.Read.Done(GetAccountingSize(), requestTimeMs);
        break;
    case TUringOperationBase::EWRITE:
        Actor.Counters.DirectIO.Write.Done(GetAccountingSize(), requestTimeMs);
        break;
    default:
        Y_ABORT("Unknown OperationType");
    }

    if (Y_UNLIKELY(result < 0)) {
        const char* opName = opType == TUringOperationBase::EREAD ? "read" : "write";
        const auto bufAddr = reinterpret_cast<uintptr_t>(GetIovBase());
        TString reason = TStringBuilder()
            << "io_uring " << opName << " error:"
            << " errno=" << (-result) << " (" << strerror(-result) << ")"
            << " diskOffset=" << GetDiskOffset()
            << " totalSize=" << GetTotalSize()
            << " iovLen=" << GetOperationBytes()
            << " bufAddr=0x" << Hex(bufAddr)
            << " bufAligned4k=" << (int)(bufAddr % MinBlockSize == 0)
            << " offsetAligned4k=" << (int)(GetDiskOffset() % MinBlockSize == 0)
            << " sizeAligned4k=" << (int)(GetOperationBytes() % MinBlockSize == 0)
            << " chunkIdx=" << ChunkIdx
            << " chunkOffset=" << ChunkOffsetInBytes
            << " DDiskId=" << DDiskId;
        YDB_LOG_ERROR_CTX(*actorSystem, reason);
        const bool exhausted = IsCriticalDDiskIo()
            && UringErrorToStatus(result, opType) == TReplyStatus::OVERLOADED;
        if (exhausted) {
            reason += TStringBuilder() << " retry exhausted: attempts=" << (RetryCount + 1);
        }
        Reply(actorSystem, exhausted ? TReplyStatus::ERROR : UringErrorToStatus(result, opType), std::move(reason));
        return;
    }

    // Both the router and the PDisk fallback complete the whole logical request.
    Y_ABORT_UNLESS(static_cast<ui64>(result) == GetTotalSize());
    Reply(actorSystem, TReplyStatus::OK);
}

bool TDDiskActor::TDirectIoOpBase::PrepareRetry() noexcept {
    return GetResult() < 0 && IsCriticalDDiskIo()
        && UringErrorToStatus(GetResult(), GetOperationType()) == TReplyStatus::OVERLOADED
        && RetryCount < MaxResubmissions;
}

void TDDiskActor::TDirectIoOpBase::OnDrop(NActors::TActorSystem* actorSystem) noexcept {
    TCompletionGuard guard(this, actorSystem);
    AccountShortIo();

    switch (GetOperationType()) {
    case TUringOperationBase::EREAD:
        Actor.Counters.DirectIO.Read.Done(GetAccountingSize());
        break;
    case TUringOperationBase::EWRITE:
        Actor.Counters.DirectIO.Write.Done(GetAccountingSize());
        break;
    default:
        Y_ABORT("Unknown OperationType");
    }

    Reply(actorSystem, TReplyStatus::SESSION_MISMATCH, "io_uring request dropped");
}

void TDDiskActor::TDirectIoOpBase::AccountShortIo() noexcept {
    const ui64 count = TakeShortIoCount();
    if (!count) {
        return;
    }
    switch (GetOperationType()) {
    case TUringOperationBase::EREAD:
        *Actor.Counters.DirectIO.ShortReads += count;
        break;
    case TUringOperationBase::EWRITE:
        *Actor.Counters.DirectIO.ShortWrites += count;
        break;
    default:
        Y_ABORT("Unknown OperationType");
    }
}

void TDDiskActor::TDirectIoOpBase::PrepareWrite(TRope&& data, ui64 offset, TChunkIdx chunkIdx, ui32 chunkOffset) {
    Y_ABORT_UNLESS(data.size() <= MaxRwCount);
    const size_t dataSize = data.size();
    Data.reset();
    AlignedDataHolder = {};

    SetOperationType(EWRITE);

#if defined(__linux__)
    // Zero-copy scatter-gather path: taken when all rope chunks are page-aligned
    // (base address) and sector-aligned (length), and fit within MAX_IOVS. The
    // rope is moved into Data so its chunk backends (reference-counted heap
    // buffers) outlive the I/O; each chunk becomes one iovec - no memcpy.
    {
        size_t chunkCount = 0;
        bool allAligned = true;
        for (auto it = data.Begin(); it.Valid(); it.AdvanceToNextContiguousBlock()) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(it.ContiguousData());
            if ((base & (MinBlockSize - 1)) != 0 || (it.ContiguousSize() & (MinBlockSize - 1)) != 0) {
                allAligned = false;
                break;
            }
            ++chunkCount;
            if (chunkCount > NPDisk::TUringOperationBase::MAX_IOVS) {
                allAligned = false;
                break;
            }
        }

        if (allAligned && chunkCount > 0) {
            Data = std::move(data);

            PrepareScatterGather(chunkCount, offset);
            for (auto it = Data->Begin(); it.Valid(); it.AdvanceToNextContiguousBlock()) {
                // writev only reads from the buffer, so const_cast is safe here.
                AddIov(const_cast<char*>(it.ContiguousData()), it.ContiguousSize());
            }

            ChunkIdx = chunkIdx;
            ChunkOffsetInBytes = chunkOffset;
            return;
        }
    }
#endif

    // Copy path: unaligned chunks, too many chunks, or non-Linux.
    AlignedDataHolder = TRcBuf::UninitializedPageAligned(dataSize);
    data.Begin().ExtractPlainDataAndAdvance(AlignedDataHolder.GetDataMut(), dataSize);

    // UnsafeGetDataMut: writev only reads from the buffer, so we avoid COW
    // that TRcBuf::GetDataMut() would trigger on shared page-aligned buffers.
    PrepareIov(AlignedDataHolder.UnsafeGetDataMut(), dataSize, offset);

    ChunkIdx = chunkIdx;
    ChunkOffsetInBytes = chunkOffset;
}

void TDDiskActor::TDirectIoOpBase::PrepareRead(size_t size, ui64 offset, TChunkIdx chunkIdx, ui32 chunkOffset) {
    Y_ABORT_UNLESS(size <= MaxRwCount);
    Data.reset();

    AlignedDataHolder = TRcBuf::UninitializedPageAligned(size);
    SetOperationType(EREAD);
    PrepareIov(AlignedDataHolder.GetDataMut(), size, offset);

    ChunkIdx = chunkIdx;
    ChunkOffsetInBytes = chunkOffset;
}

TRope TDDiskActor::TDirectIoOpBase::ExtractData() {
    if (Data) {
        return std::move(*Data);
    }

    return TRope(std::move(AlignedDataHolder));
}



double TDDiskActor::TDirectIoOpBase::TimePassed() const {
    return HPMilliSecondsFloat(HPNow() - StartTs);
}

void TDDiskActor::TDirectIoOpBase::SetResult(i64 result, TRope&& data) {
    SetResult(result);
    Data = std::move(data);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TDDiskIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TDDiskIoOp::Reply(NActors::TActorSystem* actorSystem, TReplyStatus::E status,
        TString reason) noexcept {
    const double requestTimeMs = TimePassed();
    TRope data;

    switch (GetOperationType()) {
    case TUringOperationBase::EREAD: {
        if (status == TReplyStatus::OK) {
            data = ExtractData();
        }
        break;
    }
    case TUringOperationBase::EWRITE:
        break;
    default:
        Y_ABORT("Unknown OperationType");
    }

    actorSystem->Send(DDiskId, new TEvPrivate::TEvDDiskIoResult(
        GetOperationType(), status, std::move(reason), std::move(data),
        GetOriginalRequester(), GetInterconnectSession(), GetCookie(), ExtractSpan(),
        GetTotalSize(), requestTimeMs, TabletId, VChunkIndex, HasChunkKey,
        {}), 0, GetCompletionCookie());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TReadPartsIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TReadPartsIoOp::PrepareParts(TConstArrayRef<TPart> parts) {
    Y_ABORT_UNLESS(!parts.empty());
    Parts.clear();
    Parts.reserve(parts.size());
    ActiveParts.clear();
    ActiveParts.reserve(parts.size());
    AccountingSize = 0;
    for (const auto& part : parts) {
        Y_ABORT_UNLESS(part.Size && part.Size <= MaxRwCount);
        TOwnedPart owned;
        owned.Part = part;
        owned.Buffer = TRcBuf::UninitializedPageAligned(part.Size);
        ActiveParts.push_back(Parts.size());
        Parts.push_back(std::move(owned));
        AccountingSize += part.Size;
    }
    PrepareActiveParts();
}

void TDDiskActor::TReadPartsIoOp::PrepareActiveParts() {
    std::vector<NPDisk::TUringOperationBase::TReadPart> descriptors;
    descriptors.reserve(ActiveParts.size());
    for (const size_t index : ActiveParts) {
        auto& part = Parts[index];
        part.Completed = false;
        part.FallbackData.reset();
        descriptors.push_back({part.Part.DiskOffset, part.Part.Size, part.Buffer.GetDataMut()});
    }
    PrepareReadParts(descriptors);
    SetOperationType(EREAD);
}

const TDDiskActor::TReadPartsIoOp::TPart& TDDiskActor::TReadPartsIoOp::GetFallbackPart(
        size_t activeIndex) const {
    return Parts.at(ActiveParts.at(activeIndex)).Part;
}

void TDDiskActor::TReadPartsIoOp::SetFallbackPartResult(size_t activeIndex, i64 result, TRope&& data) {
    auto& part = Parts.at(ActiveParts.at(activeIndex));
    if (result >= 0 && (static_cast<ui64>(result) != part.Part.Size || data.size() != part.Part.Size)) {
        result = -EIO;
    }
    if (result >= 0) {
        // Keep the PDisk-owned rope instead of copying it into the direct-I/O buffer.
        part.FallbackData.emplace(std::move(data));
    }
    SetReadPartResult(activeIndex, result);
}

void TDDiskActor::TReadPartsIoOp::FinishFallbackReadParts() {
    i64 aggregate = GetTotalSize();
    for (size_t index = 0; index < ActiveParts.size(); ++index) {
        const i64 result = GetReadPartResult(index);
        if (result < 0) {
            aggregate = result;
            break;
        }
        Y_ABORT_UNLESS(static_cast<ui64>(result) == GetFallbackPart(index).Size);
    }
    SetResult(aggregate);
}

bool TDDiskActor::TReadPartsIoOp::PrepareRetry() noexcept {
    std::vector<size_t> retry;
    for (size_t index = 0; index < ActiveParts.size(); ++index) {
        auto& part = Parts[ActiveParts[index]];
        part.Result = GetReadPartResult(index);
        part.Completed = true;
        Y_ABORT_UNLESS(part.Result < 0 || static_cast<ui64>(part.Result) == part.Part.Size);
        if (part.Part.Id && part.Result < 0
                && UringErrorToStatus(part.Result, EREAD) == TReplyStatus::OVERLOADED
                && RetryCount < MaxResubmissions) {
            retry.push_back(ActiveParts[index]);
        }
    }
    if (retry.empty()) {
        return false;
    }
    ActiveParts = std::move(retry);
    PrepareActiveParts();
    return true;
}

void TDDiskActor::TReadPartsIoOp::Reply(NActors::TActorSystem* actorSystem, TReplyStatus::E status,
        TString reason) noexcept {
    std::vector<TEvPrivate::TEvReadPartsResult::TPartResult> results;
    results.reserve(Parts.size());
    for (auto& part : Parts) {
        TEvPrivate::TEvReadPartsResult::TPartResult result;
        result.Id = part.Part.Id;
        result.Status = status;
        result.ErrorMessage = reason;
        if (part.Completed) {
            if (part.Result >= 0) {
                result.Status = TReplyStatus::OK;
                result.ErrorMessage.clear();
                result.Data = part.FallbackData
                    ? std::move(*part.FallbackData)
                    : TRope(std::move(part.Buffer));
            } else {
                result.Status = UringErrorToStatus(part.Result, EREAD);
                const bool exhausted = part.Part.Id && result.Status == TReplyStatus::OVERLOADED;
                if (exhausted) {
                    result.Status = TReplyStatus::ERROR;
                }
                result.ErrorMessage = TStringBuilder()
                    << (part.Part.Id ? "metadata" : "data") << " read failed: errno=" << -part.Result
                    << " (" << strerror(-part.Result) << ")"
                    << " chunkIdx=" << part.Part.ChunkIdx << " chunkOffset=" << part.Part.OffsetInBytes;
                if (exhausted) {
                    result.ErrorMessage += TStringBuilder() << " retry exhausted: attempts=" << (RetryCount + 1);
                }
            }
        }
        results.push_back(std::move(result));
    }
    actorSystem->Send(DDiskId, new TEvPrivate::TEvReadPartsResult(std::move(results)),
        0, GetCompletionCookie());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TPersistentBufferPartIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TPersistentBufferPartIoOp::Reply(NActors::TActorSystem* actorSystem, TReplyStatus::E status,
        TString reason) noexcept {
    std::unique_ptr<IEventBase> reply;
    const auto opType = GetOperationType();
    const i64 result = GetResult();
    if (status == TReplyStatus::OVERLOADED) {
        if (!reason) {
            reason = "io_uring request temporarily overloaded (I/O error retry)";
        }
    } else if (status != TReplyStatus::OK) {
        if (!reason) {
            if (result < 0) {
                const char* opName = opType == TUringOperationBase::EREAD
                    ? "read"
                    : (opType == TUringOperationBase::EWRITE ? "write" : "unknown");
                reason = TStringBuilder()
                    << opName
                    << " failed: " << strerror(-result)
                    << " (errno " << (-result) << ")";
            } else {
                reason = "I/O failed";
            }
        }
    }

    switch (opType) {
        case TUringOperationBase::EREAD: {
            TRope data = status == TReplyStatus::OK ? ExtractData() : TRope();
            reply = std::make_unique<TEvPrivate::TEvReadPersistentBufferPart>(
                GetCookie(), PartCookie, status, std::move(reason), std::move(data), IsRestore);
            break;
        }
        case TUringOperationBase::EWRITE:
            reply = std::make_unique<TEvPrivate::TEvWritePersistentBufferPart>(
                GetCookie(), PartCookie, status, reason, IsErase);
            break;
        default:
            Y_ABORT("Unknown OperationType");
    }

    actorSystem->Send(DDiskId, reply.release());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TDirectIoOpBase — pool support
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TDirectIoOpBase::Reinit(const IEventHandle* ev) {
    CompletionCookie = 0;
    ResetSubmissionState();
    StartTs = HPNow();
    if (ev) {
        OriginalRequester = ev->Sender;
        InterconnectSession = ev->InterconnectSession;
        Cookie = ev->Cookie;
    } else {
        OriginalRequester = {};
        InterconnectSession = {};
        Cookie = 0;
    }
    ChunkIdx = 0;
    ChunkOffsetInBytes = 0;
    RetryCount = 0;
}

void TDDiskActor::TDirectIoOpBase::ClearForRecycle() noexcept {
    AlignedDataHolder = {};
    Data.reset();
    Span = {};
    RetryCount = 0;
}

void TDDiskActor::TDDiskIoOp::SelfRecycle() noexcept {
    Actor.ReturnOp(this);
}

void TDDiskActor::TDDiskIoOp::ClearForRecycle() noexcept {
    TabletId = 0;
    VChunkIndex = 0;
    HasChunkKey = false;
    TDirectIoOpBase::ClearForRecycle();
}

void TDDiskActor::TPersistentBufferPartIoOp::ClearForRecycle() noexcept {
    PartCookie = 0;
    IsErase = false;
    IsRestore = false;
    TDirectIoOpBase::ClearForRecycle();
}

void TDDiskActor::TPersistentBufferPartIoOp::SelfRecycle() noexcept {
    Actor.ReturnOp(this);
}

void TDDiskActor::TInternalSyncWriteOp::ClearForRecycle() noexcept {
    TDirectIoOpBase::ClearForRecycle();
}

void TDDiskActor::TInternalSyncWriteOp::SelfRecycle() noexcept {
    Actor.ReturnOp(this);
}

void TDDiskActor::TIntegrityIoOp::ClearForRecycle() noexcept {
    TDirectIoOpBase::ClearForRecycle();
}

void TDDiskActor::TIntegrityIoOp::SelfRecycle() noexcept {
    Actor.ReturnOp(this);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TInternalSyncWriteOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TInternalSyncWriteOp::Reply(NActors::TActorSystem* actorSystem, TReplyStatus::E status,
        TString reason) noexcept {
    const i64 result = GetResult();

    if (status == TReplyStatus::OVERLOADED) {
        if (!reason) {
            reason = "io_uring request temporarily overloaded (I/O error retry)";
        }
    } else if (status != TReplyStatus::OK) {
        if (!reason) {
            if (result < 0) {
                reason = TStringBuilder()
                    << "write failed: " << strerror(-result)
                    << " (errno " << (-result) << ")";
            } else {
                reason = "write failed";
            }
        }
    }

    actorSystem->Send(
        DDiskId,
        new TEvPrivate::TEvInternalSyncWriteResult(
            status,
            std::move(reason)), 0, GetCompletionCookie());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TIntegrityIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TIntegrityIoOp::Reply(NActors::TActorSystem* actorSystem, TReplyStatus::E status,
        TString reason) noexcept {
    const i64 result = GetResult();
    TRope data;

    if (status != TReplyStatus::OK && !reason) {
        if (result < 0) {
            reason = TStringBuilder()
                << "integrity I/O failed: " << strerror(-result)
                << " (errno " << (-result) << ")";
        } else {
            reason = "integrity I/O failed";
        }
    } else if (status == TReplyStatus::OK && GetOperationType() == TUringOperationBase::EREAD) {
        data = ExtractData();
    }

    actorSystem->Send(DDiskId, new TEvPrivate::TEvIntegrityIoResult(
        status, std::move(reason), std::move(data)), 0, GetCompletionCookie());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor::TChunkFormatIoOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TDDiskActor::TChunkFormatIoOp::Reply(NActors::TActorSystem* actorSystem, TReplyStatus::E status,
        TString reason) noexcept {
    if (status != TReplyStatus::OK && !reason) {
        const i64 result = GetResult();
        if (result < 0) {
            reason = TStringBuilder() << "chunk zero-format write failed: " << strerror(-result)
                << " (errno " << (-result) << ")";
        } else {
            reason = "chunk zero-format write failed";
        }
    }
    actorSystem->Send(DDiskId, new TEvPrivate::TEvChunkFormatIoResult(
        ChunkIdx, OffsetInBytes, Size, status, std::move(reason)), 0, ChunkIdx);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TDDiskActor — pool AllocateOp / ReturnOp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

template <typename T>
std::unique_ptr<T> TDDiskActor::AllocateOp(const IEventHandle* ev) {
    auto& pool = [] (TDDiskActor& self) -> TSpscCircularQueue<std::unique_ptr<T>>& {
        if constexpr (std::is_same_v<T, TDDiskIoOp>) {
            return self.DdiskIoOpPool;
        } else if constexpr (std::is_same_v<T, TPersistentBufferPartIoOp>) {
            return self.PersistentBufferPartIoOpPool;
        } else if constexpr (std::is_same_v<T, TIntegrityIoOp>) {
            return self.IntegrityIoOpPool;
        } else {
            static_assert(std::is_same_v<T, TInternalSyncWriteOp>);
            return self.InternalSyncWriteOpPool;
        }
    }(*this);

    std::unique_ptr<T> op;
    if (!pool.TryPop(op)) {
        op = std::make_unique<T>(*this);
    }
    op->Reinit(ev);
    return op;
}

template std::unique_ptr<TDDiskActor::TDDiskIoOp>
TDDiskActor::AllocateOp<TDDiskActor::TDDiskIoOp>(const IEventHandle*);

template std::unique_ptr<TDDiskActor::TPersistentBufferPartIoOp>
TDDiskActor::AllocateOp<TDDiskActor::TPersistentBufferPartIoOp>(const IEventHandle*);

template std::unique_ptr<TDDiskActor::TInternalSyncWriteOp>
TDDiskActor::AllocateOp<TDDiskActor::TInternalSyncWriteOp>(const IEventHandle*);

template std::unique_ptr<TDDiskActor::TIntegrityIoOp>
TDDiskActor::AllocateOp<TDDiskActor::TIntegrityIoOp>(const IEventHandle*);

void TDDiskActor::ReturnOp(TDDiskIoOp* op) {
    op->ClearForRecycle();
    if (!DdiskIoOpPool.TryPush(std::unique_ptr<TDDiskIoOp>(op))) {
        // unique_ptr destructor deletes anyway
    }
}

void TDDiskActor::ReturnOp(TPersistentBufferPartIoOp* op) {
    op->ClearForRecycle();
    if (!PersistentBufferPartIoOpPool.TryPush(std::unique_ptr<TPersistentBufferPartIoOp>(op))) {
        // unique_ptr destructor deletes anyway
    }
}

void TDDiskActor::ReturnOp(TInternalSyncWriteOp* op) {
    op->ClearForRecycle();
    if (!InternalSyncWriteOpPool.TryPush(std::unique_ptr<TInternalSyncWriteOp>(op))) {
        // unique_ptr destructor deletes anyway
    }
}

void TDDiskActor::ReturnOp(TIntegrityIoOp* op) {
    op->ClearForRecycle();
    if (!IntegrityIoOpPool.TryPush(std::unique_ptr<TIntegrityIoOp>(op))) {
        // unique_ptr destructor deletes anyway
    }
}

template <typename T>
void TDDiskActor::FillPool(TSpscCircularQueue<std::unique_ptr<T>>& pool) {
    for (ui32 i = 0; i < IoOpPoolCapacity; ++i) {
        pool.TryPush(std::make_unique<T>(*this));
    }
}

template void TDDiskActor::FillPool<TDDiskActor::TDDiskIoOp>(TSpscCircularQueue<std::unique_ptr<TDDiskActor::TDDiskIoOp>>&);
template void TDDiskActor::FillPool<TDDiskActor::TPersistentBufferPartIoOp>(TSpscCircularQueue<std::unique_ptr<TDDiskActor::TPersistentBufferPartIoOp>>&);
template void TDDiskActor::FillPool<TDDiskActor::TInternalSyncWriteOp>(TSpscCircularQueue<std::unique_ptr<TDDiskActor::TInternalSyncWriteOp>>&);
template void TDDiskActor::FillPool<TDDiskActor::TIntegrityIoOp>(TSpscCircularQueue<std::unique_ptr<TDDiskActor::TIntegrityIoOp>>&);

} // NKikimr::NDDisk
