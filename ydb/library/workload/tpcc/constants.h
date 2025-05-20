#pragma once

#include <cstddef>

namespace NYdb::NTPCC {

constexpr size_t TERMINALS_PER_WAREHOUSE = 10;

// copy-pasted TPC-C constants from the Benchbase implementation

constexpr int DISTRICT_LOW_ID = 1;
constexpr int DISTRICT_HIGH_ID = 10;

constexpr int C_ID_C = 259; // in range [0, 1023]
constexpr int CUSTOMERS_PER_DISTRICT = 3000;

constexpr int OL_I_ID_C = 7911; // in range [0, 8191]
constexpr int ITEMS_COUNT = 100000;

constexpr int INVALID_ITEM_ID = -12345;

// NewOrder transaction related constants
constexpr int MIN_ITEMS = 5;
constexpr int MAX_ITEMS = 15;

constexpr int C_LAST_LOAD_C = 157; // in range [0, 255]
constexpr int C_LAST_RUN_C = 223; // in range [0, 255]

} // namespace NYdb::NTPCC
