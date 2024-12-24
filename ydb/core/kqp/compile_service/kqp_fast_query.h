#pragma once

#include <util/generic/string.h>

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

    TString TableName;
    std::vector<TString> ColumnsToSelect;
    std::vector<std::pair<TString, size_t>> WhereColumnsWithPos;

    TPostgresQuery PostgresQuery;
};

// Tries to transform from YQL to Postgres syntax
TPostgresQuery YQL2Postgres(const TString& yqlQuery);

TFastQuery CompileToFastQuery(const TString& yqlQuery);

} // namespace NKikimr::NKqp
