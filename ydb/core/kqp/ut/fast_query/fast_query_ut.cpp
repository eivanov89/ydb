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

}

} // namespace NKikimr::NKqp
