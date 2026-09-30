#include "simulator/matching/matching_engine.hpp"

#include <algorithm>

namespace simulator
{

    std::vector<Trade> MatchingEngine::submit(Order &order)
    {

        std::vector<Trade> trades;

        if (order.state() != OrderState::Active)
        {
            return trades;
        }

        if (order.side() == Side::Buy)
        {
            Order *resting = book_.best_ask();
            if (resting == nullptr)
            {
                book_.add(order);
                return trades;
            }

            if (order.price() < resting->price())
            {
                book_.add(order);
                return trades;
            }

            const Quantity executed = std::min(order.remaining_quantity(), resting->remaining_quantity());

            order.execute(executed);
            resting->execute(executed);
            trades.push_back(
                Trade{
                    order.id(),
                    resting->id(),
                    resting->price(),
                    executed,
                    order.timestamp()});

            if (resting->state() == OrderState::Filled)
            {
                book_.remove(resting->id());
            }

            if (order.state() != OrderState::Filled)
            {
                book_.add(order);
            }

            return trades;
        }
        Order *resting = book_.best_bid();

        if (resting == nullptr)
        {
            book_.add(order);
            return trades;
        }

        if (order.price() > resting->price())
        {
            book_.add(order);
            return trades;
        }

        const Quantity executed =
            std::min(
                order.remaining_quantity(),
                resting->remaining_quantity());

        order.execute(executed);
        resting->execute(executed);

        trades.push_back(
            Trade{
                resting->id(),
                order.id(),
                resting->price(),
                executed,
                order.timestamp()});


        if (resting->state() == OrderState::Filled)
        {
            book_.remove(resting->id());
        }

        if (order.state() != OrderState::Filled)
        {
            book_.add(order);
        }

        return trades;
    }

} // namespace simulator