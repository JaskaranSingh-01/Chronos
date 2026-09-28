#include <gtest/gtest.h>

#include "simulator/orders/order.hpp"

namespace simulator {

Order make_test_order() {
    return Order{
        1,
        Side::Buy,
        Price{10000},
        Quantity{100},
        1000
    };
}

TEST(OrderTest, StartsAsNew) {
    auto order = make_test_order();

    EXPECT_EQ(order.state(), OrderState::New);
    EXPECT_EQ(order.quantity().value(), 100);
    EXPECT_EQ(order.remaining_quantity().value(), 100);
}

TEST(OrderTest, CanBeActivated) {
    auto order = make_test_order();

    EXPECT_TRUE(order.activate());
    EXPECT_EQ(order.state(), OrderState::Active);
}

TEST(OrderTest, CannotActivateActiveOrder) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_FALSE(order.activate());
    EXPECT_EQ(order.state(), OrderState::Active);
}

TEST(OrderTest, SupportsPartialExecution) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(order.execute(Quantity{30}));

    EXPECT_EQ(order.quantity().value(), 100);
    EXPECT_EQ(order.remaining_quantity().value(), 70);
    EXPECT_EQ(order.state(), OrderState::PartiallyFilled);
}

TEST(OrderTest, BecomesFilledAfterFullExecution) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(order.execute(Quantity{100}));

    EXPECT_EQ(order.quantity().value(), 100);
    EXPECT_EQ(order.remaining_quantity().value(), 0);
    EXPECT_EQ(order.state(), OrderState::Filled);
}

TEST(OrderTest, SupportsMultipleExecutions) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(order.execute(Quantity{30}));
    EXPECT_EQ(order.remaining_quantity().value(), 70);

    EXPECT_TRUE(order.execute(Quantity{20}));
    EXPECT_EQ(order.remaining_quantity().value(), 50);

    EXPECT_TRUE(order.execute(Quantity{50}));
    EXPECT_EQ(order.remaining_quantity().value(), 0);
    EXPECT_EQ(order.state(), OrderState::Filled);
}

TEST(OrderTest, RejectsExecutionGreaterThanRemaining) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(order.execute(Quantity{60}));

    EXPECT_FALSE(order.execute(Quantity{50}));

    EXPECT_EQ(order.remaining_quantity().value(), 40);
    EXPECT_EQ(order.state(), OrderState::PartiallyFilled);
}

TEST(OrderTest, RejectsZeroExecution) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_FALSE(order.execute(Quantity{0}));

    EXPECT_EQ(order.remaining_quantity().value(), 100);
    EXPECT_EQ(order.state(), OrderState::Active);
}

TEST(OrderTest, CannotExecuteNewOrder) {
    auto order = make_test_order();

    EXPECT_FALSE(order.execute(Quantity{50}));

    EXPECT_EQ(order.remaining_quantity().value(), 100);
    EXPECT_EQ(order.state(), OrderState::New);
}

TEST(OrderTest, CanCancelActiveOrder) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());

    EXPECT_TRUE(order.cancel());

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(order.remaining_quantity().value(), 100);
}

TEST(OrderTest, CanCancelPartiallyFilledOrder) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());
    ASSERT_TRUE(order.execute(Quantity{40}));

    EXPECT_TRUE(order.cancel());

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(order.remaining_quantity().value(), 60);
}

TEST(OrderTest, CannotCancelNewOrder) {
    auto order = make_test_order();

    EXPECT_FALSE(order.cancel());
    EXPECT_EQ(order.state(), OrderState::New);
}

TEST(OrderTest, CannotCancelFilledOrder) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());
    ASSERT_TRUE(order.execute(Quantity{100}));

    EXPECT_FALSE(order.cancel());
    EXPECT_EQ(order.state(), OrderState::Filled);
}

TEST(OrderTest, CannotExecuteCancelledOrder) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());
    ASSERT_TRUE(order.cancel());

    EXPECT_FALSE(order.execute(Quantity{50}));

    EXPECT_EQ(order.state(), OrderState::Cancelled);
    EXPECT_EQ(order.remaining_quantity().value(), 100);
}

TEST(OrderTest, CannotExecuteFilledOrder) {
    auto order = make_test_order();

    ASSERT_TRUE(order.activate());
    ASSERT_TRUE(order.execute(Quantity{100}));

    EXPECT_FALSE(order.execute(Quantity{1}));

    EXPECT_EQ(order.state(), OrderState::Filled);
    EXPECT_EQ(order.remaining_quantity().value(), 0);
}

} // namespace simulator