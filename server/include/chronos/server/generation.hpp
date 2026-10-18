#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "chronos/server/command.hpp"
#include "chronos/server/session_strategy.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/market/instrument_registry.hpp"

namespace simulator::server {

// Builds a fresh market stream for each generation of a session.
using SourceFactory = std::function<std::unique_ptr<MarketEventSource>()>;

// What every generation of a session shares.
struct SessionConfig
{
    std::string session_id{};
    std::string input{};  // identifies the market data in the command log's header
    Instrument instrument{};
    SymbolId symbol{0};
    std::size_t book_capacity{std::size_t{1} << 20};
    LatencyConfig latency{};
    RiskLimits limits{};
    FeeSchedule fees{};
    ReplayOptions options{};
    std::uint64_t source_size{0};  // the source's position() at the end of its stream; 0 if unknown
};

// One generation of a session: a fresh source, book, simulator, portfolio and strategy under one EventLoop.
struct Generation
{
    std::unique_ptr<MarketEventSource> source;
    MarketBook book;
    ExecutionSimulator execution;
    Portfolio portfolio;
    SessionStrategy strategy;
    EventLoop<SessionStrategy> loop;

    Generation(const SessionConfig& config, std::unique_ptr<MarketEventSource> market, const StrategySpec& spec);
};

struct Applied
{
    ErrorCode error{ErrorCode::None};
    OrderId order_id{0};  // the new order's id, or the cancelled one's
};

// Applies one command at the loop's clock through the gateway strategies use.
Applied apply_command(Generation& generation, const Command& command) noexcept;

}  // namespace simulator::server
