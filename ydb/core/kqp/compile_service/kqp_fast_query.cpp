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

        Cerr << (TStringBuilder() << Endl << "pg query: " << pgJson << Endl);

        const auto& whereExpr = selectStatement["whereClause"];
        if (!whereExpr.IsDefined()) {
            return;
        }

        bool isSelectIn = false;

        if (const auto& boolExpr = whereExpr["BoolExpr"]; boolExpr.IsDefined()) {
            if (boolExpr["boolop"] != "AND_EXPR") {
                return;
            }

            for (const auto& arg: boolExpr["args"].GetArray()) {
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
        } else if (const auto& aexpr = whereExpr["A_Expr"]; aexpr.IsDefined()) {
            if (aexpr["kind"] == "AEXPR_OP") {
                if (aexpr["name"][0]["String"]["sval"] != "=") {
                    return;
                }
                TString colName = aexpr["lexpr"]["ColumnRef"]["fields"][0]["String"]["sval"].GetString();
                int colParamNum = aexpr["rexpr"]["ParamRef"]["number"].GetInteger();
                if (colParamNum == 0 || colName.empty()) {
                    return;
                }
                result->WhereColumnsToPos.emplace(std::move(colName), colParamNum);
            } else if (aexpr["kind"] == "AEXPR_IN") {
                isSelectIn = true;

                if (const auto& columnRefFields = aexpr["lexpr"]["ColumnRef"]["fields"]; columnRefFields.IsDefined()) {
                    // WHERE COL IN (1, 2, 3)
                    for (const auto& field: columnRefFields.GetArray()) {
                        const auto& column = field["String"]["sval"];
                        if (!column.IsDefined()) {
                            return;
                        }
                        result->WhereSelectInColumns.emplace_back(column.GetString());
                    }

                    if (result->WhereSelectInColumns.empty()) {
                        return;
                    }

                    const auto& posArgs = aexpr["rexpr"]["List"]["items"];
                    for (const auto& pos: posArgs.GetArray()) {
                        result->WhereSelectInPos.push_back({});
                        auto& posVector = result->WhereSelectInPos.back();
                        const auto& posValue = pos["ParamRef"]["number"];
                        if (!posValue.IsDefined()) {
                            return;
                        }
                        posVector.push_back(posValue.GetInteger());
                    }
                } else if (const auto& rowRefArgs = aexpr["lexpr"]["RowExpr"]["args"]; rowRefArgs.IsDefined()) {
                    // WHERE (COL1, COL2) IN ((1,1), (2,2), )
                    for (const auto& arg: rowRefArgs.GetArray()) {
                        const auto& column = arg["ColumnRef"]["fields"][0]["String"]["sval"];
                        if (!column.IsDefined()) {
                            return;
                        }
                        result->WhereSelectInColumns.emplace_back(column.GetString());
                    }
                    if (result->WhereSelectInColumns.empty()) {
                        return;
                    }

                    const auto& posArgs = aexpr["rexpr"]["List"]["items"];
                    if (!posArgs.IsDefined()) {
                        return;
                    }
                    for (const auto& pos: posArgs.GetArray()) {
                        result->WhereSelectInPos.push_back({});
                        auto& posVector = result->WhereSelectInPos.back();

                        const auto& columns = pos["RowExpr"]["args"];
                        if (!columns.IsDefined()) {
                            return;
                        }
                        for (const auto& column: columns.GetArray()) {
                            const auto& colPosition = column["ParamRef"]["number"];
                            if (!colPosition.IsDefined()) {
                                return;
                            }
                            posVector.emplace_back(colPosition.GetInteger());
                        }
                    }
                } else {
                    return;
                }
            } else {
                return;
            }
        } else {
            return;
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

        bool withStar = false;

        for (const auto& target: targets) {
            const auto& fieldsArray = target["ResTarget"]["val"]["ColumnRef"]["fields"].GetArray();
            if (fieldsArray.size() != 1) {
                return;
            }

            if (fieldsArray[0].GetMap().contains("A_Star")) {
                withStar = true;
                break;
            }

            TString column = fieldsArray[0]["String"]["sval"].GetString();
            if (column.empty()) {
                return;
            }
            result->ColumnsToSelect.emplace_back(std::move(column));
        }

        if (result->WhereColumnsToPos.empty() && result->WhereSelectInColumns.empty()) {
            return;
        }

        if (result->ColumnsToSelect.empty() && !withStar) {
            return;
        }

        if (isSelectIn) {
            result->ExecutionType = TFastQuery::EExecutionType::SELECT_IN_QUERY;
        } else {
            result->ExecutionType = TFastQuery::EExecutionType::SELECT_QUERY;
        }
        return;
    } catch (std::exception ex) {
        // pg_query_free_parse_result(parseResult);
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
        //Cerr << (TStringBuilder() << "TEvents::OnError: " << issue.GetMessage() << Endl);
        Issue = issue;
    }

    TMaybe<NYql::TIssue> Issue;

    TFastQueryPtr& FastQuery;
};

/*
TFastQuery::EParamType GetParamType(const std::string& typeStr) {
    static const std::unordered_map<std::string, TFastQuery::EParamType> typeMap = {
        {"Int32", TFastQuery::EParamType::INT32},
        {"Int64", TFastQuery::EParamType::INT64},
        {"Double", TFastQuery::EParamType::DOUBLE},
        {"Timestamp", TFastQuery::EParamType::TIMESTAMP},
        {"Text", TFastQuery::EParamType::TEXT},
        {"Utf8", TFastQuery::EParamType::TEXT}
    };

    auto it = typeMap.find(typeStr);
    return (it != typeMap.end()) ? it->second : TFastQuery::EParamType::UNKNOWN;
}

void TryCompileSelect1(const TString& yqlQuery, TFastQueryPtr& result) {
    // XXX avoid multiline issues
    TString query;
    query.reserve(yqlQuery.size());
    for (auto ch: yqlQuery) {
        if (ch != '\n') {
            query.append(ch);
        } else {
            query.append(' ');
        }
    }

    const std::regex select1Pattern(R"(\s*SELECT\s+1;\s*)", std::regex::icase);
    if (std::regex_search(query.begin(), query.end(), select1Pattern)) {
        result->ExecutionType = TFastQuery::EExecutionType::SELECT1;
    }

    return;
}


void TryCompileYCSBSelect(const TString& yqlQuery, TFastQueryPtr& result) {
    // XXX avoid multiline issues
    TString query;
    query.reserve(yqlQuery.size());
    for (auto ch: yqlQuery) {
        if (ch != '\n') {
            query.append(ch);
        } else {
            query.append(' ');
        }
    }

    const std::regex selectPattern(R"(; SELECT \* FROM usertable WHERE id)", std::regex::icase);
    if (std::regex_search(query.begin(), query.end(), selectPattern)) {
        result->ExecutionType = TFastQuery::EExecutionType::SELECT_QUERY;
        result->Database = "/Root";
        result->TableName = "usertable";
        result->ColumnsToSelect = {
            "id",
            "field0",
            "field1",
            "field2",
            "field3",
            "field4",
            "field5",
            "field6",
            "field7",
            "field8",
            "field9",
        };
        result->WhereColumnsToPos["id"] = 1;
        result->PostgresQuery.PositionalNames.emplace_back("key");
    }

    return;
}
*/

/*
void TryCompileUpsert(const TString& yqlQuery, TFastQueryPtr& result) {
    // Only upserts like this one for now:
    //  DECLARE $batch AS List<Struct<p1:Int32?, p2:Int32, p3:Int32, p4:Int32>>;
    //  UPSERT INTO `oorder` SELECT p1 AS `O_CARRIER_ID`, p2 AS `O_ID`, p3 AS `O_D_ID`, p4 AS `O_W_ID`
    //         FROM AS_TABLE($batch);
    //

    // XXX avoid multiline issues
    TString query;
    query.reserve(yqlQuery.size());
    for (auto ch: yqlQuery) {
        if (ch != '\n') {
            query.append(ch);
        } else {
            query.append(' ');
        }
    }

    const std::regex upsertPattern(R"(DECLARE\s+\$batch\s+AS\s+List.*?;[ \t]*UPSERT\s+INTO\s+`?([\w\d_]+)`?\s+SELECT\s+.*?\s+FROM\s+AS_TABLE\(\s*\$batch\s*\))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(query.begin(), query.end(), match, upsertPattern)) {
        result->TableName = match[1].str();
    } else {
        return;
    }

    std::string::const_iterator searchStart(query.cbegin());

    // find param types
    const std::regex declarePattern(R"(\s*p(\d+):\s*([A-Za-z0-9?]+))", std::regex::icase);
    std::unordered_map<TString, TFastQuery::EParamType> paramTypeMap;
    std::unordered_set<TString> optionalSet;
    while (std::regex_search(searchStart, query.cend(), match, declarePattern)) {
        if (match.size() == 3) {
            TString paramName = "p" + match[1].str();  // "p1", "p2", etc.
            std::string typeStr = match[2].str();          // "Int32", "Double?", etc.

            if (typeStr.back() == '?') {
                typeStr.pop_back();
                optionalSet.insert(paramName);
            }

            paramTypeMap[paramName] = GetParamType(typeStr);
        }
        searchStart = match.suffix().first;
    }

    // params and columns
    const std::regex columnPattern(R"(\s*([\w\d_]+)\s+AS\s+`?([\w\d_]+)`?)", std::regex::icase);
    while (std::regex_search(searchStart, query.cend(), match, columnPattern)) {
        if (match.size() == 3) {
            TString paramName = match[1].str();   // "p1", "p2", etc.
            TString columnName = match[2].str();  // "C_W_ID", "C_D_ID", etc.

            result->UpsertParams.push_back({
                paramName,
                paramTypeMap[paramName],
                columnName,
                optionalSet.find(paramName) != optionalSet.end()
            });
            result->ColumnsToUpsert.push_back(columnName);
        }
        searchStart = match.suffix().first;
    }

    if (!result->UpsertParams.empty() && !result->ColumnsToUpsert.empty()) {
        result->ExecutionType = TFastQuery::EExecutionType::UPSERT;
    }
}
*/
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
        if (typeName != "Int32" && typeName != "Text") {
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

    result->OriginalQuery = yqlQuery;
    result->OriginalQueryHash = THash<TString>()(yqlQuery);

    // for some dumb reason our parser returns "limitOption": "LIMIT_OPTION_COUNT", when there is
    // no limit. So we have to check it here manually
    if (std::regex_search(yqlQuery.c_str(), LimitRegex)) {
        return result;
    }

/*
    TryCompileSelect1(yqlQuery, result);
    if (result->ExecutionType == TFastQuery::EExecutionType::SELECT1) {
        return result;
    }

    // check if this is upsert
    TryCompileUpsert(yqlQuery, result);
    if (result->ExecutionType != TFastQuery::EExecutionType::UNSUPPORTED) {
        return result;
    }
*/

    result->PostgresQuery = YQL2Postgres(yqlQuery);
    if (result->PostgresQuery.Query.empty()) {
        return result;
    }

    TEvents events(result);
    NYql::PGParse(result->PostgresQuery.Query, events);
    return result;
}

} // namespace NKikimr::NKqp
