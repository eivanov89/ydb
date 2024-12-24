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
        SELECT_QUERY,
    };

    EExecutionType ExecutionType = EExecutionType::UNSUPPORTED;
    size_t OriginalQueryHash = 0;

    TString Database;
    TString DatabaseId;

    // parsed items
    TString TableName;
    std::vector<TString> ColumnsToSelect;

    THashMap<TString, size_t> WhereColumnsToPos;

    // resolved items
    THashMap<TString, TSysTables::TTableColumnInfo> ResolvedColumns;
    TVector<TKeyDesc::TColumnOp> Columns;
    TVector<TSysTables::TTableColumnInfo> KeyColumns;
    TVector<NScheme::TTypeInfo> KeyColumnTypes;
    TTableId TableId = {};

    TPostgresQuery PostgresQuery;

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
