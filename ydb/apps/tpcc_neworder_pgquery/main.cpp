#include <ydb/public/sdk/cpp/client/ydb_table/table.h>
#include <ydb/public/sdk/cpp/client/ydb_types/credentials/credentials.h>

#include <library/cpp/getopt/last_getopt.h>
#include <library/cpp/histogram/hdr/histogram.h>

#include <contrib/libs/libpg_query/pg_query.h>

#include <util/string/printf.h>

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <vector>

using namespace NLastGetopt;

using namespace NYdb;
using namespace NTable;

using namespace std::chrono_literals;

// copy-pasted TPC-C constants from the Benchbase implementation

const int C_ID_C = 259; // in range [0, 1023]
const int CUSTOMERS_PER_DISTRICT = 3000;

const int OL_I_ID_C = 7911; // in range [0, 8191]
int ITEMS_COUNT = 100000;

int INVALID_ITEM_ID = -12345;

// our almost random delay between transactions
const int DEFAULT_DELAY_MS = 200;

const int MIN_ITEMS = 5;
const int MAX_ITEMS = 15;

TString GetPricesSql[MAX_ITEMS];
TString GetStocksSql[MAX_ITEMS];

struct TLatencies {
    NHdr::THistogram getCustomerHistogram{1, 1000, 5};
    NHdr::THistogram getWarehouseHistogram{1, 1000, 5};
    NHdr::THistogram getDistrictHistogram{1, 1000, 5};
    NHdr::THistogram updateDistrictHistogram{1, 1000, 5};
    NHdr::THistogram insertOpenOrderHistogram{1, 1000, 5};
    NHdr::THistogram insertNewOrderHistogram{1, 1000, 5};
    NHdr::THistogram getPriceHistogram{1, 1000, 5};
    NHdr::THistogram getStockHistogram{1, 1000, 5};
    NHdr::THistogram updateOrderLine{1, 1000, 5};
    NHdr::THistogram updateStockHistogram{1, 1000, 5};
    NHdr::THistogram commitHistogram{1, 1000, 5};
};

class TYdbErrorException : public yexception {
public:
    TYdbErrorException(const TStatus& status)
        : Status(status) {}

    TStatus Status;
};

class TUserAbortedException : public yexception {
};

void ThrowOnError(const TStatus& status) {
    if (!status.IsSuccess()) {
        throw TYdbErrorException(status);
    }
}

void PrintStatus(const TStatus& status) {
    Cerr << "Status: " << status.GetStatus() << Endl;
    status.GetIssues().PrintTo(Cerr);
}

int GetRandomNumber(int min, int max) {
    return min + (std::rand() % (max - min + 1));
}

int NonUniformRandom(int A, int C, int min, int max) {
    return (((GetRandomNumber(0, A) | GetRandomNumber(min, max)) + C) %
        (max - min + 1)) + min;
}

int GetRandomCustomerID() {
    return NonUniformRandom(1023, C_ID_C, 1, CUSTOMERS_PER_DISTRICT);
}

TString UsToMsString(int64_t us) {
    double ms = static_cast<double>(us) / 1000;
    return Sprintf("%.2f", ms);
}

int GetRandomItemId() {
    return NonUniformRandom(8191, OL_I_ID_C, 1, ITEMS_COUNT);
}

void PrintHistogramAsTableRow(const char *name, const NHdr::THistogram &histogram, const std::vector<double> &percentiles) {
    std::cout << std::left << std::setw(20) << name;
    for (double percentile : percentiles) {
        std::cout << std::right << std::setw(10) << std::setfill(' ')
            << std::setprecision(3) << (histogram.GetValueAtPercentile(percentile) / 1000.0);
    }
    std::cout << std::endl;
}

TDataQueryResult GetCustomer(TSession session, const TString& path, int warehouseID, int districtID, int customerID) {
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $c_w_id AS Int32;
        DECLARE $c_d_id AS Int32;
        DECLARE $c_id AS Int32;

        SELECT C_DISCOUNT, C_LAST, C_CREDIT
          FROM customer
         WHERE C_W_ID = $c_w_id
           AND C_D_ID = $c_d_id
           AND C_ID = $c_id;
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$c_w_id").Int32(warehouseID).Build()
        .AddParam("$c_d_id").Int32(districtID).Build()
        .AddParam("$c_id").Int32(customerID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::BeginTx(TTxSettings::SerializableRW()),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

TDataQueryResult GetWarehouse(TSession session, const TTransaction& tx, const TString& path, int warehouseID) {
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $w_id AS Int32;

        SELECT W_TAX
        FROM warehouse
        WHERE W_ID = $w_id;
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$w_id").Int32(warehouseID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

TDataQueryResult GetDistrict(TSession session, const TTransaction& tx, const TString& path, int warehouseID, int districtID) {
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $w_id AS Int32;
        DECLARE $d_id AS Int32;

        SELECT D_NEXT_O_ID, D_TAX
        FROM district
        WHERE D_W_ID = $w_id
            AND D_ID = $d_id;
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$w_id").Int32(warehouseID).Build()
        .AddParam("$d_id").Int32(districtID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

TDataQueryResult UpdateDistrict(
    TSession session,
    const TTransaction& tx,
    const TString& path,
    int warehouseID,
    int districtID,
    int nextOrderID)
{
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $d_w_id AS Int32;
        DECLARE $d_id AS Int32;
        DECLARE $d_next_o_id AS Int32;

        UPSERT INTO district (D_W_ID, D_ID, D_NEXT_O_ID)
        VALUES ($d_w_id, $d_id, $d_next_o_id)
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$d_w_id").Int32(warehouseID).Build()
        .AddParam("$d_id").Int32(districtID).Build()
        .AddParam("$d_next_o_id").Int32(nextOrderID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

TDataQueryResult InsertOpenOrder(
    TSession session,
    const TTransaction& tx,
    const TString& path,
    int warehouseID,
    int districtID,
    int customerID,
    int itemCount,
    int orderID)
{
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $o_id AS Int32;
        DECLARE $o_w_id AS Int32;
        DECLARE $o_d_id AS Int32;
        DECLARE $o_c_id AS Int32;
        DECLARE $o_e_id AS Timestamp;
        DECLARE $o_ol_cnt AS Int32;

        UPSERT INTO oorder (O_ID, O_D_ID, O_W_ID, O_C_ID, O_ENTRY_D, O_OL_CNT, O_ALL_LOCAL)
        VALUES ($o_id, $o_d_id, $o_w_id, $o_c_id, $o_e_id, $o_ol_cnt, 1)
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$o_id").Int32(orderID).Build()
        .AddParam("$o_w_id").Int32(warehouseID).Build()
        .AddParam("$o_d_id").Int32(districtID).Build()
        .AddParam("$o_c_id").Int32(customerID).Build()
        .AddParam("$o_e_id").Timestamp(TInstant::Now()).Build()
        .AddParam("$o_ol_cnt").Int32(itemCount).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

TDataQueryResult InsertNewOrder(
    TSession session,
    const TTransaction& tx,
    const TString& path,
    int warehouseID,
    int districtID,
    int orderID)
{
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $no_id AS Int32;
        DECLARE $no_w_id AS Int32;
        DECLARE $no_d_id AS Int32;

        UPSERT INTO new_order (NO_O_ID, NO_D_ID, NO_W_ID)
        VALUES ($no_id, $no_d_id, $no_w_id)
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$no_id").Int32(orderID).Build()
        .AddParam("$no_w_id").Int32(warehouseID).Build()
        .AddParam("$no_d_id").Int32(districtID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

TDataQueryResult GetItemPrice(
    TSession session,
    const TTransaction& tx,
    const TString& path,
    int itemID)
{
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $i_id AS Int32;

        SELECT I_PRICE, I_NAME, I_DATA
        FROM item
        WHERE I_ID = $i_id
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$i_id").Int32(itemID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}

std::map<int, int> GetItemPrices(
    TSession session,
    const TTransaction& tx,
    const std::vector<int>& itemIDs)
{
    std::set<int> uniqueItemIDs(itemIDs.begin(), itemIDs.end());

    const TString& query = GetPricesSql[uniqueItemIDs.size() - 1];
    auto paramsBuilder = session.GetParamsBuilder();

    int itemCount = 1;
    for (auto itemID: uniqueItemIDs) {
        paramsBuilder.AddParam(Sprintf("$item%d", itemCount)).Int32(itemID).Build();
        itemCount++;
    }

    auto params = paramsBuilder.Build();

    auto queryResult = session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();

    if (!queryResult.IsSuccess()) {
        Cerr << "failed to get prices: ";
        queryResult.Out(Cerr);
        Cerr << Endl;
        throw std::runtime_error("Failed to get item prices");
    }

    std::map<int, int> result;
    TResultSetParser parser(queryResult.GetResultSet(0));
    for (size_t i = 0; i < uniqueItemIDs.size(); ++i) {
        if (!parser.TryNextRow()) {
            for (auto itemId: uniqueItemIDs) {
                if (itemId == INVALID_ITEM_ID) {
                    throw TUserAbortedException();
                }
            }
            throw std::runtime_error("No rows in result for item price query");
        }

        int itemID = parser.ColumnParser("I_ID").GetInt32();
        int price = *parser.ColumnParser("I_PRICE").GetOptionalDouble();
        result[itemID] = price;
    }

    return result;
}

TDataQueryResult GetStock(
    TSession session,
    const TTransaction& tx,
    const TString& path,
    int warehouseID,
    int itemID)
{
    TString query = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        DECLARE $s_i_id AS Int32;
        DECLARE $s_w_id AS Int32;

        SELECT S_QUANTITY, S_DATA, S_DIST_01, S_DIST_02, S_DIST_03, S_DIST_04, S_DIST_05,
               S_DIST_06, S_DIST_07, S_DIST_08, S_DIST_09, S_DIST_10
          FROM stock
         WHERE S_I_ID = $s_i_id
           AND S_W_ID = $s_w_id
    )", path.c_str());

    auto params = session.GetParamsBuilder()
        .AddParam("$s_i_id").Int32(itemID).Build()
        .AddParam("$s_w_id").Int32(warehouseID).Build()
        .Build();

    return session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();
}


std::map<std::pair<int, int>, int> GetStocks(
    TSession session,
    const TTransaction& tx,
    const std::vector<int>& warehouseIDs,
    const std::vector<int>& itemIDs)
{
    std::set<std::pair<int, int>> uniquePairs;
    for (size_t i = 0; i < warehouseIDs.size(); ++i) {
        uniquePairs.insert({warehouseIDs[i], itemIDs[i]});
    }

    const TString& query = GetStocksSql[uniquePairs.size() - 1];
    auto paramsBuilder = session.GetParamsBuilder();

    int itemCount = 1;
    for (auto [warehouseID, itemID]: uniquePairs) {
        paramsBuilder.AddParam(Sprintf("$w%d", itemCount)).Int32(warehouseID).Build();
        paramsBuilder.AddParam(Sprintf("$i%d", itemCount)).Int32(itemID).Build();
        itemCount++;
    }

    auto params = paramsBuilder.Build();

    std::map<std::pair<int, int>, int> result;

    auto queryResult = session.ExecuteDataQuery(
        query,
        TTxControl::Tx(tx),
        std::move(params),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();

    if (!queryResult.IsSuccess()) {
        Cerr << "failed to get stocks: ";
        queryResult.Out(Cerr);
        Cerr << Endl;
        throw std::runtime_error("Failed to get stocks");
    }

    TResultSetParser parser(queryResult.GetResultSet(0));
    for (size_t i = 0; i < uniquePairs.size(); ++i) {
        if (!parser.TryNextRow()) {
            throw std::runtime_error("No rows in result for stock query");
        }

        int warehouseID = parser.ColumnParser("S_W_ID").GetInt32();
        int itemID = parser.ColumnParser("S_I_ID").GetInt32();
        int quantity = *parser.ColumnParser("S_QUANTITY").GetOptionalInt32();
        result[{warehouseID, itemID}] = quantity;
    }

    return result;
}


void PrintLog(const char * msg)
{
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_us = duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1'000'000;

    // Convert to broken-down time (UTC)
    std::tm now_tm = *std::gmtime(&now_time_t);  // Convert to UTC time (gmtime)

    // Print time in the desired format (HH:MM:SS.microsecondsZ)
    std::cout << std::put_time(&now_tm, "%T")    // Print hours, minutes, seconds
              << '.' << std::setw(6) << std::setfill('0') << now_us.count()  // Print microseconds
              << 'Z' << ' ' << msg << std::endl;
}

TStatus RunSingleNewOrderTransaction(
    TSession session,
    const TString& path,
    int warehouseID,
    int districtID,
    int customerID,
    const std::vector<int>& itemIDs,
    const std::vector<int>& supplierWarehouseIDs,
    const std::vector<int>& quantities,
    TLatencies& latencies)
{
    const auto start = std::chrono::steady_clock::now();
    PrintLog("Start");

    // Get Customer

    auto customerResult = GetCustomer(session, path, warehouseID, districtID, customerID);
    if (!customerResult.IsSuccess()) {
        return customerResult;
    }
    const auto getCustomerEnd = std::chrono::steady_clock::now();
    const auto getCustomerUs = std::chrono::duration_cast<std::chrono::microseconds>(getCustomerEnd - start).count();
    PrintLog("GotCustomer");

    auto tx = *customerResult.GetTransaction();
    Cerr << "transactionId: " << tx.GetId() << Endl;

    // Get Warehouse

    PrintLog("getWarehouseStart");
    const auto getWarehouseStart = std::chrono::steady_clock::now();
    auto warehouseResult = GetWarehouse(session, tx, path, warehouseID);
    if (!warehouseResult.IsSuccess()) {
        return warehouseResult;
    }

    const auto getWarehouseEnd = std::chrono::steady_clock::now();
    const auto getWarehouseUs =
        std::chrono::duration_cast<std::chrono::microseconds>(getWarehouseEnd - getWarehouseStart).count();

    PrintLog("getWarehouseEnd");
    // Get District

    const auto getDistrictStart = std::chrono::steady_clock::now();
    auto districtResult = GetDistrict(session, tx, path, warehouseID, districtID);
    if (!districtResult.IsSuccess()) {
        return districtResult;
    }
    const auto getDistrictEnd = std::chrono::steady_clock::now();
    const auto getDistrictUs =
        std::chrono::duration_cast<std::chrono::microseconds>(getDistrictEnd - getDistrictStart).count();

    TResultSetParser districtParser(districtResult.GetResultSet(0));
    if (!districtParser.TryNextRow()) {
        TStringStream ss;
        ss << "No rows in result for district next order id query: warehouse "
            << warehouseID << ", district " << districtID;
        throw std::runtime_error(ss.Str());
    }
    const int nextOrderID = *districtParser.ColumnParser("D_NEXT_O_ID").GetOptionalInt32();

    const auto startDistrictUs = std::chrono::steady_clock::now();
    auto updateResult = UpdateDistrict(session, tx, path, warehouseID, districtID, nextOrderID + 1);
    if (!updateResult.IsSuccess()) {
        return updateResult;
    }
    const auto endDistrictUs = std::chrono::steady_clock::now();
    const auto updateDistrictUs = std::chrono::duration_cast<std::chrono::microseconds>(endDistrictUs - startDistrictUs).count();

    const auto startOpenOrder = std::chrono::steady_clock::now();
    auto insertOpenOrderResult =
        InsertOpenOrder(session, tx, path, warehouseID, districtID, customerID, itemIDs.size(), nextOrderID);
    if (!insertOpenOrderResult.IsSuccess()) {
        return insertOpenOrderResult;
    }
    const auto endOpenOrder = std::chrono::steady_clock::now();
    const auto openOrderUs = std::chrono::duration_cast<std::chrono::microseconds>(endOpenOrder - startOpenOrder).count();

    const auto startNewOrder = std::chrono::steady_clock::now();
    auto insertNewOrderResult = InsertNewOrder(session, tx, path, warehouseID, districtID, nextOrderID);
    if (!insertNewOrderResult.IsSuccess()) {
        return insertNewOrderResult;
    }
    const auto endNewOrder = std::chrono::steady_clock::now();
    const auto newOrderUs = std::chrono::duration_cast<std::chrono::microseconds>(endNewOrder - startNewOrder).count();

    static const TString odreLineSQL = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        declare $values as List<Struct<p1:Int32,p2:Int32,p3:Int32,p4:Int32,p5:Int32,p6:Timestamp,p7:Double,p8:Int32,p9:Double,p10:Utf8>>;
        $mapper = ($row) -> (AsStruct(
            $row.p1 as OL_W_ID, $row.p2 as OL_D_ID, $row.p3 as OL_O_ID, $row.p4 as OL_NUMBER, $row.p5 as OL_I_ID,
            $row.p6 as OL_DELIVERY_D, $row.p7 as OL_AMOUNT, $row.p8 as OL_SUPPLY_W_ID, $row.p9 as OL_QUANTITY,
            $row.p10 as OL_DIST_INFO));

        upsert into order_line select * from as_table(ListMap($values, $mapper));
    )", path.c_str());

    auto orderLineParamsBuilder = session.GetParamsBuilder();
    auto& orderLineValues = orderLineParamsBuilder.AddParam("$values");
    orderLineValues.BeginList();

    static const TString stockSQL = Sprintf(R"(
        --!syntax_v1

        PRAGMA TablePathPrefix("%s");

        declare $values as List<Struct<p1:Int32,p2:Int32,p3:Int32,p4:Double,p5:Int32,p6:Int32>>;
        $mapper = ($row) -> (AsStruct(
            $row.p1 as S_W_ID, $row.p2 as S_I_ID, $row.p3 as S_QUANTITY,
            $row.p4 as S_YTD, $row.p5 as S_ORDER_CNT,
            $row.p6 as S_REMOTE_CNT));

        upsert into stock select * from as_table(ListMap($values, $mapper));
    )", path.c_str());

    auto stockParamsBuilder = session.GetParamsBuilder();
    auto& stockValues = stockParamsBuilder.AddParam("$values");
    stockValues.BeginList();

    const auto startPrices = std::chrono::steady_clock::now();
    auto priceMap = GetItemPrices(session, tx, itemIDs);
    const auto endPrices = std::chrono::steady_clock::now();
    const auto getPricesUs = std::chrono::duration_cast<std::chrono::microseconds>(endPrices - startPrices).count();

    const auto startGetStockUs = std::chrono::steady_clock::now();
    auto stockMap = GetStocks(session, tx, supplierWarehouseIDs, itemIDs);
    const auto endGetStockUs = std::chrono::steady_clock::now();
    const auto getStockUs = std::chrono::duration_cast<std::chrono::microseconds>(endGetStockUs - startGetStockUs).count();

    for (int ol_number = 1; ol_number <= (int)itemIDs.size(); ++ol_number) {
        const auto ol_supply_w_id = supplierWarehouseIDs[ol_number - 1];
        const auto ol_i_id = itemIDs[ol_number - 1];
        const auto ol_quantity = quantities[ol_number - 1];

        const double i_price = priceMap[ol_i_id];
        const double ol_amount = ol_quantity * i_price;

        auto quantity = stockMap[{ol_supply_w_id, ol_i_id}];
        if (quantity - ol_quantity >= 10) {
            quantity -= ol_quantity;
        } else {
            quantity += 91 - ol_quantity;
        }

        // we don't mimic the original implementation here
        std::vector<TString> dist_infos(10);
        dist_infos[0] = "s_dist_01";
        dist_infos[1] = "s_dist_02";
        dist_infos[2] = "s_dist_03";
        dist_infos[3] = "s_dist_04";
        dist_infos[4] = "s_dist_05";
        dist_infos[5] = "s_dist_06";
        dist_infos[6] = "s_dist_07";
        dist_infos[7] = "s_dist_08";
        dist_infos[8] = "s_dist_09";
        dist_infos[9] = "s_dist_10";

        const TString& dist_info = dist_infos[districtID - 1];

        auto& orderLineStruct = orderLineValues.AddListItem().BeginStruct();
        orderLineStruct.AddMember("p1").Int32(warehouseID);
        orderLineStruct.AddMember("p2").Int32(districtID);
        orderLineStruct.AddMember("p3").Int32(nextOrderID);
        orderLineStruct.AddMember("p4").Int32(ol_number);
        orderLineStruct.AddMember("p5").Int32(ol_i_id);
        orderLineStruct.AddMember("p6").Timestamp(TInstant::Now());
        orderLineStruct.AddMember("p7").Double(ol_amount);
        orderLineStruct.AddMember("p8").Int32(ol_supply_w_id);
        orderLineStruct.AddMember("p9").Double(ol_quantity);
        orderLineStruct.AddMember("p10").Utf8(dist_info);
        orderLineStruct.EndStruct();

        auto& stockStruct = stockValues.AddListItem().BeginStruct();
        stockStruct.AddMember("p1").Int32(warehouseID);
        stockStruct.AddMember("p2").Int32(ol_i_id);
        stockStruct.AddMember("p3").Int32(quantity);
        stockStruct.AddMember("p4").Double(ol_quantity); // buggy like in Benchbase
        stockStruct.AddMember("p5").Int32(1); // buggy like in Benchbase
        stockStruct.AddMember("p6").Int32(0);
        stockStruct.EndStruct();
    }

    orderLineValues.EndList();
    orderLineValues.Build();
    const auto startOrderLine = std::chrono::steady_clock::now();
    auto orderLinesResult = session.ExecuteDataQuery(
        odreLineSQL,
        TTxControl::Tx(tx),
        orderLineParamsBuilder.Build(),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();

    if (!orderLinesResult.IsSuccess()) {
        return orderLinesResult;
    }
    const auto endOrderLine = std::chrono::steady_clock::now();
    const auto orderLineUs = std::chrono::duration_cast<std::chrono::microseconds>(endOrderLine - startOrderLine).count();

    stockValues.EndList();
    stockValues.Build();
    const auto startStock = std::chrono::steady_clock::now();
    auto stockResult = session.ExecuteDataQuery(
        stockSQL,
        TTxControl::Tx(tx),
        stockParamsBuilder.Build(),
        TExecDataQuerySettings().KeepInQueryCache(true)).GetValueSync();

    if (!stockResult.IsSuccess()) {
        return stockResult;
    }
    const auto endStock = std::chrono::steady_clock::now();
    const auto stockUs = std::chrono::duration_cast<std::chrono::microseconds>(endStock - startStock).count();

    // Commit

    PrintLog("startCommit");
    const auto commitStart = std::chrono::steady_clock::now();
    auto commitResult = tx.Commit().GetValueSync();
    PrintLog("endCommit");

    const auto end = std::chrono::steady_clock::now();
    const auto commitUs = std::chrono::duration_cast<std::chrono::microseconds>(end - commitStart).count();

    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    // including commit request
    const auto totalDbRequests = 11;

    if (commitResult.IsSuccess()) {
        latencies.getCustomerHistogram.RecordValue(getCustomerUs);
        latencies.getWarehouseHistogram.RecordValue(getWarehouseUs);
        latencies.getDistrictHistogram.RecordValue(getDistrictUs);
        latencies.updateDistrictHistogram.RecordValue(updateDistrictUs);
        latencies.insertOpenOrderHistogram.RecordValue(openOrderUs);
        latencies.insertNewOrderHistogram.RecordValue(newOrderUs);

        latencies.getPriceHistogram.RecordValue(getPricesUs);
        latencies.getStockHistogram.RecordValue(getStockUs);

        latencies.updateOrderLine.RecordValue(orderLineUs);
        latencies.updateStockHistogram.RecordValue(stockUs);
        latencies.commitHistogram.RecordValue(commitUs);
    }

    const auto avgWithoutBeginEnd = (totalUs - getCustomerUs - commitUs) / (totalDbRequests - 2);

    std::cout << "Total: " << UsToMsString(totalUs) << " in " << totalDbRequests << " requests (with commit)"
        << ", avg request time: " << UsToMsString(totalUs / totalDbRequests)
        << ", avg without first and commit: " << UsToMsString(avgWithoutBeginEnd)
        << ", GetCustomer: " << UsToMsString(getCustomerUs) << ", getWarehouse: " << UsToMsString(getWarehouseUs)
        << ", GetPrices: " << UsToMsString(getPricesUs) << ", GetStocks: " << UsToMsString(getStockUs)
        << ", Commit: " << UsToMsString(commitUs)
        << std::endl;

    return commitResult;
}

void InitSqls(const TString& path) {

    // Prices

    for (int i = 1; i <= MAX_ITEMS; ++i) {
        TStringStream ss;
        ss << Sprintf(R"(
            --!syntax_v1

            PRAGMA TablePathPrefix("%s");

        )", path.c_str());

        for (int j = 1; j <= i; ++j) {
            ss << Sprintf("DECLARE $item%d AS Int32;\n", j);
        }

        ss << "SELECT I_ID, I_PRICE, I_NAME, I_DATA FROM item WHERE I_ID IN (";
        for (int j = 1; j <= i; ++j) {
            if (j == 1) {
                ss << "$item1";
            } else {
                ss << ", $item" << j;
            }
        }
        ss << ")";
        GetPricesSql[i - 1] = ss.Str();
    }

    // Stock

    for (int i = 1; i <= MAX_ITEMS; ++i) {
        TStringStream ss;
        ss << Sprintf(R"(
            --!syntax_v1

            PRAGMA TablePathPrefix("%s");

        )", path.c_str());

        for (int j = 1; j <= i; ++j) {
            ss << Sprintf("DECLARE $w%d AS Int32;\n", j);
            ss << Sprintf("DECLARE $i%d AS Int32;\n", j);
        }

        ss << "SELECT S_W_ID, S_I_ID, S_QUANTITY, S_DATA, S_YTD, S_REMOTE_CNT, S_DIST_01, " <<
                        "S_DIST_02, S_DIST_03, S_DIST_04, S_DIST_05, S_DIST_06, S_DIST_07, S_DIST_08, " <<
                        "S_DIST_09, S_DIST_10 FROM stock WHERE (S_W_ID, S_I_ID) IN (";

        for (int j = 1; j <= i; ++j) {
            if (j == 1) {
                ss << "($w1, $i1)";
            } else {
                ss << ", ($w" << j << ", $i" << j << ")";
            }
        }
        ss << ")";
        GetStocksSql[i - 1] = ss.Str();
    }
}

void Run(const TDriver& driver, const TString& path, int warehouses, int count, int delayMs) {
    InitSqls(path);

    TTableClient client(driver);

    const auto delayDuration = std::chrono::milliseconds(delayMs);

    size_t userAborts = 0;
    size_t transactionsDone = 0;
    size_t errors = 0;
    NHdr::THistogram totalHistogram(1, 1000, 5);
    TLatencies latencies;

    for (int i = 0; i < count; ++i) {
        if (delayMs > 0) {
            // both pre and post execution delay
            std::this_thread::sleep_for(delayDuration);
        }

        const int districtID = std::rand() % 10 + 1;
        const int customerID = GetRandomCustomerID();
        const int warehouseID = warehouses == 1 ? 1 : std::rand() % warehouses + 1;

        int numItems = GetRandomNumber(MIN_ITEMS, MAX_ITEMS);
        std::vector<int> itemIDs(numItems);
        std::vector<int> supplierWarehouseIDs(numItems);
        std::vector<int> quantities(numItems);

        for (int i = 0; i < numItems; ++i) {
            itemIDs[i] = GetRandomItemId();
            supplierWarehouseIDs[i] = warehouseID;
            quantities[i] = GetRandomNumber(1, 10);
        }

        // we need to cause 1% of the new orders to be rolled back.
        if (GetRandomNumber(1, 100) == 1) {
            itemIDs[numItems - 1] = INVALID_ITEM_ID;
        }

        PrintLog("StartTransaction");

        const auto start = std::chrono::steady_clock::now();

        bool isAborted = false;
        ThrowOnError(client.RetryOperationSync(
                [path, warehouseID, districtID, customerID, &itemIDs, &supplierWarehouseIDs, &quantities, &isAborted, &errors, &latencies](TSession session) {
                    try {
                        return RunSingleNewOrderTransaction(
                            session,
                            path,
                            warehouseID,
                            districtID,
                            customerID,
                            itemIDs,
                            supplierWarehouseIDs,
                            quantities,
                            latencies
                            );
                    } catch (const TUserAbortedException&) {
                        isAborted = true;
                        return TStatus(EStatus::SUCCESS, NYql::TIssues());
                    } catch (const std::exception& e) {
                        Cerr << "Error: " << e.what() << Endl;
                        ++errors;
                        return TStatus(EStatus::SUCCESS, NYql::TIssues());
                    }
                }));

        if (isAborted) {
            ++userAborts;
            continue;
        }

        const auto end = std::chrono::steady_clock::now();
        const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        totalHistogram.RecordValue(totalUs);
        transactionsDone++;
    }

    Cout << "Finished" << Endl << "Transactions done: " << transactionsDone << ", user aborts: " << userAborts << Endl;

    std::vector<double> percentiles = {50.0, 90.0, 95.0, 99.0, 99.9};

    // Print the table header
    std::cout << std::endl;
    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
    std::cout << std::left << std::setw(20) << "Operation";
    for (double percentile : percentiles) {
        std::cout << std::right << std::setw(10) << std::setprecision(3) << percentile;
    }
    std::cout << std::endl;

    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
    PrintHistogramAsTableRow("Total", totalHistogram, percentiles);

    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
    PrintHistogramAsTableRow("GetCustomer", latencies.getCustomerHistogram, percentiles);
    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;

    PrintHistogramAsTableRow("GetWarehouse", latencies.getWarehouseHistogram, percentiles);
    PrintHistogramAsTableRow("GetDistrict", latencies.getDistrictHistogram, percentiles);
    PrintHistogramAsTableRow("UpdateDistrict", latencies.updateDistrictHistogram, percentiles);
    PrintHistogramAsTableRow("InsertOpenOrder", latencies.insertOpenOrderHistogram, percentiles);
    PrintHistogramAsTableRow("InsertNewOrder", latencies.insertNewOrderHistogram, percentiles);

    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
    PrintHistogramAsTableRow("GetPrice", latencies.getPriceHistogram, percentiles);
    PrintHistogramAsTableRow("GetStock", latencies.getStockHistogram, percentiles);
    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;

    PrintHistogramAsTableRow("UpdateOrderLine", latencies.updateOrderLine, percentiles);
    PrintHistogramAsTableRow("UpdateStock", latencies.updateStockHistogram, percentiles);
    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
    PrintHistogramAsTableRow("Commit", latencies.commitHistogram, percentiles);
    std::cout << std::left << std::setw(70) << std::setfill('-') << "" << std::setfill(' ') << std::endl;
}

int main(int argc, char** argv) {
    PgQueryParseResult result;
    result = pg_query_parse("SELECT 1");

    printf("%s\n", result.parse_tree);
    pg_query_free_parse_result(result);
    return 0;

    TOpts opts = TOpts::Default();

    TString endpoint;
    TString database;
    int count = 100;
    int delayMs = DEFAULT_DELAY_MS;
    int warehouses = 1;

    opts.AddLongOption('e', "endpoint", "YDB endpoint").Required().RequiredArgument("HOST:PORT")
        .StoreResult(&endpoint);
    opts.AddLongOption('d', "database", "YDB database name").Required().RequiredArgument("PATH")
        .StoreResult(&database);
    opts.AddLongOption('c', "count", "count requests").Optional().RequiredArgument("NUM")
        .StoreResult(&count).DefaultValue(count);
    opts.AddLongOption("delay", "delay between requests").Optional().RequiredArgument("NUM")
        .StoreResult(&delayMs).DefaultValue(delayMs);
    opts.AddLongOption('w', "warehouses", "number of warehouses").Optional().RequiredArgument("NUM")
        .StoreResult(&warehouses).DefaultValue(warehouses);

    TOptsParseResult res(&opts, argc, argv);
    Y_UNUSED(res);

    auto driverConfig = TDriverConfig()
        .SetEndpoint(endpoint)
        .SetDatabase(database);

    TDriver driver(driverConfig);

    // use fixed seed for reproducibility
    std::srand(1);

    try {
        Run(driver, database, warehouses, count, delayMs);
    } catch (const TYdbErrorException& e) {
        PrintStatus(e.Status);
        return 1;
    }

    driver.Stop(true);

    return 0;
}
