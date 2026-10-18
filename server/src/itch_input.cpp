#include "chronos/server/itch_input.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/market/vector_source.hpp"

namespace simulator::server {
namespace {

// An ItchSource that owns the registry it registers its one instrument in.
class TickerSource final : public MarketEventSource
{
private:
    InstrumentRegistry registry_{1};
    ItchSource source_;

public:
    TickerSource(std::span<const std::byte> bytes, const ItchSourceConfig& config)
        : source_(bytes, config, registry_)
    {}

    SourceResult next(MarketEvent& event) noexcept override
    {
        return source_.next(event);
    }

    std::uint64_t position() const noexcept override
    {
        return source_.position();
    }

    SourceError error() const noexcept override
    {
        return source_.error();
    }
};

}  // namespace

std::vector<std::string> symbol_directory(std::span<const std::byte> bytes)
{
    std::vector<std::string> tickers;
    ItchMessageReader reader{bytes};
    ItchDecoder decoder;
    ItchMessage message;
    ItchStockDirectory directory;
    while (reader.next(message) == ItchReadResult::Message) {
        if (message.type == 'S' && message.payload.size() == 11 && message.payload[10] == std::byte{'S'}) {
            break;  // the start of system hours: the day's directory is complete
        }
        if (message.type == 'R' && decoder.decode_stock_directory(message, directory) == ItchDecodeResult::Success) {
            const std::string_view stock{directory.stock.data(), directory.stock.size()};
            tickers.emplace_back(stock.substr(0, stock.find_last_not_of(' ') + 1));
        }
    }
    std::sort(tickers.begin(), tickers.end());
    return tickers;
}

std::string input_fingerprint(std::string_view name, std::span<const std::byte> bytes)
{
    const std::size_t edge = std::min(std::size_t{1} << 20, bytes.size());
    std::uint64_t hash = fnv1a_offset_basis;
    for (const std::span<const std::byte> part : {bytes.first(edge), bytes.last(edge)}) {
        for (const std::byte byte : part) {
            hash ^= std::to_integer<std::uint64_t>(byte);
            hash *= 1099511628211ULL;  // FNV-1a 64 prime
        }
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016" PRIx64, hash);
    return std::string{name} + " " + std::to_string(bytes.size()) + " " + hex;
}

SourceFactory itch_factory(std::span<const std::byte> bytes, std::chrono::year_month_day date,
                           std::chrono::minutes utc_offset, bool allow_truncated_tail)
{
    return [=](std::string_view ticker) -> std::unique_ptr<MarketEventSource> {
        if (ticker.empty()) {
            return std::make_unique<VectorSource>(std::span<const MarketEvent>{});
        }
        return std::make_unique<TickerSource>(bytes,
                                              ItchSourceConfig{.ticker = ticker,
                                                               .session_date = date,
                                                               .utc_offset = utc_offset,
                                                               .allow_truncated_tail = allow_truncated_tail});
    };
}

}  // namespace simulator::server
