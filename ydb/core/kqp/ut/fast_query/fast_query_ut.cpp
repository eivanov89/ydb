#include <ydb/core/kqp/compile_service/kqp_fast_query.h>
#include <ydb/core/testlib/test_client.h>

#include <library/cpp/testing/unittest/tests_data.h>
#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

using namespace NYdb;

namespace {
} // anonymous

Y_UNIT_TEST_SUITE(KqpFastQuery) {

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
        "c_discount", "c_last", "c_credit"
    };

    std::vector<std::pair<TString, size_t>> whereColumnsWithPos = {
        {"c_w_id", 1}, {"c_d_id", 2}, {"c_id", 3}
    };

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.OriginalQueryHash, goldHash);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::SELECT_QUERY);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.PostgresQuery.TablePathPrefix, "/Root/db1");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.TableName, "customer");
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ColumnsToSelect, columnsToSelect);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.WhereColumnsWithPos, whereColumnsWithPos);
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

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
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

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
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

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
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

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

Y_UNIT_TEST(ShouldNotFastOrWhere) {
    TString query = R"(
        --!syntax_v1

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID == $c_w_id
           OR C_D_ID = $c_d_id
           OR C_ID = $c_id

        LIMIT 1;
    )";

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
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

    TFastQuery fastQuery = CompileToFastQuery(query);
    UNIT_ASSERT_VALUES_EQUAL(fastQuery.ExecutionType, TFastQuery::EExecutionType::UNSUPPORTED);
}

}

} // namespace NKikimr::NKqp
