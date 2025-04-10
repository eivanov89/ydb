#pragma once

#include <ydb/core/scheme/scheme_tabledefs.h>
#include <ydb/core/tx/locks/sys_tables.h>

#include <ydb/library/actors/core/actor.h>

#include <util/generic/hash.h>
#include <util/generic/string.h>

#include <memory>
#include <vector>

namespace NKikimr::NKqp {

// index corresponds to the $1, $2 and YDB's parameter name
using TPositionalNames = std::vector<TString>;

struct TPostgresQuery {
    TString Query;
    TPositionalNames PositionalNames;
    TString TablePathPrefix;
};

struct TFastQuery {
    enum class EExecutionType {
        UNSUPPORTED = 0,
        SELECT1,
        SELECT_QUERY,
        SELECT_IN_QUERY,
        UPSERT,
    };

    EExecutionType ExecutionType = EExecutionType::UNSUPPORTED;
    TString OriginalQuery;
    size_t OriginalQueryHash = 0;

    TString Database;
    TString DatabaseId;

    // parsed items for SELECT

    TString TableName;
    std::vector<TString> ColumnsToSelect;

    THashMap<TString, size_t> WhereColumnsToPos;

    // parsed items for SELECT IN

    TVector<TString> WhereSelectInColumns; // column names
    TVector<TVector<size_t>> WhereSelectInPos; // points

    // resolved items

    THashMap<TString, TSysTables::TTableColumnInfo> ResolvedColumns;
    TVector<TKeyDesc::TColumnOp> Columns;
    TVector<TSysTables::TTableColumnInfo> KeyColumns;
    TVector<NScheme::TTypeInfo> KeyColumnTypes;
    TTableId TableId = {};

    bool NeedToResolveIndex = false;
    struct TResolvedIndex {
        TTableId TableId;
        TVector<TKeyDesc::TColumnOp> Columns;
        TVector<TKeyDesc::TColumnOp> ColumnInfos;
        TVector<NScheme::TTypeInfo> ColumnTypes;

        // indices in ColumnsToUpsert and UpsertParams
        TVector<size_t> OrderedColumnParams;
    };
    std::optional<TResolvedIndex> ResolvedIndex;

    TVector<size_t> OrderedWhereSelectInColumns;

    TPostgresQuery PostgresQuery;

    // upsert stuff

    enum class EParamType {
        UNKNOWN = 0,
        INT32,
        INT64,
        DOUBLE,
        TIMESTAMP,
        TEXT, // either Utf8 or Text
    };

    struct TUpsertParam {
        TString Name;
        EParamType Type;
        TString ColumnName;
        bool IsOptional = false;

        bool operator==(const TUpsertParam& other) const {
            return Name == other.Name && Type == other.Type
                && ColumnName == other.ColumnName && IsOptional == other.IsOptional;
        }
    };

    // original order
    TVector<TString> ColumnsToUpsert;
    TVector<TUpsertParam> UpsertParams;

    // e.g. indices for p5, p1, p2 - index within ColumnsToUpsert and UpsertParams
    TVector<size_t> OrderedKeyParams;
    TVector<size_t> AllOrderedParams;

    TString ToString() const;
};

using TFastQueryPtr = std::shared_ptr<TFastQuery>;

class TFastQueryHelper {
public:
    TFastQueryHelper() = default;

    TFastQueryPtr HandleYQL(const TString& yqlQuery);

private:
    // bost fast and normal
    THashMap<TString, TFastQueryPtr> Queries;
};

// Tries to transform from YQL to Postgres syntax
TPostgresQuery YQL2Postgres(const TString& yqlQuery);

TFastQueryPtr CompileToFastQuery(const TString& yqlQuery);

} // namespace NKikimr::NKqp
