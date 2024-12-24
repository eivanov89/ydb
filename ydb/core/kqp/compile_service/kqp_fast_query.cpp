#include "kqp_fast_query.h"

#include <library/cpp/json/json_reader.h>

#include <yql/essentials/parser/pg_wrapper/interface/raw_parser.h>

#include <util/generic/hash.h>
#include <util/string/printf.h>

#include <regex>

namespace NKikimr::NKqp {

namespace {

const std::regex PragmaRegex(R"([ \t]*PRAGMA\s+TablePathPrefix\(\"([^\"]+)\"\);[ \t]*(\r?\n)?)", std::regex::icase);
const std::regex DeclareRegex(R"([ \t]*DECLARE\s+\$(\w+)\s+AS\s+(\w+);[ \t]*(\r?\n)?)", std::regex::icase);
const std::regex PlaceholderRegex(R"(\$(\w+))");
const std::regex LimitRegex(R"(\bLIMIT\b)", std::regex::icase);

void ProcessPostgresTree(const List* raw, TFastQueryPtr& result) {
    NJson::TJsonValue jsonValue;
    try {
        TString pgJson = NYql::PrintPGTreeJson(raw);
        NJson::ReadJsonTree(pgJson, &jsonValue, true);
        const auto& statements = jsonValue["stmts"].GetArray();
        if (statements.size() != 1) {
            return;
        }
        const auto& statement = statements[0]["stmt"];
        if (!statement.Has("SelectStmt")) {
            return;
        }

        const auto& selectStatement = statement["SelectStmt"];
        if (!selectStatement.Has("whereClause") ||
                !selectStatement.Has("fromClause") || !selectStatement.Has("targetList")) {
            return;
        }

        if (selectStatement["op"] != "SETOP_NONE") {
            return;
        }

        if (selectStatement.Has("sortClause")) {
            return;
        }

        /* we handle it separately, becayse parser seems to be buggy
        if (selectStatement["limitOption"] != "LIMIT_OPTION_DEFAULT") {
            return;
        }
        */

        // handle WHERE

        const auto& whereEpr = selectStatement["whereClause"]["BoolExpr"];
        if (!whereEpr.IsDefined()) {
            return;
        }
        if (whereEpr["boolop"] != "AND_EXPR") {
            return;
        }

        for (const auto& arg: whereEpr["args"].GetArray()) {
            if (arg["A_Expr"]["name"][0]["String"]["sval"] != "=") {
                return;
            }
            TString colName = arg["A_Expr"]["lexpr"]["ColumnRef"]["fields"][0]["String"]["sval"].GetString();
            int colParamNum = arg["A_Expr"]["rexpr"]["ParamRef"]["number"].GetInteger();
            if (colParamNum == 0 || colName.empty()) {
                return;
            }
            result->WhereColumnsToPos.emplace(std::move(colName), colParamNum);
        }

        // handle FROM

        const auto& fromClause = selectStatement["fromClause"];
        if (!fromClause.IsDefined()) {
            return;
        }

        const auto& fromArray = fromClause.GetArray();
        if (fromArray.size() != 1) {
            return;
        }

        TString tableName = fromArray[0]["RangeVar"]["relname"].GetString();
        if (tableName.empty()) {
            return;
        }
        result->TableName = tableName;

        // SELECT <WHAT>

        const auto& targets = selectStatement["targetList"].GetArray();
        if (targets.empty()) {
            return;
        }

        for (const auto& target: targets) {
            const auto& fieldsArray = target["ResTarget"]["val"]["ColumnRef"]["fields"].GetArray();
            if (fieldsArray.size() != 1) {
                return;
            }

            TString column = fieldsArray[0]["String"]["sval"].GetString();
            if (column.empty()) {
                return;
            }
            result->ColumnsToSelect.emplace_back(std::move(column));
        }

        if (result->WhereColumnsToPos.empty() || result->ColumnsToSelect.empty()) {
            return;
        }

        result->ExecutionType = TFastQuery::EExecutionType::SELECT_QUERY;
        return;
    } catch (...) {
        //pg_query_free_parse_result(parseResult);
        return;
    }
    //pg_query_free_parse_result(parseResult);

    return;
}

class TEvents : public NYql::IPGParseEvents {
public:
    TEvents(TFastQueryPtr& fastQuery)
        : FastQuery(fastQuery)
    {
    }

    void OnResult(const List* raw) override {
        ProcessPostgresTree(raw, FastQuery);
    }

    void OnError(const NYql::TIssue& issue) override {
        Issue = issue;
    }

    TMaybe<NYql::TIssue> Issue;

    TFastQueryPtr& FastQuery;
};

} // anonymous

TString TFastQuery::ToString() const {
    TStringStream ss;
    ss << "FastQuery of type " << ExecutionType << " with postgres query '"
       << PostgresQuery.Query << ", and " << PostgresQuery.PositionalNames.size()
       << " params, in DB " << Database << " with id " << DatabaseId;
    return ss.Str();
}

TFastQueryPtr TFastQueryHelper::HandleYQL(const TString& yqlQuery) {
    if (auto it = Queries.find(yqlQuery); it != Queries.end()) {
        return it->second;
    }

    auto queryPtr = CompileToFastQuery(yqlQuery);
    Queries.emplace(yqlQuery, queryPtr);

    return queryPtr;
}

// very quick and very dirty
TPostgresQuery YQL2Postgres(const TString& yqlQuery) {
    TPostgresQuery result;
    result.Query.reserve(yqlQuery.size());

    std::smatch match;
    size_t currentPos = 0; // in yqlQuery

    // Capture and remove PRAGMA
    if (std::regex_search(yqlQuery.begin(), yqlQuery.end(), match, PragmaRegex)) {
        result.TablePathPrefix = std::string(match[1]);
        result.Query.append(yqlQuery, currentPos, match.position());
        currentPos = match.position() + match.length();
    }

    // remove parameter declarations
    while (currentPos < yqlQuery.size() &&
            std::regex_search(yqlQuery.begin() + currentPos, yqlQuery.end(), match, DeclareRegex)) {
        std::string variableName = match[1];
        std::string typeName = match[2];
        if (typeName != "Int32") {
            return {};
        }
        result.PositionalNames.emplace_back(variableName);

        result.Query.append(yqlQuery, currentPos, match.position());
        currentPos += match.position() + match.length();
    }

    while (currentPos < yqlQuery.size() &&
            std::regex_search(yqlQuery.begin() + currentPos, yqlQuery.end(), match, PlaceholderRegex)) {
        std::string variableName = match[1];
        auto it = std::find(result.PositionalNames.begin(), result.PositionalNames.end(), variableName);
        if (it != result.PositionalNames.end()) {
            int positionalIndex = std::distance(result.PositionalNames.begin(), it) + 1;
            result.Query.append(yqlQuery, currentPos, match.position());
            result.Query += "$" + std::to_string(positionalIndex);
            currentPos += match.position() + match.length();
        } else {
            return {};
        }
    }

    if (currentPos < yqlQuery.size()) {
        result.Query.append(yqlQuery, currentPos, yqlQuery.size() - currentPos);
    }

    return result;
}

TFastQueryPtr CompileToFastQuery(const TString& yqlQuery) {
    TFastQueryPtr result = std::make_shared<TFastQuery>();

    result->OriginalQueryHash = THash<TString>()(yqlQuery);

    // for some dumb reason our parser returns "limitOption": "LIMIT_OPTION_COUNT", when there is
    // no limit. So we have to check it here manually
    if (std::regex_search(yqlQuery.c_str(), LimitRegex)) {
        return result;
    }

    result->PostgresQuery = YQL2Postgres(yqlQuery);
    if (result->PostgresQuery.Query.empty()) {
        return result;
    }

    TEvents events(result);
    NYql::PGParse(result->PostgresQuery.Query, events);
    return result;
}

} // namespace NKikimr::NKqp
