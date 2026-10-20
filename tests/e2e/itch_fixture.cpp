#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "itch/itch_test_builder.hpp"

namespace {

using simulator::itch_test::ItchBuilder;

constexpr std::uint64_t second = 1'000'000'000;
constexpr std::uint64_t hour = 3'600 * second;
constexpr std::uint64_t open = 34'200 * second;  // 09:30:00 ET, in ns since midnight

// The terminal tests' day of one stock, ACME, with regular hours from 09:30 to 16:00 as its System Events set them:
// - 09:30:00: 300 bid at 10.00, then 300 offered at 10.01, then 100 of the offer trade, one event at a time;
// - from 09:30:01, one print a second, each between 10.00 and 10.04, with a halt from 09:31:00 to 09:31:30.
ItchBuilder terminal_day()
{
    ItchBuilder itch;
    itch.system_event(0, 3 * hour, 'O')
        .stock_directory(1, 3 * hour, "ACME")
        .system_event(0, open, 'Q')
        .add_order(1, open, 1, 'B', 300, "ACME", 100'000)
        .add_order(1, open + 1'000'000, 2, 'S', 300, "ACME", 100'100)
        .executed(1, open + 2'000'000, 2, 100, 1);
    for (std::uint64_t s = 1; s < 120; ++s) {
        if (s == 60 || s == 90) {
            itch.trading_action(1, open + s * second, "ACME", s == 60 ? 'H' : 'T');
        } else if (s < 60 || s > 90) {
            itch.trade(1, open + s * second, 0, 'B', 100, "ACME", 100'000 + s * 7 % 5 * 100, 1 + s);
        }
    }
    itch.system_event(0, 16 * hour, 'M');
    return itch;
}

// A Lab day: ACME's book of 10.00 x 10.01 from 09:30, then `cycles` ten-second cycles, in each of which the default
// market maker buys 100 at 10.00 by rule (a), sees the bid drop a tick, and sells 100 at 10.01 by rule (c): $1 each.
// With `zeta`, a second stock is listed, and never trades.
ItchBuilder lab_day(std::uint64_t cycles, bool zeta)
{
    ItchBuilder itch;
    itch.system_event(0, 3 * hour, 'O').stock_directory(1, 3 * hour, "ACME");
    if (zeta) {
        itch.stock_directory(2, 3 * hour, "ZETA");
    }
    constexpr std::uint64_t ms = 1'000'000;
    itch.system_event(0, open, 'Q')
        .add_order(1, open, 1, 'B', 300, "ACME", 100'000)
        .add_order(1, open + ms, 2, 'S', 300, "ACME", 100'100)
        .add_order(1, open + 2 * ms, 3, 'B', 300, "ACME", 99'900)
        .add_order(1, open + 3 * ms, 4, 'S', 300, "ACME", 100'200);
    std::uint64_t bid = 1;  // the order bidding 10.00 ahead of the quote
    for (std::uint64_t k = 0; k < cycles; ++k) {
        const std::uint64_t at = open + (1 + 10 * k) * second;
        const std::uint64_t id = 10 * (k + 1);
        // (a): an execution of a bid queued behind the quote's bid fills it.
        itch.add_order(1, at, id, 'B', 200, "ACME", 100'000).executed(1, at + second, id, 100, 1 + k);
        // The bid drops to 9.99, marking the long down $1.
        itch.erase(1, at + 2 * second, bid).erase(1, at + 2 * second + ms, id);
        // (c): a bid added at the offer crosses the quote's offer.
        itch.add_order(1, at + 3 * second, id + 1, 'B', 100, "ACME", 100'100).erase(1, at + 4 * second, id + 1);
        // The bid is back at 10.00.
        itch.add_order(1, at + 5 * second, id + 2, 'B', 300, "ACME", 100'000);
        bid = id + 2;
    }
    itch.system_event(0, 16 * hour, 'M');
    return itch;
}

bool write(const std::filesystem::path& file, const ItchBuilder& itch)
{
    std::ofstream out{file, std::ios::binary};
    const auto bytes = itch.bytes();
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

}  // namespace

// Writes DIR/12272019.NASDAQ_ITCH50.bin and DIR/12302019.NASDAQ_ITCH50.bin, a Friday and a Monday, for the web
// end-to-end tests: the terminal's two alike, or with --lab the Lab's, two cycles on Friday and three on Monday,
// whose stock directory alone lists ZETA.
int main(int argc, char** argv)
{
    const bool lab = argc == 3 && std::string_view{argv[2]} == "--lab";
    if (argc != 2 && !lab) {
        std::fprintf(stderr, "usage: chronos-e2e-fixture DIR [--lab]\n");
        return 2;
    }
    const std::filesystem::path dir{argv[1]};
    std::filesystem::create_directories(dir);
    const bool friday = write(dir / "12272019.NASDAQ_ITCH50.bin", lab ? lab_day(2, false) : terminal_day());
    const bool monday = write(dir / "12302019.NASDAQ_ITCH50.bin", lab ? lab_day(3, true) : terminal_day());
    return friday && monday ? 0 : 1;
}
