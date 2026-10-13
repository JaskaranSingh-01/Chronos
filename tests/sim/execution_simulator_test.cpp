#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/sim/execution_simulator.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 3;
constexpr Timestamp kNow = 21'000;

MarketEvent add(OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.order_id = id, .price = Price{price}, .quantity = Quantity{quantity},
                       .symbol = kSymbol, .type = MarketEventType::Add, .side = side};
}

MarketEvent status(TradingState state)
{
    return MarketEvent{.symbol = kSymbol, .type = MarketEventType::Status, .trading_state = state};
}

// Worked example 1's asks, plus two bid levels:
//   bids 999,900 x 300 (h4), 999,800 x 400 (h5)
//   asks 1,000,000 x 300 (h1 200, h2 100), 1,000,100 x 500 (h3)
void build_example_book(MarketBook& book)
{
    for (const MarketEvent& event : {add(1, Side::Sell, 1'000'000, 200), add(2, Side::Sell, 1'000'000, 100),
                                     add(3, Side::Sell, 1'000'100, 500), add(4, Side::Buy, 999'900, 300),
                                     add(5, Side::Buy, 999'800, 400)}) {
        ASSERT_EQ(book.apply(event), ApplyResult::Applied);
    }
}

OrderRequest order(OrderId id, Side side, std::int64_t price, std::uint64_t quantity,
                   TimeInForce time_in_force = TimeInForce::Ioc)
{
    return OrderRequest{.id = id, .side = side, .price = Price{price}, .quantity = Quantity{quantity},
                        .time_in_force = time_in_force};
}

ExecutionReport fill(OrderId id, Side side, std::int64_t price, std::uint64_t quantity, std::uint64_t leaves)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .price = Price{price},
                           .quantity = Quantity{quantity}, .leaves = Quantity{leaves},
                           .type = ReportType::Fill, .liquidity = Liquidity::Taker, .side = side};
}

ExecutionReport expired(OrderId id, Side side, std::uint64_t quantity)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .quantity = Quantity{quantity},
                           .type = ReportType::Expired, .side = side};
}

ExecutionReport rejected(OrderId id, Side side, std::uint64_t quantity, RejectReason reason)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .quantity = Quantity{quantity},
                           .type = ReportType::Rejected, .reason = reason, .side = side};
}

class ExecutionSimulatorTest : public ::testing::Test
{
protected:
    MarketBook book_{kSymbol, 16};
    ExecutionSimulator execution_;
    std::vector<ExecutionReport> out_;

    void SetUp() override { build_example_book(book_); }

    std::vector<ExecutionReport> arrive(const OrderRequest& request)
    {
        out_.clear();
        execution_.on_order_arrive(request, kNow, book_, out_);
        return out_;
    }
};

TEST_F(ExecutionSimulatorTest, FullFillAtOneLevel)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'000, 150)),
              (std::vector{fill(1, Side::Buy, 1'000'000, 150, 0)}));
}

TEST_F(ExecutionSimulatorTest, WorkedExample1SweepsThenExpiresAndRepeatsIdentically)
{
    const std::uint64_t digest = book_.digest();

    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'100, 1000)),
              (std::vector{fill(1, Side::Buy, 1'000'000, 300, 700), fill(1, Side::Buy, 1'000'100, 500, 200),
                           expired(1, Side::Buy, 200)}));
    EXPECT_EQ(book_.digest(), digest);
    EXPECT_EQ(book_.quantity_at(Side::Sell, Price{1'000'000}), 300u);
    EXPECT_EQ(book_.quantity_at(Side::Sell, Price{1'000'100}), 500u);

    // The same visible shares are taken again.
    EXPECT_EQ(arrive(order(2, Side::Buy, 1'000'100, 1000)),
              (std::vector{fill(2, Side::Buy, 1'000'000, 300, 700), fill(2, Side::Buy, 1'000'100, 500, 200),
                           expired(2, Side::Buy, 200)}));
    EXPECT_EQ(book_.digest(), digest);
}

TEST_F(ExecutionSimulatorTest, SellSweepsBidsBestFirstAtEachLevelsPrice)
{
    const std::uint64_t digest = book_.digest();

    EXPECT_EQ(arrive(order(1, Side::Sell, 999'800, 500)),
              (std::vector{fill(1, Side::Sell, 999'900, 300, 200), fill(1, Side::Sell, 999'800, 200, 0)}));
    EXPECT_EQ(book_.digest(), digest);
}

TEST_F(ExecutionSimulatorTest, PartialFillThenExpiredWhenTheNextLevelDoesNotCross)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'000, 400)),
              (std::vector{fill(1, Side::Buy, 1'000'000, 300, 100), expired(1, Side::Buy, 100)}));
}

TEST_F(ExecutionSimulatorTest, NoFillWhenTheLimitDoesNotCross)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 999'999, 100)), (std::vector{expired(1, Side::Buy, 100)}));
    EXPECT_EQ(arrive(order(2, Side::Sell, 999'901, 100)), (std::vector{expired(2, Side::Sell, 100)}));
}

TEST_F(ExecutionSimulatorTest, EmptyOppositeSideExpiresEverything)
{
    MarketBook bids_only{kSymbol, 4};
    ASSERT_EQ(bids_only.apply(add(1, Side::Buy, 999'900, 300)), ApplyResult::Applied);

    execution_.on_order_arrive(order(1, Side::Buy, 2'000'000, 100), kNow, bids_only, out_);
    EXPECT_EQ(out_, (std::vector{expired(1, Side::Buy, 100)}));
}

TEST_F(ExecutionSimulatorTest, RejectsZeroQuantity)
{
    EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'100, 0)),
              (std::vector{rejected(1, Side::Buy, 0, RejectReason::InvalidOrder)}));
}

TEST_F(ExecutionSimulatorTest, AppendsToOutAndCountsTakerFills)
{
    out_.push_back(expired(9, Side::Sell, 1));
    execution_.on_market(add(6, Side::Sell, 1'000'000, 100), book_, out_);  // nothing of ours rests
    execution_.on_order_arrive(order(1, Side::Buy, 1'000'100, 400), kNow, book_, out_);

    EXPECT_EQ(out_, (std::vector{expired(9, Side::Sell, 1), fill(1, Side::Buy, 1'000'000, 300, 100),
                                 fill(1, Side::Buy, 1'000'100, 100, 0)}));
    EXPECT_EQ(execution_.statistics().taker_fills, 2u);
    EXPECT_EQ(execution_.statistics().taker_quantity, 400u);
    EXPECT_EQ(execution_.open_orders(), 0u);  // an IOC never rests
}

constexpr std::int64_t kP = 1'000'000;  // $100.0000

MarketEvent at(Timestamp ts, MarketEvent event)
{
    event.timestamp = ts;
    return event;
}

MarketEvent reduce(MarketEventType type, OrderId id, std::uint64_t quantity = 0)
{
    return MarketEvent{.order_id = id, .quantity = Quantity{quantity}, .symbol = kSymbol, .type = type};
}

MarketEvent replace(OrderId id, OrderId new_id, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.order_id = id, .new_order_id = new_id, .price = Price{price},
                       .quantity = Quantity{quantity}, .symbol = kSymbol, .type = MarketEventType::Replace};
}

OrderRequest day(OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return order(id, side, price, quantity, TimeInForce::Day);
}

ExecutionReport accepted(OrderId id, Side side, std::uint64_t quantity)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .quantity = Quantity{quantity},
                           .leaves = Quantity{quantity}, .type = ReportType::Accepted, .side = side};
}

ExecutionReport maker(OrderId id, Side side, std::int64_t price, std::uint64_t quantity, std::uint64_t leaves,
                      Timestamp ts = kNow)
{
    return ExecutionReport{.exchange_time = ts, .order_id = id, .price = Price{price},
                           .quantity = Quantity{quantity}, .leaves = Quantity{leaves}, .type = ReportType::Fill,
                           .liquidity = Liquidity::Maker, .side = side};
}

ExecutionReport cancelled(OrderId id, Side side, std::uint64_t quantity)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .quantity = Quantity{quantity},
                           .type = ReportType::Cancelled, .side = side};
}

ExecutionReport cancel_rejected(OrderId id)
{
    return ExecutionReport{.exchange_time = kNow, .order_id = id, .type = ReportType::CancelRejected};
}

using Reports = std::vector<ExecutionReport>;

// An empty book; market() feeds the simulator, then the book, as the EventLoop does.
class RestingOrderTest : public ::testing::Test
{
protected:
    MarketBook book_{kSymbol, 64};
    ExecutionSimulator execution_;
    std::vector<ExecutionReport> out_;

    Reports market(const MarketEvent& event)
    {
        out_.clear();
        execution_.on_market(event, book_, out_);
        EXPECT_EQ(book_.apply(event), ApplyResult::Applied);
        return out_;
    }

    Reports arrive(const OrderRequest& request)
    {
        out_.clear();
        execution_.on_order_arrive(request, kNow, book_, out_);
        return out_;
    }

    Reports cancel(OrderId id)
    {
        out_.clear();
        execution_.on_cancel_arrive(id, kNow, out_);
        return out_;
    }

    std::uint64_t ahead(OrderId id) const
    {
        const ShadowOrder* o = execution_.find(id);
        return o == nullptr ? ~0ULL : o->ahead_quantity;
    }
};

// Rule (b) shrinks the queue ahead, executions ahead of us never fill us,
// then rule (a) does.
TEST_F(RestingOrderTest, WorkedExample2QueuePosition)
{
    EXPECT_EQ(market(add(1, Side::Buy, kP, 200)), Reports{});  // seq 1
    EXPECT_EQ(market(add(2, Side::Buy, kP, 300)), Reports{});  // seq 2

    EXPECT_EQ(arrive(day(1, Side::Buy, kP, 100)), (Reports{accepted(1, Side::Buy, 100)}));
    const ShadowOrder* o1 = execution_.find(1);
    ASSERT_NE(o1, nullptr);
    EXPECT_EQ(o1->anchor_sequence, 2u);
    EXPECT_EQ(o1->ahead_quantity, 500u);
    EXPECT_EQ(o1->remaining, 100u);

    EXPECT_EQ(market(add(3, Side::Buy, kP, 100)), Reports{});  // seq 3 > 2: behind us
    EXPECT_EQ(ahead(1), 500u);
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 1, 150)), Reports{});
    EXPECT_EQ(ahead(1), 350u);
    EXPECT_EQ(market(reduce(MarketEventType::Cancel, 2, 100)), Reports{});
    EXPECT_EQ(ahead(1), 250u);
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 1, 50)), Reports{});
    EXPECT_EQ(ahead(1), 200u);
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 2, 200)), Reports{});
    EXPECT_EQ(ahead(1), 0u);

    EXPECT_EQ(market(at(9'000, reduce(MarketEventType::Execute, 3, 60))),  // rule (a)
              (Reports{maker(1, Side::Buy, kP, 60, 40, 9'000)}));
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 3, 40)), (Reports{maker(1, Side::Buy, kP, 40, 0, 0)}));

    EXPECT_EQ(execution_.find(1), nullptr);
    EXPECT_EQ(execution_.open_orders(), 0u);
    EXPECT_EQ(execution_.statistics().maker_fills, 2u);
    EXPECT_EQ(execution_.statistics().maker_quantity, 100u);
    EXPECT_EQ(execution_.statistics().fifo_anomalies, 0u);
}

TEST_F(RestingOrderTest, DeleteAndReplaceOfOrdersAheadShrinkTheQueue)
{
    market(add(1, Side::Sell, kP, 200));
    market(add(2, Side::Sell, kP, 300));
    market(add(3, Side::Sell, kP + 100, 400));  // another price: never ahead of us
    arrive(day(1, Side::Sell, kP, 100));
    EXPECT_EQ(ahead(1), 500u);

    EXPECT_EQ(market(reduce(MarketEventType::Delete, 1)), Reports{});
    EXPECT_EQ(ahead(1), 300u);
    EXPECT_EQ(market(reduce(MarketEventType::Delete, 3)), Reports{});
    EXPECT_EQ(ahead(1), 300u);

    // h2 keeps its price under a new id: a fresh sequence, so now behind us.
    EXPECT_EQ(market(replace(2, 20, kP, 300)), Reports{});
    EXPECT_EQ(ahead(1), 0u);
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 20, 30)), (Reports{maker(1, Side::Sell, kP, 30, 70, 0)}));
}

// An inside-spread bid fills on a crossing Add: rule (c).
TEST_F(RestingOrderTest, WorkedExample3CrossingAddFillsAtOurPrice)
{
    market(add(1, Side::Buy, kP, 100));
    market(add(2, Side::Sell, kP + 200, 100));
    EXPECT_EQ(arrive(day(1, Side::Buy, kP + 100, 50)), (Reports{accepted(1, Side::Buy, 50)}));
    EXPECT_EQ(ahead(1), 0u);

    EXPECT_EQ(market(add(9, Side::Sell, kP + 100, 80)), (Reports{maker(1, Side::Buy, kP + 100, 50, 0, 0)}));
    EXPECT_EQ(book_.quantity_at(Side::Sell, Price{kP + 100}), 80u);  // the book never sees our fill
}

// Rule (d): a sell hitting the 1,000,000 bid trades through our 1,000,100 bid,
// so we fill first.
TEST_F(RestingOrderTest, RuleDExecuteAtAWorsePriceFillsUsFirst)
{
    market(add(1, Side::Buy, kP, 100));
    market(add(2, Side::Sell, kP + 200, 100));
    arrive(day(1, Side::Buy, kP + 100, 50));
    arrive(day(2, Side::Sell, kP + 100, 50));  // the mirror: inside the spread on the ask

    EXPECT_EQ(market(reduce(MarketEventType::Execute, 1, 30)), (Reports{maker(1, Side::Buy, kP + 100, 30, 20, 0)}));
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 2, 100)), (Reports{maker(2, Side::Sell, kP + 100, 50, 0, 0)}));

    // Not on a cancel or delete at the worse price, and not at a better one.
    EXPECT_EQ(market(reduce(MarketEventType::Cancel, 1, 10)), Reports{});
    market(add(3, Side::Buy, kP + 150, 100));  // better than our bid
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 3, 100)), Reports{});
    EXPECT_EQ(execution_.find(1)->remaining, 20u);
}

// Replace's replacement is an Add: it crosses our opposite side (c).
TEST_F(RestingOrderTest, CrossingReplacementFillsUs)
{
    market(add(1, Side::Sell, kP + 300, 100));
    arrive(day(1, Side::Buy, kP + 100, 50));

    EXPECT_EQ(market(replace(1, 2, kP + 100, 40)), (Reports{maker(1, Side::Buy, kP + 100, 40, 10, 0)}));
}

// One historical event fills ours best price first, then earliest, capped at its quantity.
TEST_F(RestingOrderTest, AllocationFollowsOurPriorityAndIsCappedAtTheEventQuantity)
{
    market(add(1, Side::Sell, kP + 500, 100));
    arrive(day(1, Side::Buy, kP, 60));
    arrive(day(2, Side::Buy, kP + 100, 60));   // better price, later arrival
    arrive(day(3, Side::Buy, kP + 100, 60));   // same price, later still
    arrive(day(4, Side::Sell, kP + 400, 60));  // other side: untouched
    EXPECT_EQ(execution_.open_orders(), 4u);

    EXPECT_EQ(market(add(2, Side::Sell, kP, 100)),
              (Reports{maker(2, Side::Buy, kP + 100, 60, 0, 0), maker(3, Side::Buy, kP + 100, 40, 20, 0)}));
    EXPECT_EQ(market(add(3, Side::Sell, kP, 30)),
              (Reports{maker(3, Side::Buy, kP + 100, 20, 0, 0), maker(1, Side::Buy, kP, 10, 50, 0)}));
    EXPECT_EQ(execution_.open_orders(), 2u);
    EXPECT_EQ(execution_.statistics().maker_quantity, 130u);
}

TEST_F(RestingOrderTest, PostOnlyRejectsWhatWouldCrossAndRestsOtherwise)
{
    market(add(1, Side::Buy, kP, 100));
    market(add(2, Side::Sell, kP + 100, 100));

    const ExecutionReport cross_buy{.exchange_time = kNow, .order_id = 1, .quantity = Quantity{10},
                                    .type = ReportType::Rejected, .reason = RejectReason::WouldCross,
                                    .side = Side::Buy};
    EXPECT_EQ(arrive(day(1, Side::Buy, kP + 100, 10)), Reports{cross_buy});  // equal counts
    ExecutionReport cross_sell = cross_buy;
    cross_sell.order_id = 2;
    cross_sell.side = Side::Sell;
    EXPECT_EQ(arrive(day(2, Side::Sell, kP - 100, 10)), Reports{cross_sell});
    EXPECT_EQ(execution_.statistics().would_cross_rejects, 2u);

    EXPECT_EQ(arrive(day(3, Side::Buy, kP + 99, 10)), (Reports{accepted(3, Side::Buy, 10)}));
    EXPECT_EQ(execution_.statistics().accepted_quantity, 10u);

    MarketBook empty{kSymbol, 4};
    out_.clear();
    execution_.on_order_arrive(day(4, Side::Buy, 2 * kP, 10), kNow, empty, out_);  // nothing to cross
    EXPECT_EQ(out_, (Reports{accepted(4, Side::Buy, 10)}));
}

TEST_F(RestingOrderTest, CancelRemovesTheRestAndRejectsWhatIsGone)
{
    market(add(1, Side::Sell, kP + 100, 100));
    arrive(day(1, Side::Buy, kP, 100));
    arrive(day(2, Side::Buy, kP, 30));
    market(add(2, Side::Sell, kP, 60));  // 60 of order 1, leaves 40

    EXPECT_EQ(cancel(1), (Reports{cancelled(1, Side::Buy, 40)}));
    EXPECT_EQ(cancel(1), (Reports{cancel_rejected(1)}));  // already cancelled

    market(add(3, Side::Sell, kP, 30));  // fills 2 completely
    EXPECT_EQ(cancel(2), (Reports{cancel_rejected(2)}));  // too late: the fill stands
    EXPECT_EQ(cancel(99), (Reports{cancel_rejected(99)}));  // never seen
    EXPECT_EQ(execution_.open_orders(), 0u);
    EXPECT_EQ(execution_.statistics().cancelled, 1u);
    EXPECT_EQ(execution_.statistics().cancel_rejects, 3u);
}

TEST_F(RestingOrderTest, RuleAWhileOrdersRemainAheadIsAFifoAnomaly)
{
    market(add(1, Side::Buy, kP, 200));
    arrive(day(1, Side::Buy, kP, 100));
    market(add(2, Side::Buy, kP, 100));  // behind us

    EXPECT_EQ(market(reduce(MarketEventType::Execute, 2, 50)), (Reports{maker(1, Side::Buy, kP, 50, 50, 0)}));
    EXPECT_EQ(ahead(1), 200u);
    EXPECT_EQ(execution_.statistics().fifo_anomalies, 1u);
}

// ITCH C: an Execute that carries its own price.
MarketEvent executed_at(OrderId id, std::uint64_t quantity, std::int64_t price)
{
    MarketEvent event = reduce(MarketEventType::Execute, id, quantity);
    event.price = Price{price};
    return event;
}

// The full-day SPY anomaly (2019-12-30, 09:30:09.776): h2, displayed at our
// ask behind us, executes via C one tick better, at $99.99. The buyer never
// reached $100.00, so we do not fill; the trade still shrinks the queue (b).
TEST_F(RestingOrderTest, ExecutionAtABetterPriceThanOursNeverFillsUs)
{
    market(add(1, Side::Sell, kP, 200));
    arrive(day(1, Side::Sell, kP, 100));
    market(add(2, Side::Sell, kP, 100));  // behind us
    arrive(day(2, Side::Buy, kP - 300, 100));
    market(add(3, Side::Buy, kP - 300, 100));  // behind our bid

    EXPECT_EQ(market(executed_at(2, 100, kP - 100)), Reports{});
    EXPECT_EQ(market(executed_at(3, 100, kP - 200)), Reports{});  // the mirror, on the bid
    EXPECT_EQ(market(executed_at(1, 50, kP - 100)), Reports{});   // ahead of us: rule (b) only
    EXPECT_EQ(ahead(1), 150u);
    EXPECT_EQ(execution_.statistics().maker_fills, 0u);
    EXPECT_EQ(execution_.statistics().fifo_anomalies, 0u);
}

// A C at our price or worse reached us: (a) and (d) fill as for E.
TEST_F(RestingOrderTest, ExecutionWithPriceAtOrWorseThanOursFillsUs)
{
    market(add(1, Side::Buy, kP, 100));
    market(add(2, Side::Buy, kP - 100, 100));
    arrive(day(1, Side::Buy, kP + 100, 100));
    market(add(3, Side::Buy, kP + 100, 100));  // behind us at our price

    EXPECT_EQ(market(executed_at(3, 10, kP + 100)), (Reports{maker(1, Side::Buy, kP + 100, 10, 90, 0)}));  // (a)
    EXPECT_EQ(market(executed_at(1, 20, kP + 100)), (Reports{maker(1, Side::Buy, kP + 100, 20, 70, 0)}));  // (d), equal
    EXPECT_EQ(market(executed_at(2, 30, kP)), (Reports{maker(1, Side::Buy, kP + 100, 30, 40, 0)}));        // (d), worse

    const ExecutionStatistics& stats = execution_.statistics();
    EXPECT_EQ(stats.fills_by[static_cast<std::size_t>(FillCause::RuleAExecuteWithPrice)]
                            [static_cast<std::size_t>(SessionPhase::PreOpen)], 1u);
    EXPECT_EQ(stats.fills_by[static_cast<std::size_t>(FillCause::RuleDExecuteWithPrice)]
                            [static_cast<std::size_t>(SessionPhase::PreOpen)], 2u);
}

// Every maker fill and FIFO anomaly is attributed to its rule, message and
// session phase, and the first anomalies are kept with their context.
TEST_F(RestingOrderTest, AttributesFillsAndAnomaliesByCauseAndPhase)
{
    constexpr Timestamp midnight = 1'000'000'000'000;
    constexpr Timestamp second = 1'000'000'000;
    ExecutionSimulator execution{midnight};
    static_assert(session_phase(midnight + RTH_OPEN_NS - 1, midnight) == SessionPhase::PreOpen);
    static_assert(session_phase(midnight + RTH_OPEN_NS, midnight) == SessionPhase::OpeningCross);
    static_assert(session_phase(midnight + RTH_OPEN_NS + second, midnight) == SessionPhase::Regular);
    static_assert(session_phase(midnight + RTH_CLOSE_NS, midnight) == SessionPhase::ClosingCross);
    static_assert(session_phase(midnight + RTH_CLOSE_NS + second, midnight) == SessionPhase::PostClose);

    auto step = [&](const MarketEvent& event) {
        out_.clear();
        execution.on_market(event, book_, out_);
        EXPECT_EQ(book_.apply(event), ApplyResult::Applied);
        return out_.size();
    };
    step(add(1, Side::Buy, kP, 200));
    out_.clear();
    execution.on_order_arrive(day(1, Side::Buy, kP, 100), kNow, book_, out_);
    step(add(2, Side::Buy, kP, 100));  // behind us

    const Timestamp regular = midnight + RTH_OPEN_NS + 2 * second;
    EXPECT_EQ(step(at(regular, reduce(MarketEventType::Execute, 2, 10))), 1u);  // (a) E with 200 ahead
    EXPECT_EQ(step(at(midnight + RTH_CLOSE_NS, add(3, Side::Sell, kP, 5))), 1u);  // (c) in the closing cross

    const ExecutionStatistics& stats = execution.statistics();
    const auto a_e = static_cast<std::size_t>(FillCause::RuleAExecute);
    const auto c_add = static_cast<std::size_t>(FillCause::RuleCAdd);
    EXPECT_EQ(stats.fills_by[a_e][static_cast<std::size_t>(SessionPhase::Regular)], 1u);
    EXPECT_EQ(stats.anomalies_by[a_e][static_cast<std::size_t>(SessionPhase::Regular)], 1u);
    EXPECT_EQ(stats.fills_by[c_add][static_cast<std::size_t>(SessionPhase::ClosingCross)], 1u);
    EXPECT_EQ(stats.anomalies_by[c_add][static_cast<std::size_t>(SessionPhase::ClosingCross)], 0u);
    EXPECT_EQ(stats.fifo_anomalies, 1u);

    ASSERT_EQ(execution.fifo_anomaly_samples().size(), 1u);
    const FifoAnomaly& sample = execution.fifo_anomaly_samples()[0];
    EXPECT_EQ(sample.event.timestamp, regular);
    EXPECT_EQ(sample.h_sequence, 2u);
    EXPECT_EQ(sample.order.anchor_sequence, 1u);
    EXPECT_EQ(sample.order.ahead_quantity, 200u);
    EXPECT_EQ(sample.order.remaining, 100u);  // before the fill
    EXPECT_EQ(sample.level_quantity, 300u);
}

// Events the book will not apply never touch our orders.
TEST_F(RestingOrderTest, IgnoresEventsTheBookRejects)
{
    market(add(1, Side::Buy, kP, 100));
    arrive(day(1, Side::Sell, kP + 100, 50));

    MarketEvent other_symbol = add(2, Side::Buy, kP + 100, 100);
    other_symbol.symbol = kSymbol + 1;
    out_.clear();
    execution_.on_market(other_symbol, book_, out_);
    execution_.on_market(add(1, Side::Buy, kP + 100, 100), book_, out_);       // duplicate id
    execution_.on_market(reduce(MarketEventType::Execute, 7, 10), book_, out_);  // unknown id
    execution_.on_market(replace(1, 1, kP + 100, 10), book_, out_);            // invalid replace
    EXPECT_EQ(out_, Reports{});
    EXPECT_EQ(execution_.find(1)->remaining, 50u);
}

TEST_F(ExecutionSimulatorTest, RejectsArrivalsWhileNotTrading)
{
    for (const TradingState state : {TradingState::Halted, TradingState::Paused, TradingState::QuotationOnly}) {
        ASSERT_EQ(book_.apply(status(state)), ApplyResult::Applied);
        EXPECT_EQ(arrive(order(1, Side::Buy, 1'000'100, 400)),  // would sweep two levels
                  (Reports{rejected(1, Side::Buy, 400, RejectReason::NotTrading)}));
        EXPECT_EQ(arrive(day(2, Side::Sell, 1'000'050, 100)),  // would rest
                  (Reports{rejected(2, Side::Sell, 100, RejectReason::NotTrading)}));
    }
    EXPECT_EQ(arrive(order(3, Side::Buy, 1'000'000, 0)),
              (Reports{rejected(3, Side::Buy, 0, RejectReason::InvalidOrder)}));  // checked first

    EXPECT_EQ(execution_.statistics().not_trading_rejects, 6u);
    EXPECT_EQ(execution_.statistics().taker_fills, 0u);
    EXPECT_EQ(execution_.open_orders(), 0u);

    ASSERT_EQ(book_.apply(status(TradingState::Trading)), ApplyResult::Applied);
    EXPECT_EQ(arrive(order(4, Side::Buy, 1'000'000, 150)), (Reports{fill(4, Side::Buy, 1'000'000, 150, 0)}));
}

// Halted, the book may cross and orders may still execute in the feed: no
// rule (a), (c) or (d) fill, while rule (b) keeps our queue position.
TEST_F(RestingOrderTest, NoFillsWhileNotTradingButTheQueueStillMoves)
{
    market(add(1, Side::Buy, kP, 200));        // h1, ahead of us
    market(add(2, Side::Buy, kP - 100, 100));  // h2, a worse bid
    EXPECT_EQ(arrive(day(1, Side::Buy, kP, 100)), (Reports{accepted(1, Side::Buy, 100)}));
    EXPECT_EQ(ahead(1), 200u);

    EXPECT_EQ(market(status(TradingState::Halted)), Reports{});
    EXPECT_EQ(market(add(3, Side::Sell, kP - 100, 50)), Reports{});         // crosses us: no (c)
    EXPECT_EQ(market(add(4, Side::Buy, kP, 40)), Reports{});                // behind us
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 4, 40)), Reports{});  // no (a)
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 2, 50)), Reports{});  // no (d)
    EXPECT_EQ(market(replace(3, 5, kP - 200, 30)), Reports{});              // no (c) on a replacement
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 1, 150)), Reports{});
    EXPECT_EQ(ahead(1), 50u);                                               // (b) still runs
    EXPECT_EQ(execution_.find(1)->remaining, 100u);

    EXPECT_EQ(market(status(TradingState::Trading)), Reports{});
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 1, 50)), Reports{});
    EXPECT_EQ(ahead(1), 0u);
    EXPECT_EQ(market(reduce(MarketEventType::Execute, 2, 50)), (Reports{maker(1, Side::Buy, kP, 50, 50, 0)}));  // (d)
    EXPECT_EQ(execution_.statistics().fifo_anomalies, 0u);
}

} // namespace
} // namespace simulator
