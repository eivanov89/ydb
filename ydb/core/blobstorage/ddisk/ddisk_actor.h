#pragma once

#include "defs.h"
#include <list>

#include "ddisk.h"
#include "chunk_manager.h"
#include "integrity_manager.h"
#include "persistent_buffer.h"
#include "persistent_buffer_header.h"
#include "persistent_buffer_barriers_manager.h"
#include "persistent_buffer_space_allocator.h"
#include "segment_manager.h"
#include "span_utils.h"

#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_data.h>
#include <ydb/core/blobstorage/vdisk/common/vdisk_config.h>
#include <ydb/core/util/hp_timer_helpers.h>
#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk.h>

#include <ydb/library/actors/core/mon.h>
#include <ydb/library/actors/async/event.h>
#include <ydb/library/actors/async/frame_cache.h>
#include <ydb/library/actors/async/continuation.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <ydb/library/actors/wilson/wilson_span.h>
#include <ydb/library/wilson_ids/wilson.h>

#if defined(__linux__)
#include <ydb/library/pdisk_io/uring_router_client.h>
#endif

#include <ydb/library/pdisk_io/uring_operation.h>

#include <ydb/core/util/spsc_circular_queue.h>

#include <array>
#include <atomic>
#include <deque>
#include <optional>
#include <queue>

#include <util/generic/hash_set.h>

#include <library/cpp/containers/absl/flat_hash_map.h>
#include <library/cpp/containers/absl/flat_hash_set.h>

namespace NKikimrBlobStorage::NDDisk::NInternal {
    class TChunkMapLogRecord;
    class TPersistentBufferChunkMapLogRecord;
}

#define LIST_COUNTERS_INTERFACE_OPS(XX) \
    XX(Write) \
    XX(Read) \
    XX(Sync) \
    XX(WritePersistentBuffer) \
    XX(ReadPersistentBuffer) \
    XX(ErasePersistentBuffer) \
    XX(ListPersistentBuffer) \
    XX(GetPersistentBufferRegistrationToken) \
    /**/

namespace NKikimr::NDDisk {

    namespace NPrivate {
        template<typename TRecord>
        struct THasSelectorField {
            template<typename T> static constexpr auto check(T*) -> typename std::is_same<
                std::decay_t<decltype(std::declval<T>().GetSelector())>,
                NKikimrBlobStorage::NDDisk::TBlockSelector
            >::type;

            template<typename> static constexpr std::false_type check(...);

            static constexpr bool value = decltype(check<TRecord>(nullptr))::value;
        };

        template<typename TRecord>
        struct THasWriteInstructionField {
            template<typename T> static constexpr auto check(T*) -> typename std::is_same<
                std::decay_t<decltype(std::declval<T>().GetInstruction())>,
                NKikimrBlobStorage::NDDisk::TWriteInstruction
            >::type;

            template<typename> static constexpr std::false_type check(...);

            static constexpr bool value = decltype(check<TRecord>(nullptr))::value;
        };
    }

    class TDDiskActor : public TActorBootstrapped<TDDiskActor> {
        // Declared first so frame-owning members, if added, are destroyed first.
        NActors::TAsyncFrameCache AsyncFrameCache;

        NActors::TAsyncFrameCache* GetAsyncFrameCache() noexcept override {
            return &AsyncFrameCache;
        }

        TString DDiskId;
        TVDiskConfig::TBaseInfo BaseInfo;
        TDDiskConfig Config;
        TIntrusivePtr<TBlobStorageGroupInfo> Info;
        TIntrusivePtr<NMonitoring::TDynamicCounters> CountersParent;
        TIntrusivePtr<NMonitoring::TDynamicCounters> CountersBase;
        std::vector<std::pair<TString, TString>> CountersChain;
        ui64 DDiskInstanceGuid = RandomNumber<ui64>();

        class TDirectIoOpBase;
        class TDDiskIoOp;
        class TPersistentBufferPartIoOp;
        class TInternalSyncWriteOp;
        class TIntegrityIoOp;
        class TReadPartsIoOp;
        class TChunkFormatIoOp;

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // I/O operation pools
        //
        // SPSC contract: the queues have a single producer and a single consumer.
        //   Consumer (TryPop)  — always the actor thread (AllocateOp).
        //   Producer (TryPush) — the io_uring I/O thread (OnComplete/OnDrop → SelfRecycle → ReturnOp)
        //                        when UringRouter is active, or the actor thread itself on the PDisk fallback
        //                        path. These two paths are mutually exclusive: either UringRouter is set for
        //                        the whole lifetime (uring path) or it is not (PDisk fallback), so only one
        //                        thread ever pushes.
        //   FillPool (TryPush) runs once during Bootstrap before any I/O is in flight.
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        static constexpr ui32 IoOpPoolCapacity = 128;

        TSpscCircularQueue<std::unique_ptr<TDDiskIoOp>> DdiskIoOpPool;
        TSpscCircularQueue<std::unique_ptr<TPersistentBufferPartIoOp>> PersistentBufferPartIoOpPool;
        TSpscCircularQueue<std::unique_ptr<TInternalSyncWriteOp>> InternalSyncWriteOpPool;
        TSpscCircularQueue<std::unique_ptr<TIntegrityIoOp>> IntegrityIoOpPool;

        template <typename T>
        std::unique_ptr<T> AllocateOp(const IEventHandle* ev = nullptr);

        void ReturnOp(TDDiskIoOp* op);
        void ReturnOp(TPersistentBufferPartIoOp* op);
        void ReturnOp(TInternalSyncWriteOp* op);
        void ReturnOp(TIntegrityIoOp* op);

        template <typename T>
        void FillPool(TSpscCircularQueue<std::unique_ptr<T>>& pool);

        void InitUring();

        NPDisk::TDiskFormatPtr DiskFormat{nullptr, nullptr};

    private:
        struct TOpCountersBase {
            NMonitoring::TDynamicCounters::TCounterPtr Requests;
            NMonitoring::TDynamicCounters::TCounterPtr RequestsInFlight;
            NMonitoring::TDynamicCounters::TCounterPtr Bytes;
            NMonitoring::TDynamicCounters::TCounterPtr BytesInFlight;
            NMonitoring::THistogramPtr RequestSizeKiB;
            NMonitoring::THistogramPtr ResponseTime;

            void Request(ui32 bytes = 0) {
                ++*Requests;
                ++*RequestsInFlight;
                if (bytes) {
                    *Bytes += bytes;
                    *BytesInFlight += bytes;
                    RequestSizeKiB->Collect(bytes >> 10);
                }
            }

            void Done(ui32 bytes, double durationMs = 0) {
                --*RequestsInFlight;
                *BytesInFlight -= bytes;
                if (durationMs != 0) {
                    ResponseTime->Collect(durationMs);
                }
            }
        };

        struct TInterfaceOpCounters : public TOpCountersBase {
            NMonitoring::TDynamicCounters::TCounterPtr ReplyOk;
            NMonitoring::TDynamicCounters::TCounterPtr ReplyErr;

            void Reply(bool ok, ui32 bytes = 0, double durationMs = 0) {
                ++*(ok ? ReplyOk : ReplyErr);
                Done(bytes, durationMs);
            }
        };

        struct TCounters {
            struct {
#define DECLARE_COUNTERS_INTERFACE(NAME) \
                TInterfaceOpCounters NAME;

                LIST_COUNTERS_INTERFACE_OPS(DECLARE_COUNTERS_INTERFACE)

#undef DECLARE_COUNTERS_INTERFACE
                NMonitoring::TDynamicCounters::TCounterPtr UnalignedWritePayloads;
            } Interface;

            struct {
                NMonitoring::TDynamicCounters::TCounterPtr ReadLogChunks;
                NMonitoring::TDynamicCounters::TCounterPtr LogRecordsProcessed;
                NMonitoring::TDynamicCounters::TCounterPtr LogRecordsApplied;
                NMonitoring::TDynamicCounters::TCounterPtr LogRecordsWritten;
                NMonitoring::TDynamicCounters::TCounterPtr NumChunkMapSnapshots;
                NMonitoring::TDynamicCounters::TCounterPtr NumChunkMapIncrements;
                NMonitoring::TDynamicCounters::TCounterPtr CutLogMessages;
            } RecoveryLog;

            struct {
                NMonitoring::TDynamicCounters::TCounterPtr ChunksOwned;
            } Chunks;

            struct {
                TOpCountersBase Write;
                TOpCountersBase Read;

                NMonitoring::TDynamicCounters::TCounterPtr ShortReads;
                NMonitoring::TDynamicCounters::TCounterPtr ShortWrites;

                NMonitoring::TDynamicCounters::TCounterPtr RunningCount;
            } DirectIO;

            struct {
                NMonitoring::TDynamicCounters::TCounterPtr AllocatedChunks;
                NMonitoring::TDynamicCounters::TCounterPtr TotalBytes;
                NMonitoring::TDynamicCounters::TCounterPtr PendingEventsQueueSize;
                NMonitoring::TDynamicCounters::TCounterPtr InMemoryCacheSize;
                NMonitoring::THistogramPtr WriteBatchSize;
            } PersistentBuffer;

            struct {
                // Writes rejected because no checksum list was attached.
                NMonitoring::TDynamicCounters::TCounterPtr WritesWithoutChecksums;
                // Payload checksum mismatches (write-time sender list, sync source payload, or
                // disk-read vs stored checksums) reported as TReplyStatus::CORRUPTED.
                NMonitoring::TDynamicCounters::TCounterPtr ChecksumMismatch;
                NMonitoring::TDynamicCounters::TCounterPtr IntegrityPairReads;
                // Pair-slot writes only; chunk-header and extent-format writes are excluded.
                NMonitoring::TDynamicCounters::TCounterPtr IntegrityPairWrites;
                NMonitoring::TDynamicCounters::TCounterPtr IntegrityCorruption;
                NMonitoring::TDynamicCounters::TCounterPtr IntegrityLostWriteDetected;
            } Checksums;
        };

        TCounters Counters;

        // Separate from the shared monitoring counters: only this actor's router
        // callbacks contribute, until their last access to actor-owned state.
    private:
        friend class TDDiskActorTestPeer;
        std::function<TMonotonic()> DestructionNow;
        std::function<void()> DestructionSleep;
    public:
        static constexpr ui64 DirectIoStopping = ui64{1} << 63;
        std::atomic<ui64> DirectIoState{0};
        ui64 GetDirectIoInflight() const {
            return DirectIoState.load(std::memory_order_acquire) & ~DirectIoStopping;
        }
        void OnDirectIODone(NActors::TActorSystem* actorSystem);
        NMonitoring::TDynamicCounters::TCounterPtr IoStalledCounter;

#if defined(__linux__)
        std::shared_ptr<NPDisk::IUringRouterClient> UringRouter;
#endif

    public:
        struct TEvPrivate {
            enum {
                EvHandleSingleQuery = EventSpaceBegin(TEvents::ES_PRIVATE),
                EvHandlePersistentBufferEventForChunk,
                EvRetryIO,
                EvWritePersistentBufferPart,
                EvReadPersistentBufferPart,
                EvInternalSyncWriteResult,
                EvIssuePersistentBufferChunkAllocation,
                EvDeallocatePersistentBufferChunk,
                EvDeallocatePersistentBufferChunkResult,
                EvRetryListPersistentBuffer,
                EvDDiskIoResult,
                EvIntegrityIoResult,
                EvChunkFormatIoResult,
                EvFinishStopping,
                EvStopIoTimeout,
                EvBeginStopping,
                EvCompleteStop,
                EvRetryIODelayed,
                EvProcessPersistentBufferRemoval,
                EvExpirePersistentBufferRegistrationToken,
                EvReadPartsResult,
            };

            struct TEvExpirePersistentBufferRegistrationToken
                : TEventLocal<TEvExpirePersistentBufferRegistrationToken, EvExpirePersistentBufferRegistrationToken> {
            };

            struct TEvProcessPersistentBufferRemoval : TEventLocal<TEvProcessPersistentBufferRemoval, EvProcessPersistentBufferRemoval> {
                TPersistentBufferTabletKey Key;
                explicit TEvProcessPersistentBufferRemoval(TPersistentBufferTabletKey key)
                    : Key(key)
                {}
            };

            struct TEvCompleteStop : TEventLocal<TEvCompleteStop, EvCompleteStop> {};
            struct TEvBeginStopping : TEventLocal<TEvBeginStopping, EvBeginStopping> {};
            struct TEvRetryIODelayed : TEventLocal<TEvRetryIODelayed, EvRetryIODelayed> {
                ui64 Id;
                explicit TEvRetryIODelayed(ui64 id) : Id(id) {}
            };
            struct TEvFinishStopping : TEventLocal<TEvFinishStopping, EvFinishStopping> {};
            struct TEvStopIoTimeout : TEventLocal<TEvStopIoTimeout, EvStopIoTimeout> {};

           struct TEvRetryListPersistentBuffer : TEventLocal<TEvRetryListPersistentBuffer, EvRetryListPersistentBuffer> {
                TAutoPtr<TEventHandle<TEvListPersistentBuffer>> Ev;
                ui32 RetriesLeft;

                TEvRetryListPersistentBuffer(TAutoPtr<TEventHandle<TEvListPersistentBuffer>> ev, ui32 retriesLeft)
                    : Ev(ev)
                    , RetriesLeft(retriesLeft)
                {}
            };

            struct TEvIssuePersistentBufferChunkAllocation : TEventLocal<TEvIssuePersistentBufferChunkAllocation, EvIssuePersistentBufferChunkAllocation> {
            };

            struct TEvDeallocatePersistentBufferChunk : TEventLocal<TEvDeallocatePersistentBufferChunk, EvDeallocatePersistentBufferChunk> {
                ui32 ChunkIdx;

                TEvDeallocatePersistentBufferChunk(ui32 chunkIdx)
                    : ChunkIdx(chunkIdx)
                {}
            };

            struct TEvDeallocatePersistentBufferChunkResult : TEventLocal<TEvDeallocatePersistentBufferChunkResult, EvDeallocatePersistentBufferChunkResult> {
                ui32 ChunkIdx;

                TEvDeallocatePersistentBufferChunkResult(ui32 chunkIdx)
                    : ChunkIdx(chunkIdx)
                {}
            };


            struct TEvHandlePersistentBufferEventForChunk : TEventLocal<TEvHandlePersistentBufferEventForChunk, EvHandlePersistentBufferEventForChunk> {
                ui32 ChunkIndex;

                TEvHandlePersistentBufferEventForChunk(ui32 chunkIndex)
                    : ChunkIndex(chunkIndex)
                {}
            };

            struct TEvReadPersistentBufferPart : TEventLocal<TEvReadPersistentBufferPart, EvReadPersistentBufferPart> {
                ui64 InflightCookie;
                ui64 PartCookie;
                NKikimrBlobStorage::NDDisk::TReplyStatus::E Status;
                TString ErrorMessage;
                TRope Data;
                bool IsRestore = false;

                TEvReadPersistentBufferPart(ui64 inflightCookie, ui64 partCookie,
                    NKikimrBlobStorage::NDDisk::TReplyStatus::E status, TString errorMessage, TRope data, bool isRestore)
                    : InflightCookie(inflightCookie)
                    , PartCookie(partCookie)
                    , Status(status)
                    , ErrorMessage(std::move(errorMessage))
                    , Data(std::move(data))
                    , IsRestore(isRestore)
                {}
            };

            struct TEvWritePersistentBufferPart : TEventLocal<TEvWritePersistentBufferPart, EvWritePersistentBufferPart> {
                ui64 InflightCookie;
                ui64 PartCookie;
                NKikimrBlobStorage::NDDisk::TReplyStatus::E Status;
                TString ErrorMessage;
                bool IsErase = false;

                TEvWritePersistentBufferPart(ui64 inflightCookie, ui64 partCookie,
                    NKikimrBlobStorage::NDDisk::TReplyStatus::E status, TString errorMessage, bool isErase = false)
                    : InflightCookie(inflightCookie)
                    , PartCookie(partCookie)
                    , Status(status)
                    , ErrorMessage(errorMessage)
                    , IsErase(isErase)
                {}
            };

            struct TEvRetryIO : TEventLocal<TEvRetryIO, EvRetryIO> {
                std::unique_ptr<TDirectIoOpBase> Op;

                explicit TEvRetryIO(std::unique_ptr<TDirectIoOpBase> op);
                ~TEvRetryIO();
            };

            // I/O callback for a client DDisk read/write. The callback only
            // packages status/data and routing metadata; the actor serializes it with
            // integrity failures, decides the final reply status, and sends the client response.
            struct TEvDDiskIoResult : TEventLocal<TEvDDiskIoResult, EvDDiskIoResult> {
                NPDisk::TUringOperationBase::EOperationType OperationType;
                NKikimrBlobStorage::NDDisk::TReplyStatus::E Status;
                TString ErrorMessage;
                TRope Data;
                TActorId OriginalRequester;
                TActorId InterconnectSession;
                ui64 Cookie = 0;
                NWilson::TSpan Span;
                ui64 TotalSize = 0;
                double RequestTimeMs = 0;
                ui64 TabletId = 0;
                ui64 VChunkIndex = 0;
                bool HasChunkKey = false;
                std::vector<ui64> Checksums;
                ui64 IndexedReadToken = 0;

                TEvDDiskIoResult(NPDisk::TUringOperationBase::EOperationType operationType,
                        NKikimrBlobStorage::NDDisk::TReplyStatus::E status, TString errorMessage,
                        TRope data, TActorId originalRequester, TActorId interconnectSession,
                        ui64 cookie, NWilson::TSpan span, ui64 totalSize, double requestTimeMs,
                        ui64 tabletId = 0, ui64 vChunkIndex = 0, bool hasChunkKey = false,
                        std::vector<ui64> checksums = {})
                    : OperationType(operationType)
                    , Status(status)
                    , ErrorMessage(std::move(errorMessage))
                    , Data(std::move(data))
                    , OriginalRequester(originalRequester)
                    , InterconnectSession(interconnectSession)
                    , Cookie(cookie)
                    , Span(std::move(span))
                    , TotalSize(totalSize)
                    , RequestTimeMs(requestTimeMs)
                    , TabletId(tabletId)
                    , VChunkIndex(vChunkIndex)
                    , HasChunkKey(hasChunkKey)
                    , Checksums(std::move(checksums))
                {}
            };

            struct TEvIntegrityIoResult : TEventLocal<TEvIntegrityIoResult, EvIntegrityIoResult> {
                NKikimrBlobStorage::NDDisk::TReplyStatus::E Status;
                TString ErrorMessage;
                TRope Data;
                TEvIntegrityIoResult(NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
                        TString errorMessage = {}, TRope data = {})
                    : Status(status), ErrorMessage(std::move(errorMessage)), Data(std::move(data))
                {}
            };

            struct TEvReadPartsResult : TEventLocal<TEvReadPartsResult, EvReadPartsResult> {
                struct TPartResult {
                    ui64 Id;
                    NKikimrBlobStorage::NDDisk::TReplyStatus::E Status;
                    TString ErrorMessage;
                    TRope Data;
                };
                std::vector<TPartResult> Parts;
                explicit TEvReadPartsResult(std::vector<TPartResult> parts)
                    : Parts(std::move(parts))
                {}
            };

            struct TEvChunkFormatIoResult : TEventLocal<TEvChunkFormatIoResult, EvChunkFormatIoResult> {
                TChunkIdx ChunkIdx = 0;
                ui32 OffsetInBytes = 0;
                ui32 Size = 0;
                NKikimrBlobStorage::NDDisk::TReplyStatus::E Status =
                    NKikimrBlobStorage::NDDisk::TReplyStatus::UNKNOWN;
                TString ErrorMessage;

                TEvChunkFormatIoResult(TChunkIdx chunkIdx, ui32 offsetInBytes, ui32 size,
                        NKikimrBlobStorage::NDDisk::TReplyStatus::E status, TString errorMessage = {})
                    : ChunkIdx(chunkIdx)
                    , OffsetInBytes(offsetInBytes)
                    , Size(size)
                    , Status(status)
                    , ErrorMessage(std::move(errorMessage))
                {}
            };

            struct TEvInternalSyncWriteResult : TEventLocal<TEvInternalSyncWriteResult, EvInternalSyncWriteResult> {
                NKikimrBlobStorage::NDDisk::TReplyStatus::E Status;
                TString ErrorMessage;
                TEvInternalSyncWriteResult(NKikimrBlobStorage::NDDisk::TReplyStatus::E status,
                        TString errorMessage = {})
                    : Status(status), ErrorMessage(std::move(errorMessage))
                {}
            };
        };

    private:
        enum EWakeupTag {
            WakeupUpdateFreeSpaceInfo = 2,
            WakeupCollectPbStats = 3,
            WakeupProcessPersistentBufferBatchWrite = 4,
            WakeupProcessDeallocatePersistentBufferChunk = 5,
        };

        struct TPbOpSnapshot {
            TInstant Timestamp;
            ui64 Requests = 0;
            std::vector<ui64> BucketCounts;
        };

        // Sliding window of cumulative snapshots for each PB operation,
        // used to compute IOPS and latency percentiles over the last ~15 seconds.
        std::unordered_map<TString, std::deque<TPbOpSnapshot>> PbStatsHistory;
        static constexpr TDuration PbStatsWindow = TDuration::Seconds(15);
        static constexpr TDuration PbStatsSnapshotPeriod = TDuration::Seconds(1);

        void CollectPbStatsSnapshot();

        const bool IsPersistentBufferActor = false;

        // Actor-thread-only health state. I/O callbacks communicate status/data exclusively
        // through TEvPrivate callbacks, so Broken ordering is defined by the actor mailbox.
        bool Broken = false;
        TString BrokenReason;

        static constexpr TDuration StopIoTimeout = TDuration::Minutes(1);
        static constexpr TStringBuf StoppingReason = "DDisk is stopping";
        bool Stopping = false;
        bool IoStalled = false;
        bool PoisonReceived = false;
        bool OwnDrainComplete = false;
        bool OwnDrainFinishing = false;
        void CompleteStop();
        bool PersistentBufferGone = true;
        TActorId ParentDDiskId;
        static constexpr ui64 PBShutdownCookie = Max<ui64>();
        void BeginStopping(TString reason);
        void HandleBeginStopping();
        void TryCompleteStop();
        void HandleGone(TEvents::TEvGone::TPtr ev);
        void CancelPendingIo(std::unique_ptr<TDirectIoOpBase> op);
        void CancelRetries();
        void HandleRetryIODelayed(TEvPrivate::TEvRetryIODelayed::TPtr ev);

        void RejectQueryWhenStopping(IEventHandle& ev);
        void RejectQuery(IEventHandle& ev,
            NKikimrBlobStorage::NDDisk::TReplyStatus::E status, const TString& reason);
        void RejectPendingDDiskQueries(
            NKikimrBlobStorage::NDDisk::TReplyStatus::E status, const TString& reason);
        void RejectQueuedQueries();
        void FinishStopping();
        void HandleStopIoTimeout();
        void ClearIoStalled();

        bool IsBroken() const;
        bool ChecksumsEnabled() const {
            return Config.EnableChecksums;
        }
        TString GetBrokenReason() const;
        void EnterBroken(TString reason);
        void FailDirectIoOp(std::unique_ptr<TDirectIoOpBase> op, TString reason = {});

    public:
        TDDiskActor(TVDiskConfig::TBaseInfo&& baseInfo, TIntrusivePtr<TBlobStorageGroupInfo> info,
            TPersistentBufferFormat&& pbFormat, TDDiskConfig&& ddiskConfig,
            TIntrusivePtr<NMonitoring::TDynamicCounters> counters, bool isPersistentBufferActor = false);

        TDDiskActor(TVDiskConfig::TBaseInfo&& baseInfo, TIntrusivePtr<TBlobStorageGroupInfo> info,
            TPersistentBufferFormat&& pbFormat, TDDiskConfig&& ddiskConfig,
            TIntrusivePtr<NMonitoring::TDynamicCounters> counters, const std::vector<ui32>& initPersistentBufferChunks,
            ui64 persistentBufferUniqueId, TIntrusivePtr<TPDiskParams> pDiskParams, NPDisk::TDiskFormatPtr diskFormat
#if defined(__linux__)
            , std::shared_ptr<NPDisk::IUringRouterClient> uringRouter
#endif
            );

        ~TDDiskActor();
        void Bootstrap();
        STFUNC(StateFuncDDisk);
        STFUNC(StateFuncPersistentBuffer);
        STFUNC(StateFuncStopping);
        void PassAway() override;

        // Mirrors TVDiskContext::CheckPDiskResponse: returns true on OK, returns false and
        // switches to StateFuncStopping on session-loss statuses (ERROR / INVALID_OWNER /
        // INVALID_ROUND) and device-error statuses (CORRUPTED / OUT_OF_SPACE), Y_ABORTs on
        // anything else. Caller must `return` immediately on false because the actor's
        // state has changed.
        bool CheckPDiskReply(NKikimrProto::EReplyStatus status,
            const TString& errorReason, TStringBuf source);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Boot sequence and PDisk management
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        struct TPendingEvent {
            std::unique_ptr<IEventHandle> Ev;
            NWilson::TSpan QueueSpan;

            template<typename TEvent>
            TPendingEvent(TAutoPtr<TEventHandle<TEvent>> ev, const char *name)
                : Ev(ev.Release())
                , QueueSpan(TWilson::DDiskTopLevel, NWilson::TTraceId(Ev->TraceId), name, NWilson::EFlags::AUTO_END,
                    TActivationContext::ActorSystem())
            {
                NPrivate::AddMessageWaitAttributes(QueueSpan);
            }

            TAutoPtr<IEventHandle> Release() {
                return Ev.release();
            }
        };

        struct TChunkRef {
            TChunkIdx ChunkIdx = 0;
            ui32 InFlightDataIo = 0;

            bool AllocationPending = false;
            ui32 AllocationWaiters = 0;
            NActors::TAsyncEvent AllocationReady;

            NActors::TAsyncEvent CommitReady;

            bool IntegrityExtentWriteInFlight = false;
            std::list<ui64> ExtentWaiters;
            NActors::TAsyncEvent ExtentAvailable;
        };

        class TDataIoPin {
        public:
            explicit TDataIoPin(TChunkRef& chunk)
                : Chunk(&chunk)
            {
                ++Chunk->InFlightDataIo;
            }

            ~TDataIoPin() {
                if (Chunk) { --Chunk->InFlightDataIo; }
            }

            TDataIoPin(TDataIoPin&& other) noexcept
                : Chunk(std::exchange(other.Chunk, nullptr))
            {}

            TDataIoPin(const TDataIoPin&) = delete;
            TDataIoPin& operator=(const TDataIoPin&) = delete;

        private:
            TChunkRef* Chunk;
        };

        // Node-stable: waiters hold TChunkRef& (and its TAsyncEvent members) across co_await.
        THashMap<ui64, THashMap<ui64, TChunkRef>> ChunkRefs; // TabletId -> (VChunkIndex -> ChunkIdx)

        TIntrusivePtr<TPDiskParams> PDiskParams;
        std::vector<TChunkIdx> OwnedChunksOnBoot;
        std::queue<TChunkIdx> StartupOrphanChunks;
        ui64 ChunkMapSnapshotLsn = Max<ui64>();
        std::queue<TPendingEvent> PendingQueries;
        bool HandlingQueries = false;
        bool LogReplayComplete = false;
        std::optional<ui64> DeferredCutLogFreeUpToLsn;
        ui64 NextLsn = 1;
        std::set<std::tuple<ui64, ui64, ui32>> ChunkMapIncrementsInFlight;

        void InitPDiskInterface();
        void Handle(NPDisk::TEvYardInitResult::TPtr ev);
        void Handle(NPDisk::TEvReadLogResult::TPtr ev);
        void ValidateChecksumsModeAfterLogReplay();
        void ReconcileStartupReservations();
        void ForgetNextStartupOrphan();
        void FinishRecovery();
        void StartHandlingQueries();
        void HandleSingleQuery();

        template<typename TEvent>
        bool CanHandleQuery(TAutoPtr<TEventHandle<TEvent>>& ev) {
            if (HandlingQueries) {
                return true;
            }
            PendingQueries.emplace(ev, "WaitPDiskInit");
            return false;
        }

        // Chunk management code

        // DDisk may pull an integrity chunk from the same reserve as a data
        // chunk, so it keeps a larger reserve than PersistentBuffer.
        static constexpr ui32 MinChunksReservedDDisk = 4;
        static constexpr ui32 MinChunksReservedPersistentBuffer = 2;
        const ui32 MinChunksReserved;
        TChunkManager ChunkManager;
        // Newly reserved chunks are zeroed in slices before they become allocatable in
        // checksums-disabled mode. Value is the next byte offset to format.
        absl::flat_hash_map<TChunkIdx, ui32> FormattingChunks;
        // Abandoned allocations may still have writes in flight. Never reuse them for PB.
        absl::flat_hash_set<TChunkIdx> PendingChunkRelease;
        absl::flat_hash_set<TChunkIdx> ShutdownChunkReleasesIssued;
        using TChunkForData = TChunkManager::TChunkForData;
        using TChunkForPersistentBuffer = TChunkManager::TChunkForPersistentBuffer;
        using TChunkForIntegrity = TChunkManager::TChunkForIntegrity;
        struct TLogWaiter {
            NActors::TAsyncContinuation<bool> Continuation;
            ui64 DeliveryCookie = 0;
            bool IsDDisk = false;
        };
        absl::flat_hash_map<ui64, TLogWaiter> LogWaiters;
        NActors::async<bool> WaitForLog(ui64 lsn);
        void FailLogWaiters(bool ddiskOnly);
        ui64 NextCookie = 1;

        struct TPendingIoOp {
            std::unique_ptr<TDirectIoOpBase> Op;

            TPendingIoOp() = default;
            explicit TPendingIoOp(std::unique_ptr<TDirectIoOpBase> op);
            TPendingIoOp(TPendingIoOp&&) noexcept;

            TPendingIoOp(const TPendingIoOp&) = delete;

            TPendingIoOp& operator=(TPendingIoOp&&) noexcept;
            TPendingIoOp& operator=(const TPendingIoOp&) = delete;

            ~TPendingIoOp();
        };

        THashMap<ui64, TPendingIoOp> WriteCallbacks;
        THashMap<ui64, TPendingIoOp> ReadCallbacks;
        struct TReadPartCallback {
            ui64 ParentCookie;
            size_t Index;
        };
        THashMap<ui64, TReadPartCallback> ReadPartCallbacks;
        THashMap<ui64, size_t> ReadPartsRemaining;
        THashMap<ui64, TPendingIoOp> DelayedRetries;
        ui64 NextRetryId = 0;

        void IssueChunkAllocation(ui64 tabletId, ui64 vChunkIndex);
        NActors::async<void> WaitForChunk(ui64 tabletId, ui64 vChunkIndex, bool allocate);
        void ReserveChunks(size_t count);
        void ReleaseUncommittedChunks();
        void HandleChunkReserved();
        void FormatChunk(TChunkIdx chunkIdx);
        void Handle(NPDisk::TEvLogResult::TPtr ev);
        void Handle(TEvPrivate::TEvHandlePersistentBufferEventForChunk::TPtr ev);

        void Handle(NPDisk::TEvCutLog::TPtr ev);
        void ProcessCutLog(ui64 freeUpToLsn);
        void Handle(TEvDeleteTabletChunks::TPtr ev);
        // Tablets whose removal snapshot is not committed yet. Their integrity extents are
        // quarantined in TIntegrityManager and data operations must not start a new incarnation
        // with the same (TabletId, VChunkIndex) keys until the deletion becomes durable.
        absl::flat_hash_set<ui64> TabletChunkDeletionsInFlight;
        struct TTabletChunkDeletionReply {
            TActorId ReplyTo;
            ui64 Cookie = 0;
            TActorId InterconnectSession;
        };
        THashMap<ui64, TTabletChunkDeletionReply> TabletChunkDeletionReplies;

        void Handle(NPDisk::TEvChunkWriteRawResult::TPtr ev);
        void Handle(NPDisk::TEvChunkReadRawResult::TPtr ev);

        ui64 GetFirstLsnToKeep() const;

        ui64 IssuePDiskLogRecord(TLogSignature signature, TChunkIdx chunkIdxToCommit, const NProtoBuf::Message& data,
            ui64 *startingPointLsnPtr,
            TVector<TChunkIdx> chunksToDelete = {});
        ui64 IssuePDiskLogRecord(TLogSignature signature, TVector<TChunkIdx> chunksToCommit,
            const NProtoBuf::Message& data, ui64 *startingPointLsnPtr,
            TVector<TChunkIdx> chunksToDelete = {});

        NKikimrBlobStorage::NDDisk::NInternal::TPersistentBufferChunkMapLogRecord CreatePersistentBufferChunkMapSnapshot();
        NKikimrBlobStorage::NDDisk::NInternal::TChunkMapLogRecord CreateChunkMapSnapshot();
        NKikimrBlobStorage::NDDisk::NInternal::TChunkMapLogRecord CreateChunkMapIncrement(ui64 tabletId, ui64 vChunkIndex,
            TChunkIdx chunkIdx, const TIntegrityManager::TExtentRef* extentRef,
            const TIntegrityManager::TMappingSnapshot::TIntegrityChunkEntry* integrityChunk = nullptr);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Integrity management (DDisk mode only)
        //
        // TIntegrityManager owns coroutine workflows for integrity chunks, extents and pairs;
        // this actor supplies allocation and typed awaitable device I/O. A reserved
        // chunk is formatted immediately. Data writes start once the extent is placed (IntegrityChunk
        // found). The combined chunk-map increment is logged only after the extent is Ready, and
        // the originating write/sync is not answered until that record is durable.
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        // Constructed in Handle(TEvYardInitResult) once DiskFormat (chunk size) is known.
        std::optional<TIntegrityManager> IntegrityManager;

        // Integrity chunks that have appeared in a durable (or in-flight) log record, with the
        // generation stamped into that record. Appended when the increment is issued, so a
        // concurrently written snapshot already includes the chunk - by the time that snapshot is
        // read back the commit has landed.
        std::vector<TIntegrityManager::TMappingSnapshot::TIntegrityChunkEntry> CommittedIntegrityChunks;

        // DataChunk -> IntegrityExtent mapping accumulated from the chunk-map snapshot and log
        // increments during boot; fed to IntegrityManager->ApplyMappingSnapshot at end-of-log.
        TIntegrityManager::TMappingSnapshot RestoredIntegrityMapping;

        struct TDataChunkAllocationInFlight {
            TChunkIdx ChunkIdx = 0;
            bool LogIssued = false;
            ui32 NewlyCommittedChunks = 0;
        };
        absl::flat_hash_map<std::pair<ui64, ui64>, TDataChunkAllocationInFlight> DataChunkAllocationsInFlight;

        struct TIntegrityHost : TIntegrityManager::IHost {
            TDDiskActor& Self;
            explicit TIntegrityHost(TDDiskActor& self) : Self(self) {}
            IActor& Actor() override { return Self; }
            void Launch(std::function<NActors::async<void>()> factory) override { Self.LaunchIntegrity(std::move(factory)); }
            NActors::async<TChunkIdx> Allocate() override { return Self.AllocateIntegrityChunk(); }
            void ReturnChunk(TChunkIdx chunk) override { Self.ChunkManager.ReturnChunk(chunk); }
            void SubmitPairReads(std::vector<TIntegrityManager::TPairRead> reads) override {
                Self.SubmitIntegrityPairReads(std::move(reads));
            }
            NActors::async<bool> Write(TChunkIdx chunk, ui32 offset, TRcBuf data,
                    TIntegrityManager::EWriteIoKind kind) override {
                return Self.WriteIntegrity(chunk, offset, std::move(data), kind);
            }
        } IntegrityHost{*this};
        std::deque<NActors::TAsyncContinuation<TChunkIdx>> IntegrityAllocations;
        void LaunchIntegrity(std::function<NActors::async<void>()> factory);
        NActors::async<TChunkIdx> AllocateIntegrityChunk();
        void SubmitIntegrityPairReads(std::vector<TIntegrityManager::TPairRead> reads);
        NActors::async<bool> WriteIntegrity(TChunkIdx chunk, ui32 offset, TRcBuf data,
            TIntegrityManager::EWriteIoKind kind);
        void CountIntegrityResult(const TIntegrityManager::TOperationResult& result);
        // Callers retain a TDataIoPin until the returned completion is consumed.
        using TDataIoCompletion = decltype(NActors::ActorWaitForEvent<TEvPrivate::TEvDDiskIoResult>(0));
        using TSyncIoCompletion = decltype(NActors::ActorWaitForEvent<TEvPrivate::TEvInternalSyncWriteResult>(0));
        TDataIoCompletion SubmitDataIo(std::unique_ptr<TDirectIoOpBase> op);
        TSyncIoCompletion SubmitSyncIo(std::unique_ptr<TDirectIoOpBase> op);
        bool IsChunkCommitted(ui64 tabletId, ui64 vChunkIndex) const;
        NActors::async<bool> WaitForChunkCommit(ui64 tabletId, ui64 vChunkIndex);
        static std::unique_ptr<TEvPrivate::TEvDDiskIoResult> MakeDDiskReadResult(
            const IEventHandle& request, ui64 tabletId, const TBlockSelector& selector, NWilson::TSpan&& span);
        bool SubmitDDiskDataRead(TEvRead::TPtr& request, TChunkIdx chunkIdx,
            ui64 tabletId, const TBlockSelector& selector, NWilson::TSpan& span, ui64 indexedReadToken,
            std::unique_ptr<TEvPrivate::TEvDDiskIoResult>& result);
        struct TPendingDDiskRead {
            TDataIoPin Pin;
            TIntegrityManager::TOperation Metadata;
            std::unique_ptr<TEvPrivate::TEvDDiskIoResult> Result;
            NActors::TAsyncEvent Changed;
            bool IoPending = false;
            bool Done = false;
            bool Detached = false;

            explicit TPendingDDiskRead(TChunkRef& chunk) : Pin(chunk) {}
        };
        THashMap<ui64, std::shared_ptr<TPendingDDiskRead>> PendingDDiskReads;

        class TDDiskReadAwaiter;
        struct TIndexedReadSlot {
            ui32 Generation = 1;
            ui32 NextFree = Max<ui32>();
            TDDiskReadAwaiter* Waiter = nullptr;
            std::optional<TDataIoPin> Pin;
            bool Submitted = false;
        };
        std::vector<TIndexedReadSlot> IndexedReads;
        ui32 FirstFreeIndexedRead = Max<ui32>();
        size_t ActiveIndexedReads = 0;
        ui64 ReserveIndexedRead(TDDiskReadAwaiter& waiter, TChunkRef& chunk);
        TIndexedReadSlot* FindIndexedRead(ui64 token);
        void ReleaseIndexedRead(ui64 token);
        void DetachIndexedRead(ui64 token);
        void Handle(TEvPrivate::TEvDDiskIoResult::TPtr ev);

        class TDDiskReadAwaiter {
        public:
            static constexpr bool IsActorAwareAwaiter = true;
            TDDiskReadAwaiter(TDDiskActor& self, TEvRead::TPtr& request, TChunkRef& chunk,
                ui64 tabletId, const TBlockSelector& selector, NWilson::TSpan& span);
            ~TDDiskReadAwaiter();
            TDDiskReadAwaiter(const TDDiskReadAwaiter&) = delete;
            TDDiskReadAwaiter& CoAwaitByValue() && noexcept { return *this; }
            bool await_ready() const noexcept;
            template<class TPromise>
            void await_suspend(std::coroutine_handle<TPromise> parent) {
                if (Mode == EMode::DataEvent) {
                    Continuation = parent;
                } else {
                    Cold->Waiter.await_suspend(parent);
                }
            }
            std::coroutine_handle<> await_cancel(std::coroutine_handle<> continuation) noexcept {
                if (Mode == EMode::DataEvent) {
                    if (!Token) { return {}; }
                    Self.DetachIndexedRead(std::exchange(Token, 0));
                    Continuation = {};
                    return continuation;
                }
                return Cold->Waiter.await_cancel(continuation) ? continuation : std::coroutine_handle<>{};
            }
            std::unique_ptr<TEvPrivate::TEvDDiskIoResult> await_resume();

        private:
            friend class TDDiskActor;
            enum class EMode { Ready, DataEvent, Cold } Mode = EMode::Ready;
            struct TColdWait {
                std::shared_ptr<TPendingDDiskRead> Context;
                decltype(Context->Changed.Wait()) Waiter;
                explicit TColdWait(std::shared_ptr<TPendingDDiskRead> context)
                    : Context(std::move(context)), Waiter(Context->Changed.Wait())
                {}
            };
            TDDiskActor& Self;
            std::unique_ptr<TEvPrivate::TEvDDiskIoResult> Result;
            std::optional<TIntegrityManager::TOperationResult> Metadata;
            ui64 Token = 0;
            std::coroutine_handle<> Continuation;
            std::optional<TColdWait> Cold;
        };
        TDDiskReadAwaiter ReadDDisk(TEvRead::TPtr& request, TChunkRef& chunk,
            ui64 tabletId, const TBlockSelector& selector, NWilson::TSpan& span);
        void ApplyDDiskReadMetadata(TEvPrivate::TEvDDiskIoResult& result,
            TIntegrityManager::TOperationResult&& metadata);
        void ApplyDDiskReadMetadata(TEvPrivate::TEvDDiskIoResult& result,
            const TIntegrityManager::TOperationResult& metadata);
        template<class TMetadata>
        void ApplyDDiskReadMetadataImpl(TEvPrivate::TEvDDiskIoResult& result, TMetadata&& metadata);
        void TryFinishDDiskRead(ui64 cookie);
        void Handle(TEvPrivate::TEvReadPartsResult::TPtr ev);
        void FinishDDiskIoResult(TEvPrivate::TEvDDiskIoResult& msg);
        void ReleaseIntegrityExtentWrite(ui64 tabletId, ui64 vChunkIndex);
        bool TryAcquireIntegrityExtent(TChunkRef& chunk);
        NActors::async<bool> AcquireIntegrityExtent(ui64 tabletId, ui64 vChunkIndex);
        // Assigns newly free slots to pending extents, starts their formatting, and
        // releases integrity chunks that remain completely unused. Never-logged chunks return to
        // the reserve; committed ones are dropped via a snapshot. Returns only after the
        // optional release snapshot commits, or false on terminal failure.
        NActors::async<bool> ReclaimUnusedIntegrityChunks();
        struct TIntegrityReclamation {
            bool Ok;
            std::optional<ui64> LogLsn;
        };
        // Performs eager placement/reclamation and submits its optional durability log.
        TIntegrityReclamation PrepareIntegrityReclamation();
        void RunIntegrityReclamation();
        void WaitForIntegrityReclamation(ui64 lsn);
        NActors::async<bool> CommitDataChunk(ui64 tabletId, ui64 vChunkIndex);
        void AllocateChunk(TChunkManager::TAllocation allocation, TChunkIdx chunkIdx);
        NActors::async<void> AllocateDataChunk(ui64 tabletId, ui64 vChunkIndex, TChunkIdx chunkIdx);
        void CompleteDataChunkAllocation(ui64 tabletId, ui64 vChunkIndex);
        bool IsIntegrityChunkCommitted(TChunkIdx chunkIdx) const;

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Connection management
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        enum class EConnectionTokenInvalidationReason : ui8 {
            Reconnect,
            Disconnect,
        };

        struct TPreviousConnectionTokenInfo {
            TConnectionToken Token;
            ui64 TabletId = 0;
            ui32 Generation = 0;
            ui32 DirectBlockGroupIndex = 0;
            ui64 DDiskSessionSeqNo = 0;
            EConnectionTokenInvalidationReason InvalidationReason = EConnectionTokenInvalidationReason::Reconnect;
            bool Valid = false;
        };

        struct TConnectionInfo {
            ui64 TabletId = 0;
            ui32 Generation = 0;
            ui32 DirectBlockGroupIndex = 0;
            ui64 DDiskSessionSeqNo = 0;
            ui32 NodeId = 0;
            TActorId InterconnectSessionId;
            TConnectionToken Token;
            ui8 TokenSequenceNo = 0;
            std::array<TPreviousConnectionTokenInfo, 2> PreviousTokens;
            ui32 NextPreviousTokenIndex = 0;
            bool Active = false;
        };

        using TConnectionKey = std::pair<ui64, ui32>;
        TVector<TConnectionInfo> Connections;
        THashMap<TConnectionKey, ui32> ConnectionIndexBySession;
        TVector<ui32> FreeConnectionIndices;

        void Handle(TEvConnect::TPtr ev);
        void Handle(TEvDisconnect::TPtr ev);

        TConnectionToken IssueConnectionToken(ui32 connectionIndex, TConnectionInfo& connection);

        void RememberConnectionToken(TConnectionInfo& connection, EConnectionTokenInvalidationReason reason);

        enum class EConnectionResolution : ui8 {
            Resolved,
            StaleToken,
            InvalidToken,
        };

        // validate query credentials and restore token-backed connection data
        EConnectionResolution ResolveConnection(const TQueryCredentials& requestCreds, TQueryCredentials* resolvedCreds) const;
        static TStringBuf ConnectionErrorReason(EConnectionResolution resolution);
        static TStringBuf ConnectionInvalidationReason(EConnectionTokenInvalidationReason reason);
        TString DescribeConnectionFailure(const TQueryCredentials& requestCreds, EConnectionResolution resolution) const;

        // a general way to send reply to any incoming message
        void SendReply(const IEventHandle& queryEv, std::unique_ptr<IEventBase> replyEv) const;

        // common function to validate any incoming event's credentials
        template<typename TEvent, typename TCountersPtr>
        bool CheckQuery(TEventHandle<TEvent>& ev, TCountersPtr counters) const {
            auto& record = ev.Get()->Record;
            using TEventType = std::decay_t<TEvent>;

            auto registerError = [&] {
                if constexpr (!std::is_same_v<TCountersPtr, std::nullptr_t>) {
                    counters->Request(0);
                    counters->Reply(false);
                }
            };

            if (IsBroken()) {
                SendReply(ev, std::make_unique<typename TEvent::TResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR, GetBrokenReason()));
                registerError();
                return false;
            }

            auto logError = [&](TStringBuf reason) {
                YDB_LOG_DEBUG_CTX_COMP(*TActivationContext::ActorSystem(), NKikimrServices::BS_DDISK, "TDDiskActor::CheckQuery validation failed",
                    {"reason", reason},
                    {"DDiskId", DDiskId},
                    {"evType", ev.GetTypeRewrite()},
                    {"sender", ev.Sender},
                    {"cookie", ev.Cookie},
                    {"ICSession", ev.InterconnectSession});
            };

            const TQueryCredentials requestCreds(record.GetCredentials());
            TQueryCredentials creds;
            const EConnectionResolution resolution = ResolveConnection(requestCreds, &creds);

            if (resolution != EConnectionResolution::Resolved) {
                logError(DescribeConnectionFailure(requestCreds, resolution));
                auto result = std::make_unique<typename TEvent::TResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH
                );
                const TStringBuf errorReason = ConnectionErrorReason(resolution);
                result->Record.SetErrorReason(errorReason.data(), errorReason.size());

                SendReply(ev, std::move(result));
                registerError();
                return false;
            }

            creds.SerializeResolvedForRequest(record.MutableCredentials());

            if constexpr (std::is_same_v<TEventType, TEvWritePersistentBuffer>
                    || std::is_same_v<TEventType, TEvReadPersistentBuffer>
                    || std::is_same_v<TEventType, TEvErasePersistentBuffer>
                    || std::is_same_v<TEventType, TEvBatchErasePersistentBuffer>
                    || std::is_same_v<TEventType, TEvListPersistentBuffer>) {
                // NOTE: durable registration (TEvRegisterPersistentBuffer) is a hard precondition
                // for all persistent-buffer reads/writes/erases below. This requires lockstep
                // upgrades of client and DDisk: an older client without register support loses
                // all PB operations against a DDisk running this code, and this client build
                // connecting to an older DDisk gets its register event undelivered and the
                // connect fails. There is no fallback or version negotiation, so mixed fleets
                // running old/new builds simultaneously are not supported for PB traffic.
                if (PersistentBufferReady) {
                    const auto status = CheckPersistentBufferOwnership(creds);
                    if (status != NKikimrBlobStorage::NDDisk::TReplyStatus::OK) {
                        SendReply(ev, std::make_unique<typename TEvent::TResult>(status,
                            "persistent buffer registration is not registered, not yet durable, or closed"));
                        registerError();
                        return false;
                    }
                }
            }

            using TRecord = std::decay_t<decltype(record)>;

            if constexpr (NPrivate::THasSelectorField<TRecord>::value) {
                const TBlockSelector selector(record.GetSelector());

                if (selector.OffsetInBytes % DiskFormat->SectorSize || selector.Size % DiskFormat->SectorSize || !selector.Size) {
                    TStringStream ss;
                    ss << "offset and size must be multiple of sector size and size must be nonzero: ";
                    selector.Print(ss);
                    logError(ss.Str());
                    SendReply(ev, std::make_unique<typename TEvent::TResult>(
                        NKikimrBlobStorage::NDDisk::TReplyStatus::INCORRECT_REQUEST,
                        ss.Str()));
                    registerError();
                    return false;
                }

                if constexpr (std::is_same_v<TEventType, TEvRead> || std::is_same_v<TEventType, TEvWrite>) {
                    if (selector.OffsetInBytes > DiskFormat->ChunkSize ||
                            selector.Size > DiskFormat->ChunkSize - selector.OffsetInBytes) {
                        TStringStream ss;
                        ss << "request should be within a chunk (chunk size: " << DiskFormat->ChunkSize << "): ";
                        selector.Print(ss);
                        logError(ss.Str());
                        SendReply(ev, std::make_unique<typename TEvent::TResult>(
                            NKikimrBlobStorage::NDDisk::TReplyStatus::INCORRECT_REQUEST,
                            ss.Str()));
                        registerError();
                        return false;
                    }
                }

                if constexpr (NPrivate::THasWriteInstructionField<TRecord>::value) {
                    const TWriteInstruction instruction(record.GetInstruction());
                    size_t size = 0;
                    if (instruction.PayloadId) {
                        const TRope& data = ev.Get()->GetPayload(*instruction.PayloadId);
                        size = data.size();
                    }
                    // this check is crucial for the code submitting IO
                    if (size != selector.Size) {
                        TStringStream ss;
                        ss << "declared data size must match actually sent one: size="
                            << size << ", selector.Size=" << selector.Size << ", ";
                        selector.Print(ss);
                        logError(ss.Str());
                        SendReply(ev, std::make_unique<typename TEvent::TResult>(
                            NKikimrBlobStorage::NDDisk::TReplyStatus::INCORRECT_REQUEST,
                            ss.Str()));
                        registerError();
                        return false;
                    }
                }
            }

            return true;
        }

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Read/write
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        // PDisk read/write fallback
        void SendPDiskWrite(std::unique_ptr<TDirectIoOpBase> op);
        void SendPDiskRead(std::unique_ptr<TDirectIoOpBase> op);

        void Handle(TEvWrite::TPtr ev);
        void Handle(TEvRead::TPtr ev);

        // Regular direct I/O.
        // Note: releases the op when it is submitted to io_uring or moved to the PDisk fallback.
        void DirectUringOp(std::unique_ptr<TDirectIoOpBase>& op, bool isRetry = false);

        // Do not call manually!
        void DirectUringOpImpl(std::unique_ptr<TDirectIoOpBase>& op);

        void HandleRetryIO(TEvPrivate::TEvRetryIO::TPtr ev);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Sync
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        struct TSyncReadRequest {
            NKikimrBlobStorage::NDDisk::TReplyStatus::E Status = NKikimrBlobStorage::NDDisk::TReplyStatus::UNKNOWN;
            TBlockSelector Selector;
            TString ErrorReason;
            TActorId Source;
            std::unique_ptr<IEventBase> Query;
            ui64 RequestId = 0;
            bool Admitted = false;
            NActors::TAsyncCancellationScope Preparation;
        };
        struct TSyncInFlight {
            TQueryCredentials Creds;
            NWilson::TSpan Span;
            ui64 VChunkIndex = 0;
            std::deque<TSyncReadRequest> Requests;
        };
        ui64 NextSyncId = 1;
        THashMap<ui64, TSyncInFlight*> SyncsInFlight; // non-owning; root coroutine owns each context
        THashSet<ui64> SyncReadCookiesInFlight; // includes expected late replies after preparation cancellation
        TSegmentManager SegmentManager;
        struct TSyncData {
            TRope Data;
            std::vector<ui64> Checksums;
            std::vector<TSegmentManager::TSegment> Segments;
        };
        struct TSyncSegmentResult {
            NKikimrBlobStorage::NDDisk::TReplyStatus::E Status = NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
            TString ErrorReason;
        };
        void Handle(TEvSync::TPtr ev);
        void Handle(TEvReadResult::TPtr ev);
        void Handle(TEvReadPersistentBufferResult::TPtr ev);
        void HandleLateSyncSource(ui64 cookie);
        NActors::async<TSyncData> PrepareSyncSource(TSyncInFlight& sync, TSyncReadRequest& request);
        NActors::async<void> RunSyncSource(TSyncInFlight& sync, TSyncReadRequest& request);
        NActors::async<TSyncSegmentResult> WriteSyncSegment(TSyncInFlight& sync, ui32 begin,
            TRope data, std::vector<ui64> checksums);

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Persistent buffer services
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        std::map<TPersistentBufferId, TPersistentBuffer> PersistentBuffers;
        std::map<TInstant, absl::flat_hash_set<TPersistentBufferRecordId>> PersistentBuffersInMemoryCacheUptime;
        ui64 PersistentBufferInMemoryCacheSize = 0;
        TInstant StartedAt;

        ui64 CalcPersistentBufferInMemoryCacheSize();
        TString PersistentBufferToString();

        void SanitizePersistentBufferInMemoryCache();
        void SanitizePersistentBufferInMemoryCache(ui64 tabletId, ui32 generation, ui64 lsn, TPersistentBuffer::TRecord& record, ui8 directBlockGroupIndex = 0);


        ui32 SectorSize;
        ui32 SectorInChunk;
        ui32 ChunkSize;
        TPersistentBufferFormat PersistentBufferFormat;

        double NormalizedOccupancy = -1;

        bool IssuePersistentBufferChunkAllocationInflight = false;

        struct TEraseLsnId {
            ui32 Generation;
            ui64 Lsn;
        };

        struct TPersistentBufferDiskOperationInFlight {
            struct TRecord {
                TActorId Sender;
                ui64 Cookie;
                TActorId Session;
                NWilson::TSpan Span;

                ui64 TabletId;
                ui32 Generation;
                ui64 VChunkIndex;
                ui64 Lsn;
                ui32 OffsetInBytes;
                ui32 Size;

                std::map<ui64, TRope> DataParts;
                ui32 PartsCount;
                std::vector<TPersistentBufferSectorInfo> Sectors;
                // Sender-supplied per-MinSectorSize-block payload checksums for this record, in order.
                // Empty when the write carried no checksums. See TPersistentBuffer::TRecord::PayloadChecksums.
                std::vector<ui64> PayloadChecksums;
                // Direct block group number this record belongs to. See TPersistentBufferId for
                // rationale; defaults to 0 to preserve the pre-existing single-namespace-per-tablet
                // behavior. Declared last so it never conflicts with designated-initializer ordering
                // at existing call sites that only name fields up to PayloadChecksums.
                ui8 DirectBlockGroupIndex = 0;
                bool ChecksumsDisabled = false;
                ui64 HeaderUniqueId = 0;
                TRope JoinData(ui32 sectorSize);
            };

            std::vector<TRecord> Records;

            absl::flat_hash_set<ui64> OperationCookies;
            // map operationCookie to <lsn, generation> pairs that were erased by this operation
            std::unordered_map<ui64, std::vector<TEraseLsnId>> Erases;
            TRope DataToWrite;

            std::vector<TPersistentBufferSectorInfo> OccupiedSectors;
            NKikimrBlobStorage::NDDisk::TReplyStatus::E Status = NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
            std::optional<TString> ErrorMessage = std::nullopt;

            NHPTimer::STime StartTs{};
            enum class EBarrierOperation { None, Erase, Register, Close, Remove };
            EBarrierOperation BarrierOperation = EBarrierOperation::None;
        };

        struct TPersistentBufferEraseInflight {
            ui64 EraseCookie;
            std::vector<ui64> OperationsCookie;
        };

        ui64 PersistentBufferBatchWriteCookie = 0;
        ui64 NextPersistentBufferHeaderUniqueId = 0;
        absl::flat_hash_map<TPersistentBufferLocation, absl::flat_hash_set<TPersistentBufferRecordId>> PersistentBufferHeaders;
        absl::flat_hash_map<ui64, TPersistentBufferDiskOperationInFlight> PersistentBufferDiskOperationInflight;

        // map record to operation cookie + record in inflight position
        absl::flat_hash_map<TPersistentBufferRecordId, std::vector<std::tuple<ui64, ui32>>> PersistentBufferWriteInflightsByRecord;
        absl::flat_hash_map<TPersistentBufferRecordId, TPersistentBufferEraseInflight> PersistentBufferEraseInflightsByRecord;

        ui32 PersistentBufferRestoreChunksInflight = 0;
        std::vector<ui32> PersistentBufferChunks;
        ui64 PersistentBufferUniqueId = 0;

        TPersistentBufferSpaceAllocator PersistentBufferSpaceAllocator;
        TPersistentBufferBarriersManager PersistentBufferBarriersManager;

        struct TPersistentBufferRemoval {
            enum class EStage { Drain, Close, Wait, Remove };
            EStage Stage = EStage::Drain;
            TInstant Deadline;
            TAutoPtr<IEventHandle> Request;
        };
        std::map<TPersistentBufferTabletKey, TPersistentBufferRemoval> PersistentBufferRemovals;
        std::set<TPersistentBufferTabletKey> PersistentBufferRegistrations;
        // In-flight barrier sectors stay occupied even if a newer version is durable.
        // The flag requests reclamation once this sector's own write has completed.
        absl::flat_hash_map<TPersistentBufferLocation, bool> PersistentBufferBarrierWrites;
        void ReleasePersistentBufferBarrierSector(TPersistentBufferSectorInfo sector);
        void CompletePersistentBufferBarrierWrite(TPersistentBufferDiskOperationInFlight& inflight);
        NKikimrBlobStorage::NDDisk::TReplyStatus::E CheckPersistentBufferOwnership(const TQueryCredentials& creds) const;
        struct TPersistentBufferRegistrationToken {
            ui64 Token = 0;
            TMonotonic IssuedAt = TMonotonic::Zero();
            TPersistentBufferTabletKey Key{};
            ui32 Generation = 0;

            TPersistentBufferRegistrationToken() = default;
            TPersistentBufferRegistrationToken(TMonotonic now, const TQueryCredentials& creds);
            static ui64 Generate(TMonotonic now);
        };
        // Actor-local, ordered by token and issue time; never survives a PB restart.
        std::deque<TPersistentBufferRegistrationToken> PersistentBufferRegistrationTokens;
        bool PersistentBufferRegistrationTokenExpiryScheduled = false;
        void Handle(TEvGetPersistentBufferRegistrationToken::TPtr ev);
        void Handle(TEvPrivate::TEvExpirePersistentBufferRegistrationToken::TPtr ev);
        void Handle(TEvRegisterPersistentBuffer::TPtr ev);
        void Handle(TEvUnregisterPersistentBuffer::TPtr ev);
        void Handle(TEvPrivate::TEvProcessPersistentBufferRemoval::TPtr ev);
        void ProcessPersistentBufferRemoval(TPersistentBufferTabletKey key);

        ui64 PersistentBufferChunkMapSnapshotLsn = Max<ui64>();
        std::queue<TPendingEvent> PendingPersistentBufferEvents;
        bool PersistentBufferReady = false;

        struct TPersistentBufferDataSectorInfo {
            ui64 Checksum;
            ui64 HeaderUniqueId;
        };
        // During restoration every data sector is inspected once for both
        // on-disk formats; the record header flag selects the value to validate.
        absl::flat_hash_map<ui64, std::vector<TPersistentBufferDataSectorInfo>> PersistentBufferDataSectorsInfo;
        absl::flat_hash_set<ui32> PersistentBufferAllocatedChunks;
        absl::flat_hash_set<ui32> PersistentBufferRestoringChunks;

        TActorId WritePersistentBuffersActor;
        TActorId PersistentBufferActorId;

        ui64 CalculateChecksum(const TRope::TIterator begin) {
            return CalculateChecksum(begin, SectorSize);
        }

        ui64 CalculateChecksum(const TRope::TIterator begin, size_t numBytes);

        void CreatePersistentBuffer();
        void InitPersistentBuffer();
        void IssuePersistentBufferChunkAllocation();
        void ProcessDeallocatePersistentBufferChunk(bool forceToNextChunk = false);
        void ProcessPersistentBufferQueue();
        std::vector<std::tuple<ui32, ui32, TRope>> SlicePersistentBuffer(ui64 tabletId, ui32 generation, ui64 vchunkIndex, ui64 lsn, ui32 offsetInBytes, ui32 size, TRcBuf&& payloadWithHeader, std::vector<TPersistentBufferSectorInfo>& sectors, const std::vector<ui64>& payloadChecksums, ui8 directBlockGroupIndex = 0, ui64 headerUniqueId = 0);
        std::vector<std::tuple<ui32, ui32, TRope>> SlicePersistentBufferData(TRope& data, std::vector<TPersistentBufferSectorInfo>& sectors);
        void StartRestorePersistentBuffer();
        void RestorePersistentBufferChunk(TEvPrivate::TEvReadPersistentBufferPart::TPtr ev);
        void ReplyReadPersistentBuffer(ui64 operationCookie);
        void ReplyReadPersistentBuffer(TPersistentBuffer::TRecord& pr, NKikimrBlobStorage::NDDisk::TReplyStatus::E status, std::optional<TString> errorMessage);

        bool PreprocessPersistentBufferWrite(NActors::TEventHandle<TEvWritePersistentBuffer>& ev);
        void ProcessPersistentBufferWrite(TEvWritePersistentBuffer::TPtr ev);
        // ev is taken by reference (not TPtr by value, unlike its sibling above): TPtr is a TAutoPtr
        // with ownership-transferring copy semantics, so a by-value parameter here would null out the
        // caller's ev as soon as this is invoked -- including on the "doesn't fit, fall back" (false)
        // return path, where Handle(TEvWritePersistentBuffer) still needs a valid ev afterwards to retry
        // via ProcessPersistentBufferWrite.
        bool ProcessPersistentBufferBatchWriteData(TEvWritePersistentBuffer::TPtr& ev);
        void ProcessPersistentBufferBatchWrite();
        double GetPersistentBufferFreeSpace();
        void ErasePersistentBuffer(IEventHandle& queryEv, const TQueryCredentials& creds, const std::vector<TEraseLsnId>& erases);
        void BarrierErasePersistentBuffer(IEventHandle& queryEv, const TQueryCredentials& creds, const std::vector<TEraseLsnId>& erases, ui64 lsn,
            TPersistentBufferDiskOperationInFlight::EBarrierOperation operation = TPersistentBufferDiskOperationInFlight::EBarrierOperation::Erase);
        void FastErasePersistentBuffer(IEventHandle& queryEv, const TQueryCredentials& creds, const std::vector<TEraseLsnId>& erases, const TFastErase& fastErase);
        void ClearPersistentBufferRecords(TPersistentBufferDiskOperationInFlight& inflight, ui64 partCookie);
        void HandleWritePart(TPersistentBufferDiskOperationInFlight& inflight,  ui64 opCookie, ui64 partCookie);
        void FinishPersistentBufferWrite(ui64 opCookie);
        void HandleErasePart(TPersistentBufferDiskOperationInFlight& inflight, ui64 opCookie, ui64 partCookie, bool resultStatus);

        void Handle(TEvWritePersistentBuffer::TPtr ev);
        void Handle(TEvReadPersistentBuffer::TPtr ev);
        void Handle(TEvErasePersistentBuffer::TPtr ev);
        void Handle(TEvBatchErasePersistentBuffer::TPtr ev);
        void Handle(TEvWriteResult::TPtr ev);
        void Handle(TEvents::TEvUndelivered::TPtr ev);
        void Handle(TEvListPersistentBuffer::TPtr ev);
        void Handle(TEvPrivate::TEvRetryListPersistentBuffer::TPtr ev);
        // Returns true if the given tablet currently has at least one persistent-buffer disk
        // operation (write/erase/read) in flight. TEvListPersistentBuffer must not be answered
        // while this holds, otherwise it could observe a partially-applied write or erase.
        bool HasPersistentBufferInflightForTablet(ui64 tabletId) const;
        void ProcessListPersistentBuffer(TAutoPtr<TEventHandle<TEvListPersistentBuffer>> ev, ui32 retriesLeft);
        void ReplyListPersistentBuffer(TEventHandle<TEvListPersistentBuffer>& ev);
        void Handle(TEvPrivate::TEvIssuePersistentBufferChunkAllocation::TPtr ev);
        void Handle(TEvPrivate::TEvDeallocatePersistentBufferChunk::TPtr ev);
        void Handle(TEvPrivate::TEvDeallocatePersistentBufferChunkResult::TPtr ev);
        void Handle(TEvGetPersistentBufferInfo::TPtr ev);

        template<typename TEventPtr>
        void HandlePersistentBufferWriteRequest(TEventPtr& ev);

        void Handle(TEvReadThenWritePersistentBuffers::TPtr ev);
        void Handle(TEvWritePersistentBuffers::TPtr ev);

        void Handle(TEvPrivate::TEvReadPersistentBufferPart::TPtr ev);
        void Handle(TEvPrivate::TEvWritePersistentBufferPart::TPtr ev);

        void HandleWakeup(TEvents::TEvWakeup::TPtr &ev);
        void Handle(NPDisk::TEvCheckSpaceResult::TPtr ev);
        void UpdateFreeSpaceInfo();

        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // Monitoring page (DDisk mode only)
        ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        void RegisterMonPage();
        void Handle(NMon::TEvHttpInfo::TPtr ev);
    };

} // NKikimr::NDDisk
