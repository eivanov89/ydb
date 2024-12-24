#include "kqp_fast_query.h"

#include <library/cpp/json/json_reader.h>

#include <contrib/libs/libpg_query/pg_query.h>

#include <util/generic/hash.h>
#include <util/string/printf.h>

#include <regex>

namespace NKikimr::NKqp {

namespace {

const std::regex PragmaRegex(R"([ \t]*PRAGMA\s+TablePathPrefix\(\"([^\"]+)\"\);[ \t]*(\r?\n)?)", std::regex::icase);
const std::regex DeclareRegex(R"([ \t]*DECLARE\s+\$(\w+)\s+AS\s+(\w+);[ \t]*(\r?\n)?)", std::regex::icase);
const std::regex PlaceholderRegex(R"(\$(\w+))");

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

TFastQuery CompileToFastQuery(const TString& yqlQuery) {
    TFastQuery result;

    result.OriginalQueryHash = THash<TString>()(yqlQuery);

    result.PostgresQuery = YQL2Postgres(yqlQuery);
    if (result.PostgresQuery.Query.empty()) {
        return result;
    }

    PgQueryParseResult parseResult;
    parseResult = pg_query_parse(result.PostgresQuery.Query.c_str());
    NJson::TJsonValue jsonValue;
    try {
        NJson::ReadJsonTree(parseResult.parse_tree, &jsonValue, true);
        const auto& statements = jsonValue["stmts"].GetArray();
        if (statements.size() != 1) {
            return result;
        }
        const auto& statement = statements[0]["stmt"];
        if (!statement.Has("SelectStmt")) {
            return result;
        }

        const auto& selectStatement = statement["SelectStmt"];
        if (!selectStatement.Has("whereClause") ||
                !selectStatement.Has("fromClause") || !selectStatement.Has("targetList")) {
            return result;
        }

        if (selectStatement["op"] != "SETOP_NONE") {
            return result;
        }

        if (selectStatement.Has("sortClause")) {
            return result;
        }

        if (selectStatement["limitOption"] != "LIMIT_OPTION_DEFAULT") {
            return result;
        }

        // handle WHERE

        const auto& whereEpr = selectStatement["whereClause"]["BoolExpr"];
        if (!whereEpr.IsDefined()) {
            return result;
        }
        if (whereEpr["boolop"] != "AND_EXPR") {
            return result;
        }

        for (const auto& arg: whereEpr["args"].GetArray()) {
            if (arg["A_Expr"]["name"][0]["String"]["sval"] != "=") {
                return result;
            }
            TString colName = arg["A_Expr"]["lexpr"]["ColumnRef"]["fields"][0]["String"]["sval"].GetString();
            int colParamNum = arg["A_Expr"]["rexpr"]["ParamRef"]["number"].GetInteger();
            if (colParamNum == 0 || colName.empty()) {
                return result;
            }
            result.WhereColumnsWithPos.emplace_back(std::move(colName), colParamNum);
        }

        // handle FROM

        const auto& fromClause = selectStatement["fromClause"];
        if (!fromClause.IsDefined()) {
            return result;
        }

        const auto& fromArray = fromClause.GetArray();
        if (fromArray.size() != 1) {
            return result;
        }

        TString tableName = fromArray[0]["RangeVar"]["relname"].GetString();
        if (tableName.empty()) {
            return result;
        }
        result.TableName = tableName;

        // SELECT <WHAT>

        const auto& targets = selectStatement["targetList"].GetArray();
        if (targets.empty()) {
            return result;
        }

        for (const auto& target: targets) {
            const auto& fieldsArray = target["ResTarget"]["val"]["ColumnRef"]["fields"].GetArray();
            if (fieldsArray.size() != 1) {
                return result;
            }

            TString column = fieldsArray[0]["String"]["sval"].GetString();
            if (column.empty()) {
                return result;
            }
            result.ColumnsToSelect.emplace_back(std::move(column));
        }

        if (result.WhereColumnsWithPos.empty() || result.ColumnsToSelect.empty()) {
            return result;
        }

        result.ExecutionType = TFastQuery::EExecutionType::SELECT_QUERY;
        return result;
    } catch (...) {
        pg_query_free_parse_result(parseResult);
        return result;
    }
    pg_query_free_parse_result(parseResult);


    return result;
}

} // namespace NKikimr::NKqp
