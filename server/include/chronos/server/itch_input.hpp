#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "chronos/server/generation.hpp"

namespace simulator::server {

// The tickers of the stock directory (R) messages before the start of system hours (S 'S'), sorted.
// ponytail: an R after the start of system hours (an intraday listing) is left out; scan the whole day if one matters.
std::vector<std::string> symbol_directory(std::span<const std::byte> bytes);

// Names the market data in the command log: "<name> <size in bytes> <FNV-1a 64 of the first and last MiB, hex>".
std::string input_fingerprint(std::string_view name, std::span<const std::byte> bytes);

// One ITCH day's streams: per ticker, an ItchSource with its own one-instrument registry; for the empty ticker, an
// empty stream. Never throws but for memory. The caller keeps `bytes` alive.
SourceFactory itch_factory(std::span<const std::byte> bytes, std::chrono::year_month_day date,
                           std::chrono::minutes utc_offset, bool allow_truncated_tail);

}  // namespace simulator::server
