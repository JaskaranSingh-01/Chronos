#include <iostream>

#include "simulator/clock/clock.hpp"
#include "simulator/events/event.hpp"

int main(){
	using namespace simulator;
	Clock clock;
	clock.advance_to(1000);

    Event event{
        .timestamp = clock.now(),
        .type = EventType::AddOrder,
        .order_id = 1,
        .side = Side::Buy,
        .price = Price{10000},
        .quantity = Quantity{100}
    };

    std::cout << "Simulator initialized\n";
    std::cout << "Timestamp: " << event.timestamp << '\n';
    std::cout << "Order ID: " << event.order_id << '\n';
    std::cout << "Price ticks: " << event.price.ticks << '\n';
    std::cout << "Quantity: " << event.quantity.value << '\n';

    return 0;

}