#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <utility>

#include "simulator/orders/order.hpp"
#include "simulator/orders/order_index.hpp"

namespace simulator {
namespace {

// The order `id` of `book`, one per (book, id) for the test's ids, as OrderBook::add marks it.
Order& order(SymbolId book, OrderId id)
{
    static std::map<std::pair<SymbolId, OrderId>, Order> orders;
    Order& o = orders[{book, id}];
    o.initialize(id, Side::Buy, Price{100}, Quantity{1}, 0);
    o.set_book(book);
    return o;
}

TEST(OrderIndexTest, FindsAnOrderByItsBookAndId)
{
    OrderIndex index{8};
    EXPECT_EQ(index.find(0, 1), nullptr);
    EXPECT_FALSE(index.erase(0, 1));

    Order& one = order(0, 1);
    Order& other = order(0, 42);
    ASSERT_TRUE(index.add(one));
    ASSERT_TRUE(index.add(other));
    EXPECT_EQ(index.size(), 2u);
    EXPECT_EQ(index.find(0, 1), &one);
    EXPECT_EQ(index.find(0, 42), &other);
    EXPECT_EQ(index.find(1, 1), nullptr);  // another book
    EXPECT_EQ(index.find(0, 7), nullptr);

    EXPECT_TRUE(index.erase(0, 1));
    EXPECT_FALSE(index.erase(0, 1));
    EXPECT_EQ(index.find(0, 1), nullptr);
    EXPECT_EQ(index.find(0, 42), &other);
    EXPECT_EQ(index.size(), 1u);
}

// MarketEvent promises an id unique only within its symbol: one id in two books is two orders.
TEST(OrderIndexTest, OneIdInTwoBooksIsTwoOrders)
{
    OrderIndex index{8};
    Order& first = order(1, 5);
    Order& second = order(2, 5);
    ASSERT_TRUE(index.add(first));
    ASSERT_TRUE(index.add(second));
    EXPECT_EQ(index.find(1, 5), &first);
    EXPECT_EQ(index.find(2, 5), &second);

    ASSERT_TRUE(index.erase(1, 5));
    EXPECT_EQ(index.find(1, 5), nullptr);
    EXPECT_EQ(index.find(2, 5), &second);
    EXPECT_FALSE(index.erase(3, 5));
}

// Ids shared by three books and a small table: their entries meet in long probe runs, where only the book tells them
// apart.
TEST(OrderIndexTest, MatchesAReferenceUnderChurn)
{
    OrderIndex index{8};
    Order orders[3][6];             // by book and id - 1
    Order* reference[3][6]{};       // the order indexed there, or nullptr
    std::size_t size = 0;
    for (SymbolId book = 0; book < 3; ++book) {
        for (OrderId id = 1; id <= 6; ++id) {
            orders[book][id - 1].initialize(id, Side::Buy, Price{100}, Quantity{1}, 0);
            orders[book][id - 1].set_book(book);
        }
    }
    std::mt19937_64 rng{1};

    for (int step = 0; step < 20'000; ++step) {
        const auto book = static_cast<SymbolId>(rng() % 3);
        const OrderId id = 1 + rng() % 6;
        Order*& entry = reference[book][id - 1];
        if (entry != nullptr) {
            ASSERT_TRUE(index.erase(book, id));
            entry = nullptr;
            --size;
        } else if (size < index.capacity()) {
            entry = &orders[book][id - 1];
            ASSERT_TRUE(index.add(*entry));
            ++size;
        }
        const auto probe_book = static_cast<SymbolId>(rng() % 3);
        const OrderId probe_id = 1 + rng() % 6;
        ASSERT_EQ(index.find(probe_book, probe_id), reference[probe_book][probe_id - 1]) << "step " << step;
        ASSERT_EQ(index.size(), size);
    }
}

TEST(OrderIndexTest, AFullIndexRefusesAnAdd)
{
    OrderIndex index{8};
    ASSERT_EQ(index.capacity(), 8u);
    for (OrderId id = 1; id <= 8; ++id) {
        ASSERT_TRUE(index.add(order(0, id)));
    }
    EXPECT_TRUE(index.full());
    EXPECT_FALSE(index.add(order(0, 9)));
    EXPECT_EQ(index.find(0, 9), nullptr);
    EXPECT_EQ(index.size(), 8u);
}

} // namespace
} // namespace simulator
