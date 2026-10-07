#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "simulator/itch/itch_replay_event_reader.hpp"
#include "simulator/replay/replay_engine.hpp"
#include "simulator/simulator.hpp"

namespace simulator {
namespace {

void append_u16_be(
    std::vector<std::byte>& data,
    std::uint16_t value
)
{
    data.push_back(
        static_cast<std::byte>((value >> 8) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>(value & 0xff)
    );
}

void append_u32_be(
    std::vector<std::byte>& data,
    std::uint32_t value
)
{
    data.push_back(
        static_cast<std::byte>((value >> 24) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>((value >> 16) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>((value >> 8) & 0xff)
    );

    data.push_back(
        static_cast<std::byte>(value & 0xff)
    );
}

void append_u64_be(
    std::vector<std::byte>& data,
    std::uint64_t value
)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        data.push_back(
            static_cast<std::byte>(
                (value >> shift) & 0xff
            )
        );
    }
}

void append_u48_be(
    std::vector<std::byte>& data,
    std::uint64_t value
)
{
    for (int shift = 40; shift >= 0; shift -= 8)
    {
        data.push_back(
            static_cast<std::byte>(
                (value >> shift) & 0xff
            )
        );
    }
}

void append_bytes(
    std::vector<std::byte>& data,
    const char* value,
    std::size_t size
)
{
    for (std::size_t i = 0; i < size; ++i)
    {
        data.push_back(
            static_cast<std::byte>(
                static_cast<unsigned char>(value[i])
            )
        );
    }
}

void append_add_order(
    std::vector<std::byte>& data,
    std::uint64_t timestamp,
    std::uint64_t order_id,
    char side,
    std::uint32_t shares,
    const char* stock,
    std::uint32_t price
)
{
    /*
     * Length:
     *   type      = 1 byte
     *   payload   = 31 bytes
     *   total     = 32 bytes
     */
    append_u16_be(data, 32);

    data.push_back(
        static_cast<std::byte>('A')
    );

    append_u48_be(data, timestamp);
    append_u64_be(data, order_id);

    data.push_back(
        static_cast<std::byte>(side)
    );

    append_u32_be(data, shares);

    append_bytes(data, stock, 8);

    append_u32_be(data, price);
}

void append_order_executed(
    std::vector<std::byte>& data,
    std::uint64_t timestamp,
    std::uint64_t order_id,
    std::uint32_t shares,
    std::uint64_t match_number
)
{
    /*
     * Length:
     *   type      = 1 byte
     *   payload   = 26 bytes
     *   total     = 27 bytes
     */
    append_u16_be(data, 27);

    data.push_back(
        static_cast<std::byte>('E')
    );

    append_u48_be(data, timestamp);
    append_u64_be(data, order_id);
    append_u32_be(data, shares);
    append_u64_be(data, match_number);
}

} // namespace


TEST(
    ItchReplayEngineTest,
    ReplaysHistoricalOrdersWithoutMatching
)
{
    std::vector<std::byte> data;

    /*
     * Historical order 1:
     *
     * SELL 100 @ 100.00
     */
    append_add_order(
        data,
        100,
        1,
        'S',
        100,
        "AAPL    ",
        10000
    );

    /*
     * Historical order 2:
     *
     * BUY 100 @ 101.00
     *
     * These prices cross:
     *
     * BUY  101
     * SELL 100
     *
     * However, these are exchange-generated historical
     * orders. They must NOT go through MatchingEngine.
     */
    append_add_order(
        data,
        200,
        2,
        'B',
        100,
        "AAPL    ",
        10100
    );

    /*
     * The exchange explicitly tells us that both
     * orders were executed.
     */
    append_order_executed(
        data,
        300,
        1,
        100,
        1001
    );

    append_order_executed(
        data,
        400,
        2,
        100,
        1002
    );

    Simulator simulator{16};

    ReplayEngine engine{simulator};

    ItchReplayReader reader{data};

    const ReplayResult result =
        engine.replay(reader);

    /*
     * Four normalized ReplayEvents were produced:
     *
     * A
     * A
     * E
     * E
     */
    EXPECT_EQ(
        result.statistics.received_events,
        4
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        4
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        0
    );

    EXPECT_EQ(
        result.failure,
        ReplayFailure::None
    );

    /*
     * Both historical orders were fully executed
     * and therefore removed from the OrderBook.
     */
    EXPECT_TRUE(
        simulator.order_book().empty()
    );

    /*
     * Most important assertion:
     *
     * Historical replay must NOT generate simulated
     * trades through MatchingEngine.
     */
    EXPECT_TRUE(
        simulator.trades().empty()
    );

    /*
     * Filled historical orders should have been
     * returned to the pool.
     */
    EXPECT_EQ(
        simulator.orders_in_use(),
        0
    );

    EXPECT_EQ(
        simulator.orders_available(),
        16
    );

    /*
     * ReplayEngine should have advanced the simulator
     * clock to the final event timestamp.
     */
    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{400}
    );
}


TEST(
    ItchReplayEngineTest,
    ReplaysPartialExecution
)
{
    std::vector<std::byte> data;

    /*
     * Add 100 shares.
     */
    append_add_order(
        data,
        100,
        1,
        'S',
        100,
        "AAPL    ",
        10000
    );

    /*
     * Execute only 40 shares.
     */
    append_order_executed(
        data,
        200,
        1,
        40,
        2001
    );

    Simulator simulator{16};

    ReplayEngine engine{simulator};

    ItchReplayReader reader{data};

    const ReplayResult result =
        engine.replay(reader);

    EXPECT_EQ(
        result.statistics.received_events,
        2
    );

    EXPECT_EQ(
        result.statistics.processed_events,
        2
    );

    EXPECT_EQ(
        result.statistics.rejected_events,
        0
    );

    EXPECT_EQ(
        result.failure,
        ReplayFailure::None
    );

    /*
     * The order is only partially executed, so it
     * must still exist in the historical book.
     */
    EXPECT_FALSE(
        simulator.order_book().empty()
    );

    const Order* order =
        simulator.order_book().find(1);

    ASSERT_NE(
        order,
        nullptr
    );

    EXPECT_EQ(
        order->remaining_quantity().value(),
        60
    );

    EXPECT_EQ(
        order->state(),
        OrderState::PartiallyFilled
    );

    /*
     * No simulated trade.
     */
    EXPECT_TRUE(
        simulator.trades().empty()
    );

    EXPECT_EQ(
        simulator.orders_in_use(),
        1
    );

    EXPECT_EQ(
        simulator.clock().now(),
        Timestamp{200}
    );
}

} // namespace simulator