#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "simulator/engine/strategy_registry.hpp"

namespace simulator::server {

// A strategy written in the Strategy Lab: Lua 5.4 source, run in a sandbox (docs/strategies.md). Its strategy name is
// "script:" and the name it was saved under.
inline constexpr std::string_view script_prefix = "script:";
// A script's source travels as JSON in a request body, which the server caps at 64 KiB.
inline constexpr std::size_t max_script_bytes = 48 * 1024;

// Every script may run this many Lua instructions in one call (the top level, or one callback), and hold this much
// memory; past either it stops with an error. Both deterministic, so a script stops at the same place every run.
inline constexpr std::uint64_t script_instruction_budget = 10'000'000;
inline constexpr std::size_t script_memory_cap = 64 * 1024 * 1024;

// The log lines a run keeps from a script's log(), and the stats from its stat().
inline constexpr std::size_t max_script_log_lines = 200;
inline constexpr std::size_t max_script_stats = 32;

// A script's declarations, from running its top level once in the sandbox; with `params`, param() returns them, so
// limits that follow a param are those of a run with these params.
struct ScriptInfo
{
    std::string error{};  // why it does not load, e.g. "script:3: '=' expected near 'x'"; empty if it does
    std::string summary{};
    std::vector<ParamInfo> params{};  // in the order it declares them
    RiskLimits limits{};
};

ScriptInfo describe_script(std::string_view source, const ParamList* params = nullptr);

// The strategy `source` defines, run with `params` (resolved against its declarations). A script that fails to load
// gives a strategy that has stopped, with the reason in error().
std::unique_ptr<AnyStrategy> make_script_strategy(std::string_view source, const ParamList& params,
                                                  const StrategySetup& setup);

// Whether `name` is a script's strategy name: "script:" and a valid strategy name.
bool is_script_name(std::string_view name) noexcept;

// The scripts saved from the Strategy Lab, each <dir>/<name>.lua, written through a temp file and a rename. Safe to
// use from any thread.
class ScriptStore
{
private:
    std::filesystem::path dir_;
    mutable std::mutex mutex_;

public:
    static constexpr std::size_t max_scripts = 200;

    explicit ScriptStore(std::filesystem::path dir);

    // The saved scripts' names, sorted.
    std::vector<std::string> names() const;
    // False if there is no script `name`.
    bool read(std::string_view name, std::string& source) const;
    // Saves `source` as `name`, a valid strategy name; false, with `error` ("too_many" or a file error), if it cannot.
    bool write(std::string_view name, std::string_view source, std::string& error);
    // False if there was no script `name`.
    bool remove(std::string_view name);
};

}  // namespace simulator::server
