#include "simulator/matching/matching_engine.hpp"

#include <algorithm>

namespace simulator
{

    std::size_t MatchingEngine::submit(
        Order &order,
        std::vector<Trade> &trades,
        std::vector<Order *> &filled_orders)
    {
        const std::size_t initial_trade_count = trades.size();

        if (order.state() != OrderState::Active &&
            order.state() != OrderState::PartiallyFilled)
        {
            return 0;
        }

        while (!order.remaining_quantity().is_zero())
        {
            Order *resting = nullptr;

            if (order.side() == Side::Buy)
            {
                resting = book_.best_ask();

                if (resting == nullptr)
                {
                    break;
                }

                if (order.price() < resting->price())
                {
                    break;
                }
            }

            else
            {
                resting = book_.best_bid();

                if (resting == nullptr)
                {
                    break;
                }

                if (order.price() > resting->price())
                {
                    break;
                }
            }

            const Quantity executed =
                std::min(
                    order.remaining_quantity(),
                    resting->remaining_quantity());

            order.execute(executed);
            resting->execute(executed);

            trades.push_back(
                Trade{
                    order.side() == Side::Buy
                        ? order.id()
                        : resting->id(),

                    order.side() == Side::Buy
                        ? resting->id()
                        : order.id(),

                    resting->price(),
                    executed,
                    order.timestamp()});

            if (resting->state() == OrderState::Filled)
            {
                const OrderId resting_id = resting->id();

                if (book_.remove(resting_id))
                {
                    filled_orders.push_back(resting);
                }
            }

            if (order.remaining_quantity().is_zero())
            {
                break;
            }
        }

        if (!order.remaining_quantity().is_zero())
        {
            book_.add(order);
        }
        else
        {
            filled_orders.push_back(&order);
        }

        return trades.size() - initial_trade_count;
    }

} // namespace simulator
