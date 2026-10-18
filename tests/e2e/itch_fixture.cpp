#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "itch/itch_test_builder.hpp"

// Writes DIR/12302019.NASDAQ_ITCH50.bin, a small ITCH day of one stock, ACME, for the web end-to-end tests:
// - 09:30:00: 300 bid at 10.00, then 300 offered at 10.01, then 100 of the offer trade, one event at a time;
// - from 09:30:01, one print a second, each between 10.00 and 10.04, with a halt from 09:31:00 to 09:31:30.
int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: chronos-e2e-fixture DIR\n");
        return 2;
    }
    constexpr std::uint64_t second = 1'000'000'000;
    constexpr std::uint64_t open = 34'200 * second;  // 09:30:00 ET, in ns since midnight
    simulator::itch_test::ItchBuilder itch;
    itch.stock_directory(1, 3 * 3'600 * second, "ACME")
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
    const std::filesystem::path dir{argv[1]};
    std::filesystem::create_directories(dir);
    std::ofstream out{dir / "12302019.NASDAQ_ITCH50.bin", std::ios::binary};
    const auto bytes = itch.bytes();
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return out ? 0 : 1;
}
