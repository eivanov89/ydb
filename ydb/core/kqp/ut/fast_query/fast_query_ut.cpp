#include <ydb/core/kqp/compile_service/kqp_fast_query.h>

#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/testlib/common_helper.h>
#include <ydb/core/testlib/test_client.h>

#include <library/cpp/testing/unittest/tests_data.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {
} // anonymous

Y_UNIT_TEST_SUITE(KqpFastQueryHelpers) {

Y_UNIT_TEST(ShouldTransformToPostgresSyntax1) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto result = YQL2Postgres(query);
    UNIT_ASSERT(!result.Query.empty());
    UNIT_ASSERT_EQUAL(result.PositionalNames.size(), 3);
    UNIT_ASSERT_EQUAL(result.PositionalNames[0], "c_w_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[1], "c_d_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[2], "c_id");
    UNIT_ASSERT(result.TablePathPrefix.empty());

    TString expectedPostgresQuery = R"(
        --!syntax_v1


        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $1
           AND C_D_ID = $2
           AND C_ID = $3;
    )";

    UNIT_ASSERT_EQUAL(result.Query, expectedPostgresQuery);
}

Y_UNIT_TEST(ShouldTransformToPostgresSyntax2) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32; DECLARE $c_d_id AS Int32;DECLARE $c_id AS Int32;SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto result = YQL2Postgres(query);
    UNIT_ASSERT(!result.Query.empty());
    UNIT_ASSERT_EQUAL(result.PositionalNames.size(), 3);
    UNIT_ASSERT_EQUAL(result.PositionalNames[0], "c_w_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[1], "c_d_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[2], "c_id");
    UNIT_ASSERT(result.TablePathPrefix.empty());

    TString expectedPostgresQuery = R"(
        --!syntax_v1

SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $1
           AND C_D_ID = $2
           AND C_ID = $3;
    )";

    UNIT_ASSERT_EQUAL(result.Query, expectedPostgresQuery);
}

Y_UNIT_TEST(ShouldTransformToPostgresSyntax3) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id

        ORDER BY C_DISCOUNT;
    )";

    auto result = YQL2Postgres(query);
    UNIT_ASSERT(!result.Query.empty());
    UNIT_ASSERT_EQUAL(result.PositionalNames.size(), 3);
    UNIT_ASSERT_EQUAL(result.PositionalNames[0], "c_w_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[1], "c_d_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[2], "c_id");
    UNIT_ASSERT(result.TablePathPrefix.empty());

    TString expectedPostgresQuery = R"(
        --!syntax_v1


        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $1
           AND C_D_ID = $2
           AND C_ID = $3

        ORDER BY C_DISCOUNT;
    )";

    UNIT_ASSERT_EQUAL(result.Query, expectedPostgresQuery);
}

Y_UNIT_TEST(ShouldTransformToPostgresSyntax4) {
    // same as number 3, but different order of parameters
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_ID = $c_id
           AND C_D_ID = $c_d_id
           AND C_W_ID = $c_w_id

        ORDER BY C_DISCOUNT;
    )";

    auto result = YQL2Postgres(query);
    UNIT_ASSERT(!result.Query.empty());
    UNIT_ASSERT_EQUAL(result.PositionalNames.size(), 3);
    UNIT_ASSERT_EQUAL(result.PositionalNames[0], "c_w_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[1], "c_d_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[2], "c_id");
    UNIT_ASSERT(result.TablePathPrefix.empty());

    TString expectedPostgresQuery = R"(
        --!syntax_v1


        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_ID = $3
           AND C_D_ID = $2
           AND C_W_ID = $1

        ORDER BY C_DISCOUNT;
    )";

    UNIT_ASSERT_EQUAL(result.Query, expectedPostgresQuery);
}

Y_UNIT_TEST(ShouldTransformToPostgresSyntaxPragma1) {
    TString query = R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("/Root/db1");

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto result = YQL2Postgres(query);
    UNIT_ASSERT(!result.Query.empty());
    UNIT_ASSERT_EQUAL(result.PositionalNames.size(), 3);
    UNIT_ASSERT_EQUAL(result.PositionalNames[0], "c_w_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[1], "c_d_id");
    UNIT_ASSERT_EQUAL(result.PositionalNames[2], "c_id");
    UNIT_ASSERT_EQUAL(result.TablePathPrefix, "/Root/db1");

    TString expectedPostgresQuery = R"(
        --!syntax_v1



        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $1
           AND C_D_ID = $2
           AND C_ID = $3;
    )";

    UNIT_ASSERT_EQUAL(result.Query, expectedPostgresQuery);
}

Y_UNIT_TEST(FastUpsertBasic) {
    TString query = R"(
        PRAGMA TablePathPrefix("/Root/db1");

        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    size_t goldHash = THash<TString>()(query);

    std::vector<TString> columnsToUpsert = {
        "C_W_ID", "C_D_ID", "C_ID", "C_BALANCE", "C_YTD_PAYMENT", "C_PAYMENT_CNT"
    };

    std::vector<TFastQuery::TUpsertParam> upsertParams = {
        { "p1", TFastQuery::EParamType::INT32, "C_W_ID"},
        { "p2", TFastQuery::EParamType::INT32, "C_D_ID"},
        { "p3", TFastQuery::EParamType::INT32, "C_ID"},
        { "p4", TFastQuery::EParamType::DOUBLE, "C_BALANCE", true},
        { "p5", TFastQuery::EParamType::DOUBLE, "C_YTD_PAYMENT", true},
        { "p6", TFastQuery::EParamType::INT32, "C_PAYMENT_CNT", true},
    };

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->OriginalQueryHash, goldHash);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UPSERT);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->TableName, "customer");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ColumnsToUpsert, columnsToUpsert);
    UNIT_ASSERT(fastQuery->UpsertParams == upsertParams);
}

Y_UNIT_TEST(FastSelectBasic) {
    TString query = R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("/Root/db1");

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    size_t goldHash = THash<TString>()(query);

    std::vector<TString> columnsToSelect = {
        "C_DISCOUNT", "C_LAST", "C_CREDIT"
    };

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->OriginalQueryHash, goldHash);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::SELECT_QUERY);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->PostgresQuery.TablePathPrefix, "/Root/db1");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->TableName, "customer");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ColumnsToSelect, columnsToSelect);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereColumnsToPos.size(), 3UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereColumnsToPos["C_W_ID"], 1);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereColumnsToPos["C_D_ID"], 2);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereColumnsToPos["C_ID"], 3);
}

Y_UNIT_TEST(FastSelectBasic2) {
    TString query = R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("/Root/db1");

        DECLARE $w_id AS Int32;

        SELECT W_TAX
        FROM warehouse
        WHERE W_ID = $w_id;
    )";

    size_t goldHash = THash<TString>()(query);

    std::vector<TString> columnsToSelect = {
        "W_TAX",
    };

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->OriginalQueryHash, goldHash);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::SELECT_QUERY);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->PostgresQuery.TablePathPrefix, "/Root/db1");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->TableName, "warehouse");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ColumnsToSelect, columnsToSelect);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereColumnsToPos.size(), 1UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereColumnsToPos["W_ID"], 1);
}

Y_UNIT_TEST(FastSelectInBasic1) {
    TString query = R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("/Root/db1");

        DECLARE $w_id1 AS Int32;
        DECLARE $w_id2 AS Int32;
        DECLARE $w_id3 AS Int32;

        SELECT W_TAX
        FROM warehouse
        WHERE W_ID IN ($w_id1, $w_id2, $w_id3);
    )";

    size_t goldHash = THash<TString>()(query);

    std::vector<TString> columnsToSelect = {
        "W_TAX",
    };

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->OriginalQueryHash, goldHash);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::SELECT_IN_QUERY);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->PostgresQuery.TablePathPrefix, "/Root/db1");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->TableName, "warehouse");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ColumnsToSelect, columnsToSelect);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInColumns.size(), 1UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInColumns[0], "W_ID");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos.size(), 3UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[0][0], 1UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[1][0], 2UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[2][0], 3UL);
}

Y_UNIT_TEST(FastSelectInBasic2) {
    TString query = R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("/Root/db1");

        DECLARE $w_id1 AS Int32;
        DECLARE $d_id1 AS Int32;

        DECLARE $w_id2 AS Int32;
        DECLARE $d_id2 AS Int32;

        DECLARE $w_id3 AS Int32;
        DECLARE $d_id3 AS Int32;

        SELECT W_TAX
        FROM warehouse
        WHERE (W_ID, D_ID) IN (($w_id1, $d_id1), ($w_id2, $d_id2), ($w_id3, $d_id3));
    )";

    size_t goldHash = THash<TString>()(query);

    std::vector<TString> columnsToSelect = {
        "W_TAX",
    };

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->OriginalQueryHash, goldHash);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::SELECT_IN_QUERY);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->PostgresQuery.TablePathPrefix, "/Root/db1");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->TableName, "warehouse");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ColumnsToSelect, columnsToSelect);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInColumns.size(), 2UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInColumns[0], "W_ID");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInColumns[1], "D_ID");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos.size(), 3UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[0].size(), 2UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[0][0], 1UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[0][1], 2UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[1][0], 3UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[1][1], 4UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[2][0], 5UL);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->WhereSelectInPos[2][1], 6UL);
}

Y_UNIT_TEST(ShouldNotNonInt) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Timestamp;
        DECLARE $c_d_id AS Timestamp;
        DECLARE $c_id AS Timestamp;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

Y_UNIT_TEST(ShouldNotFastOrderBy) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id

        ORDER BY C_DISCOUNT;
    )";

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

Y_UNIT_TEST(ShouldNotFastLimit) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id

        LIMIT 1;
    )";

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

Y_UNIT_TEST(ShouldNotFastNotEqualWhere) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID != $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id

        LIMIT 1;
    )";

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

Y_UNIT_TEST(ShouldNotFastOrWhere) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           OR C_D_ID = $c_d_id
           OR C_ID = $c_id

        LIMIT 1;
    )";

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

Y_UNIT_TEST(ShouldNotQueriesWithIndex) {
    TString query = R"(
        --!syntax_v1

        SELECT C_FIRST, C_MIDDLE, C_ID, C_STREET_1, C_STREET_2, C_CITY,
               C_STATE, C_ZIP, C_PHONE, C_CREDIT, C_CREDIT_LIM, C_DISCOUNT,
               C_BALANCE, C_YTD_PAYMENT, C_PAYMENT_CNT, C_SINCE
          FROM  %s VIEW idx_customer_name AS idx
         WHERE idx.C_W_ID = $1
           AND idx.C_D_ID = $2
           AND idx.C_LAST = $3
         ORDER BY idx.C_FIRST;
    )";

    TFastQueryPtr fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery->ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}
}

Y_UNIT_TEST_SUITE(KqpFastQueryExecution) {

Y_UNIT_TEST(ShouldSelect) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 1, 0, "credit1", "last1"),
            (13, 11, 17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(13).Build()
        .AddParam("$c_d_id").Int32(11).Build()
        .AddParam("$c_id").Int32(17).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 7.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectKeyPrefix) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 1, 0, "credit1", "last1"),
            (1, 1, 2, 123, "credit1-2", "last1-2"),
            (13, 11, 17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id;
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(1).Build()
        .AddParam("$c_d_id").Int32(1).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 0.0);

    UNIT_ASSERT(resultParser.TryNextRow());
    auto discount2 = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount2, 123.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectWithInSingleArg) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 0, "credit1", "last1"),
            (2, 123, "credit1-2", "last1-2"),
            (17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_id1 AS Int32;
        DECLARE $c_id2 AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_ID IN ($c_id1, $c_id2);
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_id1").Int32(1).Build()
        .AddParam("$c_id2").Int32(17).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 0.0);

    UNIT_ASSERT(resultParser.TryNextRow());
    auto discount2 = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount2, 7.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectWithInMultiArg) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 3, "credit1", "last1"),
            (1, 2, 123, "credit1-2", "last1-2"),
            (2, 1, 71, "credit21", "last21"),
            (2, 17, 7, "credit2", "last2"),
            (2, 11, 222, "c", "l"),
            (3, 1, 111, "c", "l");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id1 AS Int32;
        DECLARE $c_id1 AS Int32;

        DECLARE $c_w_id2 AS Int32;
        DECLARE $c_id2 AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE (C_W_ID, C_ID) IN (($c_w_id1, $c_id1), ($c_w_id2, $c_id2));
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id1").Int32(1).Build()
        .AddParam("$c_id1").Int32(1).Build()
        .AddParam("$c_w_id2").Int32(2).Build()
        .AddParam("$c_id2").Int32(17).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 3.0);

    UNIT_ASSERT(resultParser.TryNextRow());
    auto discount2 = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount2, 7.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectWithInMultiArgWithDuplicates) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 3, "credit1", "last1"),
            (1, 2, 123, "credit1-2", "last1-2"),
            (2, 1, 71, "credit21", "last21"),
            (2, 17, 7, "credit2", "last2"),
            (2, 11, 222, "c", "l"),
            (3, 1, 111, "c", "l");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id1 AS Int32;
        DECLARE $c_id1 AS Int32;

        DECLARE $c_w_id2 AS Int32;
        DECLARE $c_id2 AS Int32;

        DECLARE $c_w_id3 AS Int32;
        DECLARE $c_id3 AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE (C_W_ID, C_ID) IN (($c_w_id1, $c_id1), ($c_w_id2, $c_id2), ($c_w_id3, $c_id3));
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id1").Int32(1).Build()
        .AddParam("$c_id1").Int32(1).Build()
        .AddParam("$c_w_id2").Int32(2).Build()
        .AddParam("$c_id2").Int32(17).Build()
        .AddParam("$c_w_id3").Int32(1).Build()
        .AddParam("$c_id3").Int32(1).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 3.0);

    UNIT_ASSERT(resultParser.TryNextRow());
    auto discount2 = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount2, 7.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectWithInMultiArgReversedWhere) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 3, "credit1", "last1"),
            (1, 2, 123, "credit1-2", "last1-2"),
            (2, 1, 71, "credit21", "last21"),
            (2, 17, 7, "credit2", "last2"),
            (2, 11, 222, "c", "l"),
            (3, 1, 111, "c", "l");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id1 AS Int32;
        DECLARE $c_id1 AS Int32;

        DECLARE $c_w_id2 AS Int32;
        DECLARE $c_id2 AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE (C_ID, C_W_ID) IN (($c_id1, $c_w_id1), ($c_id2, $c_w_id2));
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id1").Int32(1).Build()
        .AddParam("$c_id1").Int32(1).Build()
        .AddParam("$c_w_id2").Int32(2).Build()
        .AddParam("$c_id2").Int32(17).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 3.0);

    UNIT_ASSERT(resultParser.TryNextRow());
    auto discount2 = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount2, 7.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectWhenCustomParamNames) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 1, 0, "credit1", "last1"),
            (13, 11, 17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_d_id_var AS Int32;
        DECLARE $c_w_id_var AS Int32;
        DECLARE $c_id_var AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id_var
           AND C_ID = $c_id_var
           AND C_D_ID = $c_d_id_var;
    )";

    auto txControl = TTxControl::BeginTx().CommitTx();

    auto params = session.GetParamsBuilder()
        .AddParam("$c_id_var").Int32(17).Build()
        .AddParam("$c_w_id_var").Int32(13).Build()
        .AddParam("$c_d_id_var").Int32(11).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 7.0);

    UNIT_ASSERT(!resultParser.TryNextRow());
}

Y_UNIT_TEST(ShouldSelectInTransaction) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 1, 3, "credit1", "last1"),
            (13, 11, 17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto txControl = TTxControl::BeginTx(TTxSettings::SerializableRW());

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(13).Build()
        .AddParam("$c_d_id").Int32(11).Build()
        .AddParam("$c_id").Int32(17).Build()
        .Build();

    auto result = session.ExecuteDataQuery(query, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result.GetResultSets().size(), 1);

    TResultSetParser resultParser(result.GetResultSet(0));
    UNIT_ASSERT(resultParser.TryNextRow());

    auto discount = *resultParser.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 7.0);

    UNIT_ASSERT(result.GetTransaction());

    auto tx = *result.GetTransaction();
    params = session.GetParamsBuilder()
            .AddParam("$c_w_id").Int32(1).Build()
            .AddParam("$c_d_id").Int32(1).Build()
            .AddParam("$c_id").Int32(1).Build()
            .Build();

    auto result2 = session.ExecuteDataQuery(query, TTxControl::Tx(tx), params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result2.GetResultSets().size(), 1);

    TResultSetParser resultParser2(result2.GetResultSet(0));
    UNIT_ASSERT(resultParser2.TryNextRow());

    auto discount2 = *resultParser2.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount2, 3.0);
    UNIT_ASSERT(!resultParser2.TryNextRow());

    auto commitResult = tx.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult.GetStatus(), EStatus::SUCCESS);
}

Y_UNIT_TEST(ShouldUpsert) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    TString query = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params1 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int32(2)
                .AddMember("p3").Int32(1013)
                .AddMember("p4").OptionalDouble(132.10)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto params2 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int32(2)
                .AddMember("p3").Int32(999)
                .AddMember("p4").OptionalDouble(12.10)
                .AddMember("p5").OptionalDouble(1.0)
                .AddMember("p6").OptionalInt32(18)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto result1 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result1.GetTransaction());
    auto tx1 = *result1.GetTransaction();
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);

    auto result2 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());
    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    TString selectQuery1 = R"(
        SELECT C_BALANCE, C_PAYMENT_CNT
          FROM customer
         WHERE C_W_ID = 1
           AND C_D_ID = 2
           AND C_ID = 1013;
    )";

    auto sresult1 = session.ExecuteDataQuery(selectQuery1, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(sresult1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto pcount1 = *resultParser1.ColumnParser("C_PAYMENT_CNT").GetOptionalInt32();
    UNIT_ASSERT_VALUES_EQUAL(pcount1, 17);

    TString selectQuery2 = R"(
        SELECT C_BALANCE, C_PAYMENT_CNT
          FROM customer
         WHERE C_W_ID = 1
           AND C_D_ID = 2
           AND C_ID = 999;
    )";

    auto sresult2 = session.ExecuteDataQuery(selectQuery2, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetResultSets().size(), 1);

    TResultSetParser resultParser2(sresult2.GetResultSet(0));
    UNIT_ASSERT(resultParser2.TryNextRow());

    auto pcount2 = *resultParser2.ColumnParser("C_PAYMENT_CNT").GetOptionalInt32();
    UNIT_ASSERT_VALUES_EQUAL(pcount2, 18);
}

Y_UNIT_TEST(ShouldUpsertKeyWithInt64) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int64            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    TString query = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int64, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params1 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int64(4294967297)
                .AddMember("p3").Int32(1013)
                .AddMember("p4").OptionalDouble(132.10)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto params2 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int64(4294967298)
                .AddMember("p3").Int32(999)
                .AddMember("p4").OptionalDouble(12.10)
                .AddMember("p5").OptionalDouble(1.0)
                .AddMember("p6").OptionalInt32(18)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto result1 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result1.GetTransaction());
    auto tx1 = *result1.GetTransaction();
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);

    auto result2 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());
    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    TString selectQuery1 = R"(
        SELECT C_BALANCE, C_PAYMENT_CNT
          FROM customer
         WHERE C_W_ID = 1
           AND C_D_ID = 4294967297
           AND C_ID = 1013;
    )";

    auto sresult1 = session.ExecuteDataQuery(selectQuery1, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(sresult1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto pcount1 = *resultParser1.ColumnParser("C_PAYMENT_CNT").GetOptionalInt32();
    UNIT_ASSERT_VALUES_EQUAL(pcount1, 17);

    TString selectQuery2 = R"(
        SELECT C_BALANCE, C_PAYMENT_CNT
          FROM customer
         WHERE C_W_ID = 1
           AND C_D_ID = 4294967298
           AND C_ID = 999;
    )";

    auto sresult2 = session.ExecuteDataQuery(selectQuery2, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetResultSets().size(), 1);

    TResultSetParser resultParser2(sresult2.GetResultSet(0));
    UNIT_ASSERT(resultParser2.TryNextRow());

    auto pcount2 = *resultParser2.ColumnParser("C_PAYMENT_CNT").GetOptionalInt32();
    UNIT_ASSERT_VALUES_EQUAL(pcount2, 18);
}

Y_UNIT_TEST(ShouldUpsertKeyWithInt64_2) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE history (
            H_C_W_ID    Int32,
            H_C_ID      Int32,
            H_C_D_ID    Int32,
            H_D_ID      Int32,
            H_W_ID      Int32,
            H_DATE      Timestamp,
            H_AMOUNT    Double,
            H_DATA      Utf8,
            H_C_NANO_TS Int64        NOT NULL,

            PRIMARY KEY (H_C_W_ID, H_C_NANO_TS)
        )
    )").GetValueSync().IsSuccess());

    TString query = R"(
        DECLARE $batch AS List<Struct<p1:Int32?, p2:Int32?, p3:Int32?, p4:Int32?, p5:Int32?, p6:Timestamp?, p7:Double?, p8:Text?, p9:Int64>>;
        UPSERT INTO `history` SELECT p1 AS `H_C_D_ID`, p2 AS `H_C_W_ID`, p3 AS `H_C_ID`,p4 AS `H_D_ID`,
            p5 AS `H_W_ID`, p6 AS `H_DATE`, p7 AS `H_AMOUNT`, p8 AS `H_DATA`, p9 AS `H_C_NANO_TS` FROM AS_TABLE($batch);
    )";

    auto params1 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").OptionalInt32(10)
                .AddMember("p2").OptionalInt32(2)
                .AddMember("p3").OptionalInt32(1013)
                .AddMember("p4").OptionalInt32(11)
                .AddMember("p5").OptionalInt32(12)
                .AddMember("p6").OptionalTimestamp(TInstant::MicroSeconds(1111))
                .AddMember("p7").OptionalDouble(111)
                .AddMember("p8").OptionalUtf8("utf8 is actually text")
                .AddMember("p9").Int64(12682737714234676)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto params2 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").OptionalInt32(10)
                .AddMember("p2").OptionalInt32(2)
                .AddMember("p3").OptionalInt32(1013)
                .AddMember("p4").OptionalInt32(11)
                .AddMember("p5").OptionalInt32(12)
                .AddMember("p6").OptionalTimestamp(TInstant::MicroSeconds(1111))
                .AddMember("p7").OptionalDouble(112)
                .AddMember("p8").OptionalUtf8("utf8 is actually text2")
                .AddMember("p9").Int64(12682737714234677)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto result1 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result1.GetTransaction());
    auto tx1 = *result1.GetTransaction();
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);

    auto result2 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());
    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    TString selectQuery1 = R"(
        SELECT H_AMOUNT
          FROM history
         WHERE H_C_W_ID = 2
           AND H_C_NANO_TS = 12682737714234676;
    )";

    auto sresult1 = session.ExecuteDataQuery(selectQuery1, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(sresult1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto pcount1 = *resultParser1.ColumnParser("H_AMOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(pcount1, 111);

    TString selectQuery2 = R"(
        SELECT H_AMOUNT
          FROM history
         WHERE H_C_W_ID = 2
           AND H_C_NANO_TS = 12682737714234677;
    )";

    auto sresult2 = session.ExecuteDataQuery(selectQuery2, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetResultSets().size(), 1);

    TResultSetParser resultParser2(sresult2.GetResultSet(0));
    UNIT_ASSERT(resultParser2.TryNextRow());

    auto pcount2 = *resultParser2.ColumnParser("H_AMOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(pcount2, 112);
}

Y_UNIT_TEST(ShouldUpsertUnorderedColumns) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    TString query = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_D_ID`, p2 AS `C_W_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params1 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(2)
                .AddMember("p2").Int32(1)
                .AddMember("p3").Int32(1013)
                .AddMember("p4").OptionalDouble(132.10)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto params2 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(2)
                .AddMember("p2").Int32(1)
                .AddMember("p3").Int32(999)
                .AddMember("p4").OptionalDouble(12.10)
                .AddMember("p5").OptionalDouble(1.0)
                .AddMember("p6").OptionalInt32(18)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto result1 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result1.GetTransaction());
    auto tx1 = *result1.GetTransaction();
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);

    auto result2 = session.ExecuteDataQuery(query, TTxControl::BeginTx(), params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());
    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    TString selectQuery1 = R"(
        SELECT C_BALANCE, C_PAYMENT_CNT
          FROM customer
         WHERE C_W_ID = 1
           AND C_D_ID = 2
           AND C_ID = 1013;
    )";

    auto sresult1 = session.ExecuteDataQuery(selectQuery1, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(sresult1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto pcount1 = *resultParser1.ColumnParser("C_PAYMENT_CNT").GetOptionalInt32();
    UNIT_ASSERT_VALUES_EQUAL(pcount1, 17);

    TString selectQuery2 = R"(
        SELECT C_BALANCE, C_PAYMENT_CNT
          FROM customer
         WHERE C_W_ID = 1
           AND C_D_ID = 2
           AND C_ID = 999;
    )";

    auto sresult2 = session.ExecuteDataQuery(selectQuery2, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(sresult2.GetResultSets().size(), 1);

    TResultSetParser resultParser2(sresult2.GetResultSet(0));
    UNIT_ASSERT(resultParser2.TryNextRow());

    auto pcount2 = *resultParser2.ColumnParser("C_PAYMENT_CNT").GetOptionalInt32();
    UNIT_ASSERT_VALUES_EQUAL(pcount2, 18);
}

Y_UNIT_TEST(ShouldSelectUpsertInTransaction) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST, C_BALANCE) VALUES
            (1, 1, 1, 3, "credit1", "last1", 10),
            (13, 11, 17, 7, "credit2", "last2", 11);
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query1 = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_BALANCE, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto txControl = TTxControl::BeginTx(TTxSettings::SerializableRW());

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(13).Build()
        .AddParam("$c_d_id").Int32(11).Build()
        .AddParam("$c_id").Int32(17).Build()
        .Build();

    auto result1 = session.ExecuteDataQuery(query1, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(result1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto balance = *resultParser1.ColumnParser("C_BALANCE").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(balance, 11.0);

    UNIT_ASSERT(result1.GetTransaction());
    auto tx = *result1.GetTransaction();

    TString upsertQuery = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_D_ID`, p2 AS `C_W_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params2 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(11)
                .AddMember("p2").Int32(13)
                .AddMember("p3").Int32(17)
                .AddMember("p4").OptionalDouble(5.0)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto upsertResult = session.ExecuteDataQuery(upsertQuery, TTxControl::Tx(tx), params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(upsertResult.GetStatus(), EStatus::SUCCESS);

    auto commitResult = tx.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult.GetStatus(), EStatus::SUCCESS);
}

Y_UNIT_TEST(ShouldSelectUpsertInTransaction2) {
    // SELECT query with limit to avoid fast query

    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST, C_BALANCE) VALUES
            (1, 1, 1, 3, "credit1", "last1", 10),
            (13, 11, 17, 7, "credit2", "last2", 11);
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query1 = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_BALANCE, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id
        LIMIT 1;
    )";

    auto txControl = TTxControl::BeginTx(TTxSettings::SerializableRW());

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(13).Build()
        .AddParam("$c_d_id").Int32(11).Build()
        .AddParam("$c_id").Int32(17).Build()
        .Build();

    auto result1 = session.ExecuteDataQuery(query1, txControl, params).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(result1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto balance = *resultParser1.ColumnParser("C_BALANCE").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(balance, 11.0);

    UNIT_ASSERT(result1.GetTransaction());
    auto tx = *result1.GetTransaction();

    TString upsertQuery = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_D_ID`, p2 AS `C_W_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params2 = session.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(11)
                .AddMember("p2").Int32(13)
                .AddMember("p3").Int32(17)
                .AddMember("p4").OptionalDouble(5.0)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto upsertResult = session.ExecuteDataQuery(upsertQuery, TTxControl::Tx(tx), params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(upsertResult.GetStatus(), EStatus::SUCCESS);

    auto commitResult = tx.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult.GetStatus(), EStatus::SUCCESS);
}

Y_UNIT_TEST(ParallelUpsertsShouldCommit) {
    // 2 upserts in parallel, different rows

    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session1 = db.CreateSession().GetValueSync().GetSession();
    auto session2 = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session1.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    TString query = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params1 = session1.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int32(2)
                .AddMember("p3").Int32(1013)
                .AddMember("p4").OptionalDouble(132.10)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto params2 = session2.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int32(2)
                .AddMember("p3").Int32(999)
                .AddMember("p4").OptionalDouble(12.10)
                .AddMember("p5").OptionalDouble(1.0)
                .AddMember("p6").OptionalInt32(18)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto txControl1 = TTxControl::BeginTx(TTxSettings::SerializableRW());
    auto result1 = session1.ExecuteDataQuery(query, txControl1, params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result1.GetTransaction());

    auto txControl2 = TTxControl::BeginTx(TTxSettings::SerializableRW());
    auto result2 = session2.ExecuteDataQuery(query, txControl2, params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());

    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    auto tx1 = *result1.GetTransaction();
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);
}

Y_UNIT_TEST(ParallelUpsertsShouldAbort) {
    // 2 upserts in parallel, same rows

    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session1 = db.CreateSession().GetValueSync().GetSession();
    auto session2 = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session1.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,
            C_BALANCE      Double,
            C_YTD_PAYMENT  Double,
            C_PAYMENT_CNT  Int32,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    TString query = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_BALANCE`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params1 = session1.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int32(2)
                .AddMember("p3").Int32(1013)
                .AddMember("p4").OptionalDouble(132.10)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto params2 = session2.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(1)
                .AddMember("p2").Int32(2)
                .AddMember("p3").Int32(1013)
                .AddMember("p4").OptionalDouble(12.10)
                .AddMember("p5").OptionalDouble(1.0)
                .AddMember("p6").OptionalInt32(18)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto txControl1 = TTxControl::BeginTx(TTxSettings::SerializableRW());
    auto result1 = session1.ExecuteDataQuery(query, txControl1, params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result1.GetTransaction());

    auto txControl2 = TTxControl::BeginTx(TTxSettings::SerializableRW());
    auto result2 = session2.ExecuteDataQuery(query, txControl2, params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());

    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    auto tx1 = *result1.GetTransaction();
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::ABORTED);
}

Y_UNIT_TEST(ReadAndParallelUpsertWithin) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session1 = db.CreateSession().GetValueSync().GetSession();
    auto session2 = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session1.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session1.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 1, 3, "credit1", "last1"),
            (13, 11, 17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto txControl1 = TTxControl::BeginTx(TTxSettings::SerializableRW());

    auto params1 = session1.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(13).Build()
        .AddParam("$c_d_id").Int32(11).Build()
        .AddParam("$c_id").Int32(17).Build()
        .Build();

    auto result1 = session1.ExecuteDataQuery(query, txControl1, params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(result1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto discount = *resultParser1.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 7.0);
    UNIT_ASSERT(result1.GetTransaction());

    auto tx1 = *result1.GetTransaction();

    // parallel upsert

    TString upsertQuery = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_DISCOUNT`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params2 = session2.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(13)
                .AddMember("p2").Int32(11)
                .AddMember("p3").Int32(17)
                .AddMember("p4").OptionalDouble(99)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto txControl2 = TTxControl::BeginTx(TTxSettings::SerializableRW());
    auto result2 = session2.ExecuteDataQuery(upsertQuery, txControl2, params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());
    auto tx2 = *result2.GetTransaction();
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);

    // read shoud abort
    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);
}

Y_UNIT_TEST(UpsertAndParallelReadWithinShouldAbort) {
    TKikimrRunner kikimr;
    kikimr.GetTestServer().GetRuntime()->SetLogPriority(NKikimrServices::KQP_SESSION, NLog::PRI_TRACE);
    auto db = kikimr.GetTableClient();
    auto session1 = db.CreateSession().GetValueSync().GetSession();
    auto session2 = db.CreateSession().GetValueSync().GetSession();

    UNIT_ASSERT(session1.ExecuteSchemeQuery(R"(
        CREATE TABLE `/Root/customer` (
            C_W_ID         Int32            NOT NULL,
            C_D_ID         Int32            NOT NULL,
            C_ID           Int32            NOT NULL,
            C_DISCOUNT     Double,
            C_CREDIT       Utf8,
            C_LAST         Utf8,
            C_FIRST        Utf8,

            PRIMARY KEY (C_W_ID, C_D_ID, C_ID)
        )
    )").GetValueSync().IsSuccess());

    UNIT_ASSERT(session1.ExecuteDataQuery(R"(
        UPSERT INTO `/Root/customer` (C_W_ID, C_D_ID, C_ID, C_DISCOUNT, C_CREDIT, C_LAST) VALUES
            (1, 1, 1, 3, "credit1", "last1"),
            (13, 11, 17, 7, "credit2", "last2");
    )", TTxControl::BeginTx().CommitTx()).GetValueSync().IsSuccess());

    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )";

    auto txControl1 = TTxControl::BeginTx(TTxSettings::SerializableRW());

    auto params1 = session1.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(13).Build()
        .AddParam("$c_d_id").Int32(11).Build()
        .AddParam("$c_id").Int32(17).Build()
        .Build();

    auto result1 = session1.ExecuteDataQuery(query, txControl1, params1).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result1.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT_VALUES_EQUAL(result1.GetResultSets().size(), 1);

    TResultSetParser resultParser1(result1.GetResultSet(0));
    UNIT_ASSERT(resultParser1.TryNextRow());

    auto discount = *resultParser1.ColumnParser("C_DISCOUNT").GetOptionalDouble();
    UNIT_ASSERT_VALUES_EQUAL(discount, 7.0);
    UNIT_ASSERT(result1.GetTransaction());

    auto tx1 = *result1.GetTransaction();

    // parallel upsert

    TString upsertQuery = R"(
        DECLARE $batch AS List<Struct<p1:Int32, p2:Int32, p3:Int32, p4:Double?, p5:Double?, p6:Int32?>>;
        UPSERT INTO `customer` SELECT p1 AS `C_W_ID`, p2 AS `C_D_ID`, p3 AS `C_ID`,
            p4 AS `C_DISCOUNT`, p5 AS `C_YTD_PAYMENT`, p6 AS `C_PAYMENT_CNT` FROM AS_TABLE($batch);
    )";

    auto params2 = session2.GetParamsBuilder()
        .AddParam("$batch")
            .BeginList()
            .AddListItem()
                .BeginStruct()
                .AddMember("p1").Int32(13)
                .AddMember("p2").Int32(11)
                .AddMember("p3").Int32(17)
                .AddMember("p4").OptionalDouble(99)
                .AddMember("p5").OptionalDouble(2.0)
                .AddMember("p6").OptionalInt32(17)
                .EndStruct()
            .EndList()
            .Build()
        .Build();

    auto txControl2 = TTxControl::BeginTx(TTxSettings::SerializableRW());
    auto result2 = session2.ExecuteDataQuery(upsertQuery, txControl2, params2).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL(result2.GetStatus(), EStatus::SUCCESS);
    UNIT_ASSERT(result2.GetTransaction());
    auto tx2 = *result2.GetTransaction();

    auto commitResult1 = tx1.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult1.GetStatus(), EStatus::SUCCESS);

    // upsert shoud abort
    auto commitResult2 = tx2.Commit().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(commitResult2.GetStatus(), EStatus::SUCCESS);
}

}

} // namespace NKikimr::NKqp
