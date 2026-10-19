#include "chronos/server/command_log.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "build_info.hpp"  // generated: build_commit, build_dirty
#include "json_enums.hpp"

namespace simulator::server {

namespace {

using nlohmann::json;

json strategy_json(const StrategySpec& spec)
{
    const MarketMakerParams& mm = spec.market_maker;
    const ImbalanceTakerParams& it = spec.imbalance;
    return {{"kind", spec.kind},
            {"market_maker",
             {{"quote_size", mm.quote_size},
              {"max_position", mm.max_position},
              {"requote_cooldown_ns", mm.requote_cooldown_ns},
              {"improve_ticks", mm.improve_ticks}}},
            {"imbalance",
             {{"threshold", it.threshold},
              {"clip", it.clip},
              {"max_position", it.max_position},
              {"cooldown_ns", it.cooldown_ns}}}};
}

StrategySpec strategy_from(const json& j)
{
    const json& mm = j.at("market_maker");
    const json& it = j.at("imbalance");
    return StrategySpec{.kind = j.at("kind").get<SessionStrategyKind>(),
                        .market_maker = {.quote_size = mm.at("quote_size").get<std::uint64_t>(),
                                         .max_position = mm.at("max_position").get<std::int64_t>(),
                                         .requote_cooldown_ns = mm.at("requote_cooldown_ns").get<Timestamp>(),
                                         .improve_ticks = mm.at("improve_ticks").get<std::int64_t>()},
                        .imbalance = {.threshold = it.at("threshold").get<double>(),
                                      .clip = it.at("clip").get<std::uint64_t>(),
                                      .max_position = it.at("max_position").get<std::int64_t>(),
                                      .cooldown_ns = it.at("cooldown_ns").get<Timestamp>()}};
}

json command_json(const Command& c)
{
    return {{"generation", c.generation},
            {"type", c.type},
            {"order_type", c.order_type},
            {"side", c.side},
            {"price", c.price.ticks},
            {"quantity", c.quantity.value()},
            {"client_id", c.client_id},
            {"order_id", c.order_id},
            {"strategy", strategy_json(c.strategy)}};
}

Command command_from(const json& line)
{
    const json& payload = line.at("payload");
    return Command{.command_seq = line.at("command_seq").get<std::uint64_t>(),
                   .generation = payload.at("generation").get<std::uint64_t>(),
                   .received_wall_ts = line.at("received_wall_ts").get<Timestamp>(),
                   .type = payload.at("type").get<CommandType>(),
                   .order_type = payload.at("order_type").get<OrderType>(),
                   .side = payload.at("side").get<Side>(),
                   .price = Price{payload.at("price").get<std::int64_t>()},
                   .quantity = Quantity{payload.at("quantity").get<std::uint64_t>()},
                   .client_id = payload.at("client_id").get<std::uint64_t>(),
                   .order_id = payload.at("order_id").get<OrderId>(),
                   .strategy = strategy_from(payload.at("strategy"))};
}

// The header line of the generation `start` (a GenerationStart record) begins.
json header_json(const SessionConfig& config, const Record& start, const std::string& commit, bool dirty)
{
    return {
        {"type", "header"},
        {"protocol", log_protocol},
        {"session_id", config.session_id},
        {"generation", start.generation},
        {"commit", commit},
        {"dirty", dirty},
        {"input", config.input},
        {"ticker", std::string{name(start.control.symbol)}},
        {"symbol", config.symbol},
        {"book_capacity", config.book_capacity},
        {"instrument",
         {{"price_scale", config.instrument.price_scale}, {"quantity_scale", config.instrument.quantity_scale}}},
        {"latency", {{"order_ns", config.latency.order_latency}, {"report_ns", config.latency.report_latency}}},
        {"fees",
         {{"maker_per_share", config.fees.maker_fee_per_share}, {"taker_per_share", config.fees.taker_fee_per_share}}},
        {"risk",
         {{"max_position", config.limits.max_position},
          {"max_order_quantity", config.limits.max_order_quantity.value()},
          {"max_open_orders", config.limits.max_open_orders}}},
        {"session_midnight", config.options.session_midnight},
        {"check_invariants_every", config.options.check_invariants_every},
        {"strategy", strategy_json(start.command.strategy)}};
}

SessionConfig config_from(const json& header)
{
    const json& instrument = header.at("instrument");
    const json& latency = header.at("latency");
    const json& fees = header.at("fees");
    const json& risk = header.at("risk");
    return SessionConfig{.session_id = header.at("session_id").get<std::string>(),
                         .input = header.at("input").get<std::string>(),
                         .instrument = {.price_scale = instrument.at("price_scale").get<std::uint8_t>(),
                                        .quantity_scale = instrument.at("quantity_scale").get<std::uint8_t>()},
                         .symbol = header.at("symbol").get<SymbolId>(),
                         .book_capacity = header.at("book_capacity").get<std::size_t>(),
                         .latency = {.order_latency = latency.at("order_ns").get<Timestamp>(),
                                     .report_latency = latency.at("report_ns").get<Timestamp>()},
                         .limits = {.max_position = risk.at("max_position").get<std::int64_t>(),
                                    .max_order_quantity = Quantity{risk.at("max_order_quantity").get<std::uint64_t>()},
                                    .max_open_orders = risk.at("max_open_orders").get<std::size_t>()},
                         .fees = {.maker_fee_per_share = fees.at("maker_per_share").get<std::int64_t>(),
                                  .taker_fee_per_share = fees.at("taker_per_share").get<std::int64_t>()},
                         .options = {.check_invariants_every = header.at("check_invariants_every").get<std::uint64_t>(),
                                     .session_midnight = header.at("session_midnight").get<Timestamp>()}};
}

// The line for a command step, a report or a trailer; a GenerationStart is a header, which needs the config.
json record_json(const Record& r)
{
    if (r.kind == RecordKind::GenerationEnd) {
        return {{"type", "trailer"},
                {"seq", r.seq},
                {"generation", r.generation},
                {"exchange_ts", r.exchange_ts},
                {"market_event_seq", r.market_event_seq},
                {"book_digest", r.digests.book},
                {"execution_digest", r.digests.execution},
                {"portfolio_digest", r.digests.portfolio}};
    }
    if (r.kind == RecordKind::Report) {
        return {{"type", "report"},
                {"seq", r.seq},
                {"generation", r.generation},
                {"order_id", r.report.order_id},
                {"exchange_time", r.report.exchange_time},
                {"report_type", r.report.type},
                {"price", r.report.price.ticks},
                {"quantity", r.report.quantity.value()},
                {"leaves", r.report.leaves.value()},
                {"reject_reason", r.report.reason},
                {"liquidity", r.report.liquidity},
                {"side", r.report.side}};
    }
    return {{"type", "command"},
            {"seq", r.seq},
            {"generation", r.generation},
            {"step", r.step},
            {"reason", to_string(r.reason)},
            {"command_seq", r.command.command_seq},
            {"received_wall_ts", r.command.received_wall_ts},
            {"batch_seq", r.batch_seq},
            {"exchange_ts", r.exchange_ts},
            {"market_event_seq", r.market_event_seq},
            {"order_id", r.order_id},
            {"payload", command_json(r.command)}};
}

std::string dump_line(const json& line)
{
    return line.dump(-1, ' ', false, json::error_handler_t::replace);
}

// The scan below ends at CancelRejected: an ErrorCode added after it must extend the scan.
static_assert(to_string(static_cast<ErrorCode>(static_cast<std::uint8_t>(ErrorCode::CancelRejected) + 1)) == "unknown");

// The ErrorCode named `name`; an unknown name is None, which read_log's round trip then refuses.
ErrorCode error_from(const std::string& name) noexcept
{
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(ErrorCode::CancelRejected); ++i) {
        if (to_string(static_cast<ErrorCode>(i)) == name) {
            return static_cast<ErrorCode>(i);
        }
    }
    return ErrorCode::None;
}

Record command_record_from(const json& line)
{
    return Record{.seq = line.at("seq").get<std::uint64_t>(),
                  .generation = line.at("generation").get<std::uint64_t>(),
                  .step = line.at("step").get<Lifecycle>(),
                  .reason = error_from(line.at("reason").get<std::string>()),
                  .command = command_from(line),
                  .batch_seq = line.at("batch_seq").get<std::uint64_t>(),
                  .exchange_ts = line.at("exchange_ts").get<Timestamp>(),
                  .market_event_seq = line.at("market_event_seq").get<std::uint64_t>(),
                  .order_id = line.at("order_id").get<OrderId>()};
}

Record trailer_record_from(const json& line)
{
    return Record{.seq = line.at("seq").get<std::uint64_t>(),
                  .generation = line.at("generation").get<std::uint64_t>(),
                  .kind = RecordKind::GenerationEnd,
                  .exchange_ts = line.at("exchange_ts").get<Timestamp>(),
                  .market_event_seq = line.at("market_event_seq").get<std::uint64_t>(),
                  .digests = {.book = line.at("book_digest").get<std::uint64_t>(),
                              .execution = line.at("execution_digest").get<std::uint64_t>(),
                              .portfolio = line.at("portfolio_digest").get<std::uint64_t>()}};
}

Record report_record_from(const json& line)
{
    return Record{.seq = line.at("seq").get<std::uint64_t>(),
                  .generation = line.at("generation").get<std::uint64_t>(),
                  .kind = RecordKind::Report,
                  .order_id = line.at("order_id").get<OrderId>(),
                  .report = {.exchange_time = line.at("exchange_time").get<Timestamp>(),
                             .order_id = line.at("order_id").get<OrderId>(),
                             .price = Price{line.at("price").get<std::int64_t>()},
                             .quantity = Quantity{line.at("quantity").get<std::uint64_t>()},
                             .leaves = Quantity{line.at("leaves").get<std::uint64_t>()},
                             .type = line.at("report_type").get<ReportType>(),
                             .reason = line.at("reject_reason").get<RejectReason>(),
                             .liquidity = line.at("liquidity").get<Liquidity>(),
                             .side = line.at("side").get<Side>()}};
}

// Flushes `fd` to the device: F_FULLFSYNC where it exists, as macOS fsync leaves the data in the drive's cache.
// ponytail: F_FULLFSYNC costs about 4 ms per synced line and an order is four lines, so --fsync sustains about
// 60 commands/s; group commit (one sync per drained run) when that matters.
bool sync_to_device(int fd) noexcept
{
#ifdef F_FULLFSYNC
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return true;
    }
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) {  // a filesystem without it; any other errno fails
        return false;
    }
#endif
    return ::fsync(fd) == 0;
}

// Syncs the directory holding `path`, so a file just created there survives a crash.
bool sync_directory_of(const std::filesystem::path& path)
{
    const std::filesystem::path dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }
    const bool synced = sync_to_device(fd);
    ::close(fd);
    return synced;
}

}  // namespace

bool LogQueue::push(const Record& record) noexcept
{
    {
        const std::lock_guard lock{mutex_};
        if (failed_.load() || records_.size() + reserved_ == capacity) {
            failed_.store(true);
            return false;
        }
        records_.push(record);
        depth_.store(records_.size(), std::memory_order_relaxed);
    }
    cv_.notify_one();
    return true;
}

bool LogQueue::reserve() noexcept
{
    const std::lock_guard lock{mutex_};
    if (failed_.load() || records_.size() + reserved_ == capacity) {
        failed_.store(true);
        return false;
    }
    ++reserved_;
    return true;
}

void LogQueue::push_reserved(const Record& record) noexcept
{
    {
        const std::lock_guard lock{mutex_};
        --reserved_;
        records_.push(record);
        depth_.store(records_.size(), std::memory_order_relaxed);
    }
    cv_.notify_one();
}

bool LogQueue::pop(Record& record)
{
    std::unique_lock lock{mutex_};
    cv_.wait(lock, [&] { return closed_ || !records_.empty(); });
    const bool popped = records_.pop(record);
    depth_.store(records_.size(), std::memory_order_relaxed);
    return popped;
}

void LogQueue::close()
{
    {
        const std::lock_guard lock{mutex_};
        closed_ = true;
    }
    cv_.notify_all();
}

void LogQueue::fail() noexcept
{
    failed_.store(true);
}

bool LogQueue::failed() const noexcept
{
    return failed_.load();
}

std::size_t LogQueue::size() const noexcept
{
    return depth_.load(std::memory_order_relaxed);
}

LogWriter::LogWriter(LogQueue& queue, const std::filesystem::path& path, SessionConfig config, bool fsync)
    : queue_(queue), config_(std::move(config)), file_(nullptr), fsync_(fsync)
{
    std::error_code ignored;
    const bool creating = fsync_ && !std::filesystem::exists(path, ignored);
    file_ = std::fopen(path.c_str(), "a");
    if (file_ == nullptr || (creating && !sync_directory_of(path))) {
        queue_.fail();
        return;
    }
    thread_ = std::thread{[this] { run(); }};
}

LogWriter::~LogWriter()
{
    queue_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

std::uint64_t LogWriter::persisted_seq() const noexcept
{
    return persisted_seq_.load();
}

void LogWriter::run()
{
    Record record;
    while (queue_.pop(record)) {
        const json line = record.kind == RecordKind::GenerationStart
            ? header_json(config_, record, build_commit, build_dirty)
            : record_json(record);
        const std::string text = dump_line(line) + '\n';
        const bool command = record.kind == RecordKind::Lifecycle;
        if (std::fwrite(text.data(), 1, text.size(), file_) != text.size() || std::fflush(file_) != 0 ||
            (fsync_ && command && !sync_to_device(::fileno(file_)))) {
            queue_.fail();
            return;
        }
        persisted_seq_.store(record.seq);
    }
}

bool read_log(const std::filesystem::path& path, std::uint64_t generation, LoggedGeneration& logged, std::string& error)
{
    std::ifstream in{path};
    if (!in) {
        error = "cannot open " + path.string();
        return false;
    }
    LoggedGeneration found;
    bool has_header = false;
    std::uint64_t last_seq = 0;  // of the previous record line; headers carry none
    std::string text;
    for (std::size_t number = 1; std::getline(in, text); ++number) {
        const std::string where = "line " + std::to_string(number) + ": ";
        const json line = json::parse(text, nullptr, false);
        if (line.is_discarded()) {
            error = where + "not JSON";
            return false;
        }
        try {
            Record record;
            json expected;  // what the writer would have written for the record just read
            const std::string type = line.at("type").get<std::string>();
            if (type == "header") {
                if (line.at("protocol").get<int>() != log_protocol) {
                    error = where + "unsupported log protocol";
                    return false;
                }
                record = Record{.generation = line.at("generation").get<std::uint64_t>(),
                                .kind = RecordKind::GenerationStart,
                                .command = {.strategy = strategy_from(line.at("strategy"))}};
                if (!to_ticker(line.at("ticker").get<std::string>(), record.control.symbol)) {
                    error = where + "unexpected field or value";
                    return false;
                }
                expected = header_json(config_from(line), record, line.at("commit").get<std::string>(),
                                       line.at("dirty").get<bool>());
            } else if (type == "command") {
                record = command_record_from(line);
                expected = record_json(record);
            } else if (type == "report") {
                record = report_record_from(line);
                expected = record_json(record);
            } else if (type == "trailer") {
                record = trailer_record_from(line);
                expected = record_json(record);
            } else {
                error = where + "unexpected field or value";
                return false;
            }
            // An enum name it does not know, a negative or fractional count, or an extra key all fail this.
            if (dump_line(expected) != text) {
                error = where + "unexpected field or value";
                return false;
            }
            if (record.kind != RecordKind::GenerationStart) {
                if (record.seq <= last_seq) {
                    error = where + "seq out of order";
                    return false;
                }
                last_seq = record.seq;
            }
            if (record.generation != generation) {
                continue;
            }
            if (record.kind == RecordKind::GenerationStart) {
                if (has_header) {
                    error = where + "a second header for generation " + std::to_string(generation);
                    return false;
                }
                has_header = true;
                found.ticker = record.control.symbol;
                found.start = record.command.strategy;
            } else if (record.kind == RecordKind::GenerationEnd) {
                if (found.end) {
                    error = where + "a second trailer for generation " + std::to_string(generation);
                    return false;
                }
                found.end = record;
            } else if (record.kind == RecordKind::Lifecycle && record.step == Lifecycle::Applied) {
                found.applied.push_back(record);
            }
        } catch (const json::exception& e) {
            error = where + e.what();
            return false;
        }
    }
    if (!has_header) {
        error = "no header for generation " + std::to_string(generation);
        return false;
    }
    logged = std::move(found);
    return true;
}

bool reproduce(Generation& generation, const LoggedGeneration& logged) noexcept
{
    generation.strategy.attach(logged.start);
    EventLoop<SessionStrategy>& loop = generation.loop;
    Timestamp next_ts{0};
    std::uint64_t batch = 0;
    for (const Record& record : logged.applied) {
        while (loop.report().replay.events < record.market_event_seq) {
            if (loop.peek(next_ts) != PeekStatus::Event || !step(loop).ok) {
                return false;
            }
            generation.strategy.delivered.clear();
        }
        if (record.batch_seq != batch) {
            batch = record.batch_seq;
            loop.advance_to(record.exchange_ts);  // the batch's own advance: simulator events due by its clock
            generation.strategy.delivered.clear();
        }
        if (loop.gateway().now() != record.exchange_ts) {
            return false;
        }
        const Applied result = apply_command(generation, record.command);
        if (result.error != ErrorCode::None || result.order_id != record.order_id) {
            return false;
        }
    }
    // On to where the session left the generation (its trailer's boundary), or to the end of the stream.
    const std::uint64_t stop = logged.end ? logged.end->market_event_seq : std::numeric_limits<std::uint64_t>::max();
    while (loop.report().replay.events < stop && loop.peek(next_ts) == PeekStatus::Event && step(loop).ok) {
        generation.strategy.delivered.clear();
    }
    if (loop.peek(next_ts) != PeekStatus::Event) {
        loop.finish();
    } else if (logged.end) {
        loop.advance_to(logged.end->exchange_ts);
    }
    generation.strategy.delivered.clear();
    return !logged.end ||
        (loop.report().replay.events == stop && loop.gateway().now() == logged.end->exchange_ts &&
         digests(generation) == logged.end->digests);
}

}  // namespace simulator::server
