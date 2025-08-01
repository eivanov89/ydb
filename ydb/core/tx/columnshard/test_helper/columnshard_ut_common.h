#pragma once

#include <ydb/core/formats/arrow/arrow_batch_builder.h>
#include <ydb/core/protos/tx_columnshard.pb.h>
#include <ydb/core/scheme/scheme_tabledefs.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/testlib/test_client.h>
#include <ydb/core/tx/columnshard/blob_cache.h>
#include <ydb/core/tx/columnshard/common/snapshot.h>
#include <ydb/core/tx/columnshard/test_helper/helper.h>
#include <ydb/core/tx/data_events/common/modification_type.h>
#include <ydb/core/tx/long_tx_service/public/types.h>
#include <ydb/core/tx/tiering/manager.h>
#include <ydb/core/tx/columnshard/common/path_id.h>

#include <ydb/library/formats/arrow/switch/switch_type.h>
#include <ydb/services/metadata/abstract/fetcher.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

namespace NKikimr::NOlap {
struct TIndexInfo;
}

namespace NKikimr::NTxUT {

using TPlanStep = TPositiveIncreasingControlInteger;

// Private events of different actors reuse the same ES_PRIVATE range
// So in order to capture the right private event we need to check its type via dynamic_cast
template <class TPrivateEvent>
inline TPrivateEvent* TryGetPrivateEvent(TAutoPtr<IEventHandle>& ev) {
    if (ev->GetTypeRewrite() != TPrivateEvent::EventType) {
        return nullptr;
    }
    return dynamic_cast<TPrivateEvent*>(ev->StaticCastAsLocal<IEventBase>());
}

class TTester: public TNonCopyable {
public:
    static constexpr const ui64 FAKE_SCHEMESHARD_TABLET_ID = 4200;

    static void Setup(TTestActorRuntime& runtime);
};

namespace NTypeIds = NScheme::NTypeIds;
using TTypeId = NScheme::TTypeId;
using TTypeInfo = NScheme::TTypeInfo;

struct TTestSchema {
    static inline const TString DefaultTtlColumn = "timestamp";

    struct TStorageTier {
        TString TtlColumn = DefaultTtlColumn;
        std::optional<TDuration> EvictAfter;
        TString Name;
        TString Codec;
        std::optional<int> CompressionLevel;
        NKikimrSchemeOp::TS3Settings S3 = FakeS3();

        TStorageTier(const TString& name = {})
            : Name(name) {
        }

        TString DebugString() const {
            return TStringBuilder() << "{Column=" << TtlColumn << ";EvictAfter=" << EvictAfter.value_or(TDuration::Zero()) << ";Name=" << Name
                                    << ";Codec=" << Codec << "};";
        }

        NKikimrSchemeOp::EColumnCodec GetCodecId() const {
            if (Codec == "none") {
                return NKikimrSchemeOp::EColumnCodec::ColumnCodecPlain;
            } else if (Codec == "lz4") {
                return NKikimrSchemeOp::EColumnCodec::ColumnCodecLZ4;
            } else if (Codec == "zstd") {
                return NKikimrSchemeOp::EColumnCodec::ColumnCodecZSTD;
            }
            Y_ABORT_UNLESS(false);
        }

        bool HasCodec() const {
            return !Codec.empty();
        }

        TStorageTier& SetCodec(const TString& codec, std::optional<int> level = {}) {
            Codec = codec;
            if (level) {
                CompressionLevel = *level;
            }
            return *this;
        }

        TStorageTier& SetTtlColumn(const TString& columnName) {
            TtlColumn = columnName;
            return *this;
        }

        static NKikimrSchemeOp::TS3Settings FakeS3() {
            const TString bucket = "tiering-test-01";

            NKikimrSchemeOp::TS3Settings s3Config;
            s3Config.SetScheme(NKikimrSchemeOp::TS3Settings::HTTP);
            s3Config.SetVerifySSL(false);
            s3Config.SetBucket(bucket);
//#define S3_TEST_USAGE
#ifdef S3_TEST_USAGE
            s3Config.SetEndpoint("storage.cloud-preprod.yandex.net");
            s3Config.SetAccessKey("...");
            s3Config.SetSecretKey("...");
            s3Config.SetProxyHost("localhost");
            s3Config.SetProxyPort(8080);
            s3Config.SetProxyScheme(NKikimrSchemeOp::TS3Settings::HTTP);
#else
            s3Config.SetEndpoint("fake.fake");
            s3Config.SetSecretKey("fakeSecret");
#endif
            s3Config.SetRequestTimeoutMs(10000);
            s3Config.SetHttpRequestTimeoutMs(10000);
            s3Config.SetConnectionTimeoutMs(10000);
            return s3Config;
        }
    };

    struct TTableSpecials: public TStorageTier {
    public:
        std::vector<TStorageTier> Tiers;
        bool WaitEmptyAfter = false;
        bool UseForcedCompaction = false;

        TTableSpecials() noexcept = default;

        bool NeedTestStatistics(const std::vector<NArrow::NTest::TTestColumn>& pk) const {
            return GetTtlColumn() != pk.front().GetName();
        }

        bool HasTiers() const {
            return !Tiers.empty();
        }

        bool HasTtl() const {
            return EvictAfter.has_value();
        }

        bool GetUseForcedCompaction() const {
            return UseForcedCompaction;
        }

        TTableSpecials WithCodec(const TString& codec) const {
            TTableSpecials out = *this;
            out.SetCodec(codec);
            return out;
        }

        TTableSpecials WithForcedCompaction(bool forced) const {
            TTableSpecials out = *this;
            out.UseForcedCompaction = forced;
            return out;
        }

        TTableSpecials& SetTtl(std::optional<TDuration> ttl) {
            EvictAfter = ttl;
            return *this;
        }

        TString DebugString() const {
            auto result = TStringBuilder() << "WaitEmptyAfter=" << WaitEmptyAfter << ";Tiers=";
            for (auto&& tier : Tiers) {
                result << "{" << tier.DebugString() << "}";
            }
            result << ";TTL=" << TStorageTier::DebugString();
            return result;
        }

        TString GetTtlColumn() const {
            for (const auto& tier : Tiers) {
                UNIT_ASSERT_VALUES_EQUAL(tier.TtlColumn, TtlColumn);
            }
            return TtlColumn;
        }
    };
    using TTestColumn = NArrow::NTest::TTestColumn;
    static auto YdbSchema(const TTestColumn& firstKeyItem = TTestColumn("timestamp", TTypeInfo(NTypeIds::Timestamp))) {
        std::vector<TTestColumn> schema = { // PK
            firstKeyItem, TTestColumn("resource_type", TTypeInfo(NTypeIds::Utf8)),
            TTestColumn("resource_id", TTypeInfo(NTypeIds::Utf8)).SetAccessorClassName("SPARSED"),
            TTestColumn("uid", TTypeInfo(NTypeIds::Utf8)).SetStorageId("__MEMORY"), TTestColumn("level", TTypeInfo(NTypeIds::Int32)),
            TTestColumn("message", TTypeInfo(NTypeIds::Utf8)).SetStorageId("__MEMORY"), TTestColumn("json_payload", TTypeInfo(NTypeIds::Json)),
            TTestColumn("ingested_at", TTypeInfo(NTypeIds::Timestamp)), TTestColumn("saved_at", TTypeInfo(NTypeIds::Timestamp)),
            TTestColumn("request_id", TTypeInfo(NTypeIds::Utf8))
        };
        return schema;
    };

    static std::vector<ui32> GetColumnIds(const std::vector<TTestColumn>& schema, const std::vector<TString>& names) {
        std::vector<ui32> result;
        for (auto&& i : names) {
            bool found = false;
            for (ui32 idx = 0; idx < schema.size(); ++idx) {
                if (schema[idx].GetName() == i) {
                    result.emplace_back(idx + 1);
                    found = true;
                    break;
                }
            }
            AFL_VERIFY(found);
        }
        return result;
    }

    static auto YdbExoticSchema() {
        std::vector<TTestColumn> schema = { // PK
            TTestColumn("timestamp", TTypeInfo(NTypeIds::Timestamp)),
            TTestColumn("resource_type", TTypeInfo(NTypeIds::Utf8)).SetAccessorClassName("SPARSED"),
            TTestColumn("resource_id", TTypeInfo(NTypeIds::Utf8)), TTestColumn("uid", TTypeInfo(NTypeIds::Utf8)).SetStorageId("__MEMORY"),
            //
            TTestColumn("level", TTypeInfo(NTypeIds::Int32)), TTestColumn("message", TTypeInfo(NTypeIds::String4k)).SetStorageId("__MEMORY"),
            TTestColumn("json_payload", TTypeInfo(NTypeIds::JsonDocument)), TTestColumn("ingested_at", TTypeInfo(NTypeIds::Timestamp)),
            TTestColumn("saved_at", TTypeInfo(NTypeIds::Timestamp)),
            TTestColumn("request_id", TTypeInfo(NTypeIds::Yson)).SetAccessorClassName("SPARSED")
        };
        return schema;
    };

    static auto YdbPkSchema() {
        std::vector<TTestColumn> schema = { TTestColumn("timestamp", TTypeInfo(NTypeIds::Timestamp)),
            TTestColumn("resource_type", TTypeInfo(NTypeIds::Utf8)).SetStorageId("__MEMORY"),
            TTestColumn("resource_id", TTypeInfo(NTypeIds::Utf8)).SetAccessorClassName("SPARSED"),
            TTestColumn("uid", TTypeInfo(NTypeIds::Utf8)).SetStorageId("__MEMORY") };
        return schema;
    }

    static auto YdbAllTypesSchema() {
        std::vector<TTestColumn> schema = { TTestColumn("ts", TTypeInfo(NTypeIds::Timestamp)),

            TTestColumn("i8", TTypeInfo(NTypeIds::Int8)), TTestColumn("i16", TTypeInfo(NTypeIds::Int16)),
            TTestColumn("i32", TTypeInfo(NTypeIds::Int32)), TTestColumn("i64", TTypeInfo(NTypeIds::Int64)),
            TTestColumn("u8", TTypeInfo(NTypeIds::Uint8)), TTestColumn("u16", TTypeInfo(NTypeIds::Uint16)),
            TTestColumn("u32", TTypeInfo(NTypeIds::Uint32)), TTestColumn("u64", TTypeInfo(NTypeIds::Uint64)),
            TTestColumn("float", TTypeInfo(NTypeIds::Float)), TTestColumn("double", TTypeInfo(NTypeIds::Double)),

            TTestColumn("byte", TTypeInfo(NTypeIds::Byte)),
            //{ "bool", TTypeInfo(NTypeIds::Bool) },
            //{ "decimal", TTypeInfo(NTypeIds::Decimal) },
            //{ "dynum", TTypeInfo(NTypeIds::DyNumber) },

            TTestColumn("date", TTypeInfo(NTypeIds::Date)), TTestColumn("datetime", TTypeInfo(NTypeIds::Datetime)),
            //{ "interval", TTypeInfo(NTypeIds::Interval) },

            TTestColumn("text", TTypeInfo(NTypeIds::Text)), TTestColumn("bytes", TTypeInfo(NTypeIds::Bytes)),
            TTestColumn("yson", TTypeInfo(NTypeIds::Yson)), TTestColumn("json", TTypeInfo(NTypeIds::Json)),
            TTestColumn("jsondoc", TTypeInfo(NTypeIds::JsonDocument)) };
        return schema;
    };

    static void InitSchema(const std::vector<NArrow::NTest::TTestColumn>& columns, const std::vector<NArrow::NTest::TTestColumn>& pk,
        const TTableSpecials& specials, NKikimrSchemeOp::TColumnTableSchema* schema);

    static bool InitTiersAndTtl(const TTableSpecials& specials, NKikimrSchemeOp::TColumnDataLifeCycle* ttlSettings) {
        ttlSettings->SetVersion(1);
        if (!specials.HasTiers() && !specials.HasTtl()) {
            return false;
        }
        ttlSettings->MutableEnabled()->SetColumnName(specials.TtlColumn);
        for (const auto& tier : specials.Tiers) {
            UNIT_ASSERT(tier.EvictAfter);
            UNIT_ASSERT_EQUAL(specials.TtlColumn, tier.TtlColumn);
            auto* tierSettings = ttlSettings->MutableEnabled()->AddTiers();
            tierSettings->MutableEvictToExternalStorage()->SetStorage(tier.Name);
            tierSettings->SetApplyAfterSeconds(tier.EvictAfter->Seconds());
        }
        if (specials.HasTtl()) {
            auto* tier = ttlSettings->MutableEnabled()->AddTiers();
            tier->MutableDelete();
            tier->SetApplyAfterSeconds((*specials.EvictAfter).Seconds());
        }
        return true;
    }

    static TString CreateTableTxBody(ui64 pathId, const std::vector<NArrow::NTest::TTestColumn>& columns,
        const std::vector<NArrow::NTest::TTestColumn>& pk, const TTableSpecials& specialsExt = {}, ui64 generation = 0) {
        auto specials = specialsExt;
        NKikimrTxColumnShard::TSchemaTxBody tx;
        tx.MutableSeqNo()->SetGeneration(generation);
        auto* table = tx.MutableEnsureTables()->AddTables();
        NColumnShard::TSchemeShardLocalPathId::FromRawValue(pathId).ToProto(*table);

        {   // preset
            auto* preset = table->MutableSchemaPreset();
            preset->SetId(1);
            preset->SetName("default");

            // schema
            InitSchema(columns, pk, specials, preset->MutableSchema());
        }

        InitTiersAndTtl(specials, table->MutableTtlSettings());

        Cerr << "CreateTable: " << tx << "\n";

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&out);
        return out;
    }

    static TString CreateInitShardTxBody(ui64 pathId, const std::vector<NArrow::NTest::TTestColumn>& columns,
        const std::vector<NArrow::NTest::TTestColumn>& pk, const TTableSpecials& specials = {}, const TString& ownerPath = "/Root/olap") {
        NKikimrTxColumnShard::TSchemaTxBody tx;
        auto* table = tx.MutableInitShard()->AddTables();
        tx.MutableInitShard()->SetOwnerPath(ownerPath);
        tx.MutableInitShard()->SetOwnerPathId(pathId);
        NColumnShard::TSchemeShardLocalPathId::FromRawValue(pathId).ToProto(*table);

        {   // preset
            auto* preset = table->MutableSchemaPreset();
            preset->SetId(1);
            preset->SetName("default");

            // schema
            InitSchema(columns, pk, specials, preset->MutableSchema());
        }

        InitTiersAndTtl(specials, table->MutableTtlSettings());

        Cerr << "CreateInitShard: " << tx << "\n";

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&out);
        return out;
    }

    static TString CreateStandaloneTableTxBody(ui64 pathId, const std::vector<NArrow::NTest::TTestColumn>& columns,
        const std::vector<NArrow::NTest::TTestColumn>& pk, const TTableSpecials& specials = {}, const TString& path = "/Root/olap") {
        NKikimrTxColumnShard::TSchemaTxBody tx;
        auto* table = tx.MutableInitShard()->AddTables();
        tx.MutableInitShard()->SetOwnerPath(path);
        tx.MutableInitShard()->SetOwnerPathId(pathId);
        NColumnShard::TSchemeShardLocalPathId::FromRawValue(pathId).ToProto(*table);

        InitSchema(columns, pk, specials, table->MutableSchema());
        InitTiersAndTtl(specials, table->MutableTtlSettings());

        Cerr << "CreateStandaloneTable: " << tx << "\n";

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&out);
        return out;
    }

    static TString AlterTableTxBody(ui64 pathId, ui32 version, const std::vector<NArrow::NTest::TTestColumn>& columns,
        const std::vector<NArrow::NTest::TTestColumn>& pk, const TTableSpecials& specials) {
        NKikimrTxColumnShard::TSchemaTxBody tx;
        auto* table = tx.MutableAlterTable();
        NColumnShard::TSchemeShardLocalPathId::FromRawValue(pathId).ToProto(*table);
        tx.MutableSeqNo()->SetRound(version);

        auto* preset = table->MutableSchemaPreset();
        preset->SetId(1);
        preset->SetName("default");
        InitSchema(columns, pk, specials, preset->MutableSchema());

        auto* ttlSettings = table->MutableTtlSettings();
        if (!InitTiersAndTtl(specials, ttlSettings)) {
            ttlSettings->MutableDisabled();
        }

        Cerr << "AlterTable: " << tx << "\n";

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&out);
        return out;
    }

    static TString DropTableTxBody(ui64 pathId, ui32 version) {
        NKikimrTxColumnShard::TSchemaTxBody tx;
        NColumnShard::TSchemeShardLocalPathId::FromRawValue(pathId).ToProto(*tx.MutableDropTable());
        tx.MutableSeqNo()->SetRound(version);

        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&out);
        return out;
    }

    static TString MoveTableTxBody(ui64 srcPathId, ui64 dstPathId, ui32 version) {
        NKikimrTxColumnShard::TSchemaTxBody tx;
        tx.MutableMoveTable()->SetSrcPathId(srcPathId);
        tx.MutableMoveTable()->SetDstPathId(dstPathId);
        tx.MutableSeqNo()->SetRound(version);
        TString out;
        Y_PROTOBUF_SUPPRESS_NODISCARD tx.SerializeToString(&out);
        return out;
    }

    static THashMap<TString, NColumnShard::NTiers::TTierConfig> BuildSnapshot(const TTableSpecials& specials);

    static TString CommitTxBody(ui64, const std::vector<ui64>& writeIds) {
        NKikimrTxColumnShard::TCommitTxBody proto;
        for (ui64 id : writeIds) {
            proto.AddWriteIds(id);
        }

        TString txBody;
        Y_PROTOBUF_SUPPRESS_NODISCARD proto.SerializeToString(&txBody);
        return txBody;
    }

    static std::vector<TString> ExtractNames(const std::vector<NArrow::NTest::TTestColumn>& columns) {
        std::vector<TString> out;
        out.reserve(columns.size());
        for (auto& col : columns) {
            out.push_back(col.GetName());
        }
        return out;
    }

    static std::vector<ui32> ExtractIds(const std::vector<NArrow::NTest::TTestColumn>& columns) {
        std::vector<ui32> out;
        out.reserve(columns.size());
        for (auto& col : columns) {
            Y_UNUSED(col);
            out.push_back(out.size() + 1);
        }
        return out;
    }

    static std::vector<NScheme::TTypeInfo> ExtractTypes(const std::vector<NArrow::NTest::TTestColumn>& columns) {
        std::vector<NScheme::TTypeInfo> types;
        types.reserve(columns.size());
        for (auto& i : columns) {
            types.push_back(i.GetType());
        }
        return types;
    }
};

void RefreshTiering(TTestBasicRuntime& runtime, const TActorId& sender);

void ProposeSchemaTxFail(TTestBasicRuntime& runtime, TActorId& sender, const TString& txBody, const ui64 txId);
[[nodiscard]] TPlanStep ProposeSchemaTx(TTestBasicRuntime& runtime, TActorId& sender, const TString& txBody, const ui64 txId);
void PlanSchemaTx(TTestBasicRuntime& runtime, const TActorId& sender, NOlap::TSnapshot snap);

void PlanWriteTx(TTestBasicRuntime& runtime, const TActorId& sender, NOlap::TSnapshot snap, bool waitResult = true);

bool WriteData(TTestBasicRuntime& runtime, TActorId& sender, const ui64 shardId, const ui64 writeId, const ui64 tableId, const TString& data,
    const std::vector<NArrow::NTest::TTestColumn>& ydbSchema, std::vector<ui64>* writeIds,
    const NEvWrite::EModificationType mType = NEvWrite::EModificationType::Upsert, const ui64 lockId = 1);

bool WriteData(TTestBasicRuntime& runtime, TActorId& sender, const ui64 writeId, const ui64 tableId, const TString& data,
    const std::vector<NArrow::NTest::TTestColumn>& ydbSchema, bool waitResult = true, std::vector<ui64>* writeIds = nullptr,
    const NEvWrite::EModificationType mType = NEvWrite::EModificationType::Upsert, const ui64 lockId = 1);

ui32 WaitWriteResult(TTestBasicRuntime& runtime, ui64 shardId, std::vector<ui64>* writeIds = nullptr);

void ScanIndexStats(TTestBasicRuntime& runtime, TActorId& sender, const std::vector<ui64>& pathIds, NOlap::TSnapshot snap, ui64 scanId = 0);

void ProposeCommitFail(
     TTestBasicRuntime& runtime, TActorId& sender, ui64 shardId, ui64 txId, const std::vector<ui64>& writeIds, const ui64 lockId = 1);
[[nodiscard]] TPlanStep ProposeCommit(
    TTestBasicRuntime& runtime, TActorId& sender, ui64 shardId, ui64 txId, const std::vector<ui64>& writeIds, const ui64 lockId = 1);
[[nodiscard]] TPlanStep ProposeCommit(TTestBasicRuntime& runtime, TActorId& sender, const ui64 txId, const std::vector<ui64>& writeIds, const ui64 lockId = 1);

void PlanCommit(TTestBasicRuntime& runtime, TActorId& sender, ui64 shardId, TPlanStep planStep, const TSet<ui64>& txIds);
void PlanCommit(TTestBasicRuntime& runtime, TActorId& sender, TPlanStep planStep, const TSet<ui64>& txIds);

inline void PlanCommit(TTestBasicRuntime& runtime, TActorId& sender, TPlanStep planStep, ui64 txId) {
    TSet<ui64> ids;
    ids.insert(txId);
    PlanCommit(runtime, sender, planStep, ids);
}

void Wakeup(TTestBasicRuntime& runtime, const TActorId& sender, const ui64 shardId);

struct TTestBlobOptions {
    THashSet<TString> NullColumns;
    THashSet<TString> SameValueColumns;
    ui32 SameValue = 42;
};

TCell MakeTestCell(const TTypeInfo& typeInfo, ui32 value, std::vector<TString>& mem);
TString MakeTestBlob(std::pair<ui64, ui64> range, const std::vector<NArrow::NTest::TTestColumn>& columns, const TTestBlobOptions& options = {},
    const std::set<std::string>& notNullColumns = {});
TSerializedTableRange MakeTestRange(
    std::pair<ui64, ui64> range, bool inclusiveFrom, bool inclusiveTo, const std::vector<NArrow::NTest::TTestColumn>& columns);

}   // namespace NKikimr::NTxUT

namespace NKikimr::NColumnShard {
class TTableUpdatesBuilder {
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> Builders;
    std::shared_ptr<arrow::Schema> Schema;
    ui32 RowsCount = 0;

public:
    class TRowBuilder {
        TTableUpdatesBuilder& Owner;
        YDB_READONLY(ui32, Index, 0);

    public:
        TRowBuilder(ui32 index, TTableUpdatesBuilder& owner)
            : Owner(owner)
            , Index(index) {
        }

        TRowBuilder Add(const char* data) {
            return Add<std::string>(data);
        }

        template <class TData>
        TRowBuilder Add(const TData& data) {
            Y_ABORT_UNLESS(Index < Owner.Builders.size());
            auto& builder = Owner.Builders[Index];
            auto type = builder->type();

            Y_ABORT_UNLESS(NArrow::SwitchType(type->id(), [&](const auto& t) {
                using TWrap = std::decay_t<decltype(t)>;
                using T = typename TWrap::T;
                using TBuilder = typename arrow::TypeTraits<typename TWrap::T>::BuilderType;

                AFL_NOTICE(NKikimrServices::TX_COLUMNSHARD)("T", typeid(T).name());

                auto& typedBuilder = static_cast<TBuilder&>(*builder);
                if constexpr (std::is_arithmetic<TData>::value) {
                    if constexpr (arrow::has_c_type<T>::value) {
                        using CType = typename T::c_type;
                        Y_ABORT_UNLESS(typedBuilder.Append((CType)data).ok());
                        return true;
                    }
                }
                if constexpr (std::is_same<TData, std::string>::value) {
                    if constexpr (arrow::has_string_view<T>::value && arrow::is_parameter_free_type<T>::value) {
                        Y_ABORT_UNLESS(typedBuilder.Append(data.data(), data.size()).ok());
                        return true;
                    }
                }

                if constexpr (std::is_same<TData, NYdb::TDecimalValue>::value) {
                    if constexpr (arrow::is_decimal128_type<T>::value) {
                        Y_ABORT_UNLESS(typedBuilder.Append(arrow::Decimal128(data.Hi_, data.Low_)).ok());
                        return true;
                    }
                }
                Y_ABORT("Unknown type combination");
                return false;
            }));
            return TRowBuilder(Index + 1, Owner);
        }

        TRowBuilder AddNull() {
            Y_ABORT_UNLESS(Index < Owner.Builders.size());
            auto res = Owner.Builders[Index]->AppendNull();
            return TRowBuilder(Index + 1, Owner);
        }
    };

    TTableUpdatesBuilder(std::shared_ptr<arrow::Schema> schema)
        : Schema(schema) {
        Builders = NArrow::MakeBuilders(schema);
        Y_ABORT_UNLESS(Builders.size() == schema->fields().size());
    }

    TTableUpdatesBuilder(arrow::Result<std::shared_ptr<arrow::Schema>> schema) {
        UNIT_ASSERT_C(schema.ok(), schema.status().ToString());
        Schema = schema.ValueUnsafe();
        Builders = NArrow::MakeBuilders(Schema);
        Y_ABORT_UNLESS(Builders.size() == Schema->fields().size());
    }

    TRowBuilder AddRow() {
        ++RowsCount;
        return TRowBuilder(0, *this);
    }

    std::shared_ptr<arrow::RecordBatch> BuildArrow() {
        TVector<std::shared_ptr<arrow::Array>> columns;
        columns.reserve(Builders.size());
        for (auto&& builder : Builders) {
            auto arrayDataRes = builder->Finish();
            Y_ABORT_UNLESS(arrayDataRes.ok());
            columns.push_back(*arrayDataRes);
        }
        return arrow::RecordBatch::Make(Schema, RowsCount, columns);
    }
};

NOlap::TIndexInfo BuildTableInfo(const std::vector<NArrow::NTest::TTestColumn>& ydbSchema, const std::vector<NArrow::NTest::TTestColumn>& key);

struct TestTableDescription {
    std::vector<NArrow::NTest::TTestColumn> Schema = NTxUT::TTestSchema::YdbSchema();
    std::vector<NArrow::NTest::TTestColumn> Pk = NTxUT::TTestSchema::YdbPkSchema();
    bool InStore = true;

    std::vector<ui32> GetColumnIds(const std::vector<TString>& names) const {
        return NTxUT::TTestSchema::GetColumnIds(Schema, names);
    }
};

[[nodiscard]] NTxUT::TPlanStep SetupSchema(TTestBasicRuntime& runtime, TActorId& sender, ui64 pathId, const TestTableDescription& table = {},
    TString codec = "none", const ui64 txId = 10);
[[nodiscard]] NTxUT::TPlanStep SetupSchema(TTestBasicRuntime& runtime, TActorId& sender, const TString& txBody, const ui64 txId);

[[nodiscard]] NTxUT::TPlanStep PrepareTablet(
    TTestBasicRuntime& runtime, const ui64 tableId, const std::vector<NArrow::NTest::TTestColumn>& schema, const ui32 keySize = 1);

std::shared_ptr<arrow::RecordBatch> ReadAllAsBatch(
    TTestBasicRuntime& runtime, const ui64 tableId, const NOlap::TSnapshot& snapshot, const std::vector<NArrow::NTest::TTestColumn>& schema);
}   // namespace NKikimr::NColumnShard
