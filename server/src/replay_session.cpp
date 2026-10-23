#include "chronos/server/replay_session.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

#include "chronos/server/command_log.hpp"
#include "chronos/server/itch_input.hpp"
#include "simulator/orders/order.hpp"

namespace simulator::server {
namespace {

// What admission can check without the book: a limit order has a positive price; an attach names a strategy,
// and its imbalance threshold is finite and at least 1 (logged for every kind; a non-finite one is not JSON).
bool valid(const Command& command) noexcept
{
    switch (command.type) {
    case CommandType::Order:
        return command.order_type == OrderType::Market || command.price.ticks > 0;
    case CommandType::Attach:
        return command.strategy.kind != SessionStrategyKind::None &&
            std::isfinite(command.strategy.imbalance.threshold) && command.strategy.imbalance.threshold >= 1.0;
    case CommandType::Cancel:
    case CommandType::Detach:
        return true;
    }
    return false;
}

bool awaits_verdict(const Command& command) noexcept
{
    return command.type == CommandType::Order || command.type == CommandType::Cancel;
}

// The simulator's verdict in an order's or a cancel's first report.
ErrorCode verdict(const ExecutionReport& report) noexcept
{
    if (report.type == ReportType::CancelRejected) {
        return ErrorCode::CancelRejected;
    }
    if (report.type != ReportType::Rejected) {
        return ErrorCode::None;
    }
    switch (report.reason) {
    case RejectReason::WouldCross:
        return ErrorCode::WouldCross;
    case RejectReason::NotTrading:
        return ErrorCode::NotTrading;
    case RejectReason::None:
    case RejectReason::InvalidOrder:
        break;
    }
    return ErrorCode::InvalidOrder;
}

}  // namespace

ReplaySession::ReplaySession(SessionConfig config, SourceFactory factory, LogQueue* log, SourceFactory watch)
    : config_(std::move(config)), factory_(std::move(factory)), watch_factory_(std::move(watch)), log_(log)
{
    batch_.reserve(command_capacity);
    controls_batch_.reserve(control_capacity);
    awaiting_.reserve(command_capacity);
    engine_.emplace(config_, config_.defaults, factory_(config_.defaults.ticker, config_.defaults.days));
    publish_start();
    load();
}

Admission ReplaySession::submit(Command command, Timestamp received_wall_ts) noexcept
{
    const std::lock_guard lock{inbox_mutex_};
    command.command_seq = next_command_seq_++;
    command.received_wall_ts = received_wall_ts;
    const std::uint64_t current = generation_.load();
    ErrorCode error = admission(command, current);
    if (error == ErrorCode::None && !commands_.push(command)) {
        error = ErrorCode::QueueFull;
    }
    publish(Record{.generation = current, .step = Lifecycle::Received, .command = command});
    const std::uint64_t record_seq =
        publish(Record{.generation = current,
                       .step = error == ErrorCode::None ? Lifecycle::Admitted : Lifecycle::RejectedAtAdmission,
                       .reason = error,
                       .command = command});
    if (error == ErrorCode::None) {
        inbox_cv_.notify_one();
    }
    return {.seq = command.command_seq, .error = error, .record_seq = record_seq};
}

Admission ReplaySession::control(Control control) noexcept
{
    const std::lock_guard lock{inbox_mutex_};
    control.control_seq = next_control_seq_++;
    const bool bad = (control.type == ControlType::Step && control.steps == 0) ||
        (control.type == ControlType::Speed && control.speed > max_speed) ||
        (control.type == ControlType::Symbol && name(control.spec.ticker).empty()) ||
        // A Watch needs its factory of fresh mappings; never kept ones (review gate 1).
        (control.type == ControlType::Watch && (control.watch.run_id[0] == '\0' || !watch_factory_)) ||
        control.spec.days.count > max_days;
    ErrorCode error = bad ? ErrorCode::BadRequest : ErrorCode::None;
    if (error == ErrorCode::None && !controls_.push(control)) {
        error = ErrorCode::QueueFull;
    }
    if (error == ErrorCode::None) {
        inbox_cv_.notify_one();
    }
    return {.seq = control.control_seq, .error = error};
}

RecordRing<Record>::Read ReplaySession::reports_since(std::uint64_t from_seq, std::span<Record> out) const noexcept
{
    return ring_.read(from_seq, out);
}

bool ReplaySession::read_history(std::uint64_t generation, Timestamp from_ts, std::vector<Candle>& candles,
                                 std::vector<Trade>& trades) const
{
    const std::lock_guard lock{history_mutex_};
    if (history_.generation != generation) {
        return false;
    }
    const auto first = std::lower_bound(history_.candles.begin(), history_.candles.end(), from_ts,
                                        [](const Candle& bar, Timestamp ts) { return bar.start < ts; });
    candles.assign(first, history_.candles.end());
    trades.clear();
    const std::uint64_t held = std::min<std::uint64_t>(history_.trades_total, snapshot_trades);
    for (std::uint64_t i = history_.trades_total - held; i < history_.trades_total; ++i) {
        if (const Trade& trade = history_.trades[i % snapshot_trades]; trade.timestamp >= from_ts) {
            trades.push_back(trade);
        }
    }
    return true;
}

SessionState ReplaySession::state() const noexcept
{
    return state_.load();
}

std::uint64_t ReplaySession::generation() const noexcept
{
    return generation_.load();
}

Timestamp ReplaySession::seek_position() const noexcept
{
    return seek_position_.load();
}

Timestamp ReplaySession::tick(Timestamp wall_now) noexcept
{
    drain_inbox();
    last_drained_ = batch_.size();
    const bool input = !batch_.empty() || !controls_batch_.empty();
    for (const Control& control : controls_batch_) {
        apply_control(control, wall_now);
    }
    controls_batch_.clear();

    const SessionState state = state_.load();
    bool more = false;
    switch (state) {
    case SessionState::Playing:
        more = play(wall_now);
        break;
    case SessionState::Stepping:
        more = step_some();
        break;
    case SessionState::Seeking:
        more = seek_some();
        break;
    case SessionState::Paused:
    case SessionState::Ended:
        break;
    }
    apply_batch();

    dirty_ = dirty_ || input || (state != SessionState::Paused && state != SessionState::Ended);
    if (dirty_ && state_.load() != SessionState::Seeking && wall_now >= next_snapshot_wall_) {
        publish_snapshot(wall_now);
    }
    if (more) {
        return log_backed_up() ? wall_now + snapshot_interval : wall_now;  // poll for the writer, never spin
    }
    return due_wall();
}

Timestamp ReplaySession::snapshot_due() const noexcept
{
    const SessionState state = state_.load();
    return (dirty_ || state == SessionState::Playing) && state != SessionState::Seeking ? next_snapshot_wall_ : never;
}

Timestamp ReplaySession::clock() noexcept
{
    return engine_->loop.gateway().now();
}

const MarketBook& ReplaySession::book() const noexcept
{
    return engine_->book;
}

const Portfolio& ReplaySession::portfolio() const noexcept
{
    return engine_->portfolio;
}

const ExecutionSimulator& ReplaySession::execution() const noexcept
{
    return engine_->execution;
}

const LoopReport& ReplaySession::loop_report() const noexcept
{
    return engine_->loop.report();
}

// True while the writer is behind: market work waits for it, and the engine itself never does.
// ponytail: replay slows to the writer's rate when reports outpace it; the high-water mark is half the queue, so one
// market event (in Play, one timestamp's events) must publish fewer than half the queue's records, which
// max_open_orders bounds; a faster writer or a larger queue if that rate is too slow.
bool ReplaySession::log_backed_up() const noexcept
{
    return log_ != nullptr && log_->backed_up();
}

// Admission's refusal of `command`, or None; the engine checks again when it applies the command.
ErrorCode ReplaySession::admission(const Command& command, std::uint64_t generation) const noexcept
{
    if (!valid(command)) {
        return ErrorCode::BadRequest;
    }
    if (log_ != nullptr && log_->failed()) {
        return ErrorCode::LogUnavailable;
    }
    if (command.generation != generation) {
        return ErrorCode::StaleGeneration;
    }
    const SessionState state = state_.load();
    if (state == SessionState::Ended || state == SessionState::Seeking) {
        return ErrorCode::NotReady;
    }
    return ErrorCode::None;
}

// The engine's refusal of a drained command, or None.
ErrorCode ReplaySession::refusal(const Command& command) const noexcept
{
    if (command.generation != generation_.load()) {
        return ErrorCode::StaleGeneration;
    }
    const SessionState state = state_.load();
    if (state == SessionState::Ended || state == SessionState::Seeking) {
        return ErrorCode::NotReady;
    }
    if (awaits_verdict(command) && awaiting_.size() == command_capacity) {
        return ErrorCode::QueueFull;
    }
    return ErrorCode::None;
}

// Puts `record` on the ring and, except an Ack, in the log; `reserved` uses the slot LogQueue::reserve() held.
// Returns the record's seq.
std::uint64_t ReplaySession::publish(Record record, bool reserved) noexcept
{
    const std::lock_guard lock{publish_mutex_};
    record.seq = ring_.publish(record);
    if (log_ == nullptr || record.kind == RecordKind::Ack || record.kind == RecordKind::Verdict) {
        return record.seq;
    }
    if (reserved) {
        log_->push_reserved(record);
    } else {
        log_->push(record);
    }
    return record.seq;
}

void ReplaySession::lifecycle(Lifecycle step, const Command& command, ErrorCode reason, OrderId order_id,
                              bool reserved) noexcept
{
    publish(Record{.generation = generation_.load(),
                   .step = step,
                   .reason = reason,
                   .command = command,
                   .batch_seq = batch_seq_,
                   .exchange_ts = clock(),
                   .market_event_seq = engine_->loop.report().replay.events,
                   .order_id = order_id},
            reserved);
}

// Publishes the reports the strategy was delivered, and the simulator's verdict on our orders and cancels.
void ReplaySession::publish_delivered() noexcept
{
    std::vector<ExecutionReport>& delivered = engine_->strategy.delivered;
    for (const ExecutionReport& report : delivered) {
        publish(Record{.generation = generation_.load(),
                       .kind = RecordKind::Report,
                       .order_id = report.order_id,
                       .report = report});
        const bool cancel = report.type == ReportType::Cancelled || report.type == ReportType::CancelRejected;
        const auto waiting = std::find_if(awaiting_.begin(), awaiting_.end(), [&](const Awaiting& a) {
            return a.order_id == report.order_id && a.cancel == cancel;
        });
        if (waiting == awaiting_.end()) {
            continue;
        }
        const ErrorCode reason = verdict(report);
        lifecycle(reason == ErrorCode::None ? Lifecycle::SimulatorAccepted : Lifecycle::SimulatorRejected,
                  waiting->command, reason, report.order_id);
        awaiting_.erase(waiting);
    }
    delivered.clear();
}

void ReplaySession::publish_start() noexcept
{
    publish(Record{.generation = generation_.load(),
                   .kind = RecordKind::GenerationStart,
                   .control = {.spec = engine_->spec, .watch = watch_}});
}

// The simulator events due by the clock run first, as a reproduction's advance_to() runs them.
void ReplaySession::close() noexcept
{
    if (state_.load() != SessionState::Ended) {
        engine_->loop.advance_to(clock());
        publish_delivered();
    }
    publish(Record{.generation = generation_.load(),
                   .kind = RecordKind::GenerationEnd,
                   .exchange_ts = clock(),
                   .market_event_seq = engine_->loop.report().replay.events,
                   .digests = digests(*engine_)});
}

// Puts the clock just before the first market event, or ends an empty or failed stream.
void ReplaySession::load() noexcept
{
    Timestamp first{0};
    if (engine_->loop.peek(first) == PeekStatus::Event) {
        engine_->loop.advance_to(first - 1);
    } else {
        end();
    }
}

// The current generation's spec with the strategy attached now: what a restart keeps.
RunSpec ReplaySession::carried() const noexcept
{
    RunSpec spec = engine_->spec;
    spec.strategy = engine_->strategy.spec();
    return spec;
}

// Builds the next generation of `spec` from a fresh source, and pauses before its first event.
void ReplaySession::restart(const RunSpec& spec) noexcept
{
    close();
    engine_.reset();
    const SourceFactory& factory = watch_.run_id[0] != '\0' && watch_factory_ ? watch_factory_ : factory_;
    engine_.emplace(config_, spec, factory(spec.ticker, spec.days));
    generation_.fetch_add(1);
    awaiting_.clear();
    candles_ = CandleBuilder{std::max<std::size_t>(spec.days.count, 1)};
    candles_published_ = 0;
    trades_total_ = 0;
    trades_published_ = 0;
    last_snapshot_events_ = 0;
    state_.store(SessionState::Paused);
    publish_start();
    load();
}

// Ends the generation; a Watch's also gets its verdict: its segments' hashes as read against the run's, a segment that
// did not read back intact being a changed input.
void ReplaySession::end() noexcept
{
    finish(*engine_);
    publish_delivered();
    state_.store(SessionState::Ended);
    if (watch_.run_id[0] == '\0') {
        return;
    }
    const auto* chain = dynamic_cast<const DaySequenceSource*>(engine_->source.get());
    static const std::vector<std::string> none;
    const std::vector<std::string>& hashes = chain != nullptr ? chain->hashes() : none;
    const bool unreadable = std::find(hashes.begin(), hashes.end(), "") != hashes.end();
    const bool rehashed = hashes.size() == engine_->spec.days.count;
    const Digests now = digests(*engine_);
    const Outcome outcome = verdict(
        unreadable || (rehashed && inputs_hash(hashes) != std::string_view{watch_.inputs.data(), watch_.inputs.size()}),
        watch_.same_code, engine_->loop.report().replay.succeeded() && now == watch_.expected);
    publish(Record{.generation = generation_.load(),
                   .kind = RecordKind::Verdict,
                   .control = {.watch = watch_},
                   .digests = now,
                   .outcome = outcome});
}

void ReplaySession::drain_inbox() noexcept
{
    const std::unique_lock lock{inbox_mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        return;  // the server holds it: the next tick drains
    }
    Command command;
    while (commands_.pop(command)) {
        batch_.push_back(command);
    }
    Control control;
    while (controls_.pop(control)) {
        controls_batch_.push_back(control);
    }
}

void ReplaySession::apply_control(const Control& control, Timestamp wall_now) noexcept
{
    const bool ended = state_.load() == SessionState::Ended;
    switch (control.type) {
    case ControlType::Play:
        if (!ended) {
            state_.store(SessionState::Playing);
            anchor_wall_ = wall_now;
            anchor_exchange_ = clock();
        }
        break;
    case ControlType::Pause:
        if (!ended) {
            state_.store(SessionState::Paused);
        }
        break;
    case ControlType::Step:
        if (!ended) {
            // A Step while stepping adds to the steps owed (saturating), so two in one tick advance two events.
            steps_left_ = state_.load() == SessionState::Stepping
                ? steps_left_ + std::min(control.steps, std::numeric_limits<std::uint64_t>::max() - steps_left_)
                : control.steps;
            state_.store(SessionState::Stepping);
        }
        break;
    case ControlType::Speed:
        speed_ = control.speed;
        anchor_wall_ = wall_now;
        anchor_exchange_ = clock();
        break;
    // ponytail: a Watch's Restart or backward Seek carries the current strategy, so an attach or detach mid-Watch
    // gives mismatch; restart a Watch from its run's spec if a client ever does that.
    case ControlType::Restart:
        restart(carried());
        break;
    case ControlType::Symbol: {
        RunSpec spec = watch_.run_id[0] != '\0' ? config_.defaults : carried();  // a watched run's settings are its own
        spec.ticker = control.spec.ticker;
        spec.days = control.spec.days;
        watch_ = {};
        restart(spec);
        break;
    }
    case ControlType::Watch:
        watch_ = control.watch;
        restart(control.spec);
        break;
    case ControlType::Seek:
        if (ended || control.seek_ts < clock()) {
            restart(carried());
        }
        seek_target_ = control.seek_ts;
        seek_position_.store(clock());
        if (state_.load() != SessionState::Ended) {  // an empty stream ends at once
            state_.store(SessionState::Seeking);
        }
        break;
    case ControlType::CancelSeek:
        if (state_.load() == SessionState::Seeking) {
            state_.store(SessionState::Paused);
        }
        break;
    }
    publish(
        Record{.generation = generation_.load(), .kind = RecordKind::Ack, .control = control, .state = state_.load()});
}

// Applies the market events paced time has reached, then the simulator events due by it; true if more are due now.
// A new day's Clear is due at once: the night passes in no wall time, and pacing restarts at the day's first event.
bool ReplaySession::play(Timestamp wall_now) noexcept
{
    Generation& g = *engine_;
    const auto paced = [&] { return speed_ == 0 ? never : anchor_exchange_ + (wall_now - anchor_wall_) * speed_; };
    const auto new_day = [&] { return g.loop.pending()->type == MarketEventType::Clear; };
    // The last event applied was a day's Clear: the rest of a night plays in no wall time, however the clock got there.
    const auto night = [&] {
        const ReplayReport& r = g.loop.report().replay;
        return r.events != 0 && r.last_timestamp == g.loop.session_midnight();
    };
    Timestamp target = paced();
    Timestamp next_ts{0};
    PeekStatus status = g.loop.peek(next_ts);
    for (std::size_t n = 0; status == PeekStatus::Event && (next_ts <= target || new_day() || night()); ++n) {
        if ((n >= events_per_tick || log_backed_up()) && next_ts != clock()) {
            break;  // the budget is spent or the log is backed up, and never inside one timestamp
        }
        if (night()) {
            anchor_exchange_ = next_ts;
            anchor_wall_ = wall_now;
            target = paced();
        }
        step_market();
        status = g.loop.peek(next_ts);
    }
    if (status != PeekStatus::Event) {
        end();
        return false;
    }
    g.loop.advance_to(std::min(target, next_ts - 1));
    publish_delivered();
    return next_ts <= target || new_day() || night();
}

// Applies up to events_per_tick of the market events a Step asked for; true if some are left.
// A Step may stop inside a timestamp; a batch there applies before the timestamp's remaining events, and only the
// batch's own events wait.
bool ReplaySession::step_some() noexcept
{
    Timestamp next_ts{0};
    for (std::size_t n = 0; steps_left_ > 0 && n < events_per_tick && !log_backed_up(); ++n, --steps_left_) {
        if (engine_->loop.peek(next_ts) != PeekStatus::Event) {
            break;
        }
        step_market();
    }
    if (engine_->loop.peek(next_ts) != PeekStatus::Event) {
        end();
        return false;
    }
    if (steps_left_ > 0) {
        return true;
    }
    state_.store(SessionState::Paused);
    return false;
}

// Applies up to events_per_tick market events at or before the target; there, sets the clock to it and pauses.
bool ReplaySession::seek_some() noexcept
{
    Generation& g = *engine_;
    Timestamp next_ts{0};
    PeekStatus status = g.loop.peek(next_ts);
    for (std::size_t n = 0;
         status == PeekStatus::Event && next_ts <= seek_target_ && n < events_per_tick && !log_backed_up(); ++n) {
        step_market();
        status = g.loop.peek(next_ts);
    }
    if (status != PeekStatus::Event) {
        seek_position_.store(clock());
        end();
        return false;
    }
    if (next_ts <= seek_target_) {
        seek_position_.store(clock());
        return true;
    }
    g.loop.advance_to(seek_target_);
    publish_delivered();
    seek_position_.store(clock());
    state_.store(SessionState::Paused);
    return false;
}

// Applies the pending market event, first recording the trade it prints for the tape and the candles.
void ReplaySession::step_market() noexcept
{
    Generation& g = *engine_;
    Trade trade;
    if (const MarketEvent* event = g.loop.pending(); event != nullptr && market_trade(*event, g.book, trade)) {
        trades_[trades_total_ % snapshot_trades] = trade;
        ++trades_total_;
        candles_.add(trade);
    }
    step(g.loop);
    publish_delivered();
}

// Applies the commands drained this tick as one batch at the clock, after the simulator events due by it.
void ReplaySession::apply_batch() noexcept
{
    if (batch_.empty()) {
        return;
    }
    ++batch_seq_;
    engine_->loop.advance_to(clock());  // a simulator event at the clock comes before a command at it
    publish_delivered();
    for (const Command& command : batch_) {
        last_command_seq_ = command.command_seq;
        ErrorCode error = refusal(command);
        bool reserved = false;  // a command is never applied without its record's place in the log
        if (error == ErrorCode::None && log_ != nullptr) {
            reserved = log_->reserve();
            error = reserved ? ErrorCode::None : ErrorCode::LogUnavailable;
        }
        Applied applied;
        if (error == ErrorCode::None) {
            applied = apply_command(*engine_, command);
            error = applied.error;
        }
        lifecycle(error == ErrorCode::None ? Lifecycle::Applied : Lifecycle::RejectedAtApply, command, error,
                  applied.order_id, reserved);
        if (error == ErrorCode::None && awaits_verdict(command)) {
            awaiting_.push_back(Awaiting{
                .order_id = applied.order_id, .cancel = command.type == CommandType::Cancel, .command = command});
        }
    }
    batch_.clear();
}

// While playing, the wall time at which pacing reaches the next market or simulator event; otherwise never.
Timestamp ReplaySession::due_wall() const noexcept
{
    if (state_.load() != SessionState::Playing) {
        return never;
    }
    const EventLoop<SessionStrategy>& loop = engine_->loop;
    Timestamp due = never;
    if (const MarketEvent* next = loop.pending(); next != nullptr) {
        due = next->timestamp;
    }
    Timestamp sim_ts{0};
    if (loop.next_sim_time(sim_ts)) {
        due = std::min(due, sim_ts);
    }
    if (due == never) {
        return never;
    }
    const Timestamp ahead = due - anchor_exchange_;
    if (speed_ == 0 || ahead <= 0) {
        return anchor_wall_;
    }
    return anchor_wall_ + (ahead + speed_ - 1) / speed_;
}

// Copies the generation's new closed bars and its latest trades into the history; false while a reader holds it.
bool ReplaySession::offer_history() noexcept
{
    const std::unique_lock lock{history_mutex_, std::try_to_lock};
    if (!lock.owns_lock()) {
        return false;
    }
    if (history_.generation != generation_.load()) {
        history_.generation = generation_.load();
        history_.candles.clear();
        history_.candles.reserve(CandleBuilder::day_seconds * std::max<std::size_t>(engine_->spec.days.count, 1));
    }
    const std::span<const Candle> closed = candles_.closed();
    history_.candles.insert(history_.candles.end(),
                            closed.begin() + static_cast<std::ptrdiff_t>(history_.candles.size()), closed.end());
    history_.trades = trades_;
    history_.trades_total = trades_total_;
    return true;
}

// Builds this boundary's snapshot in snapshot_ and offers it to the slot; a held slot drops it, counted.
void ReplaySession::publish_snapshot(Timestamp wall_now) noexcept
{
    Generation& g = *engine_;
    Snapshot& s = snapshot_;
    const ReplayReport& replay = g.loop.report().replay;
    s.generation = generation_.load();
    s.spec = g.spec;
    s.run_id = watch_.run_id;
    s.snapshot_seq = snapshot_seq_ + 1;
    s.exchange_ts = clock();
    s.event_seq = replay.events;
    s.last_report_seq = ring_.last_seq();
    s.last_command_seq = last_command_seq_;
    s.state = state_.load();
    s.speed = speed_;
    const auto* chain = dynamic_cast<const DaySequenceSource*>(g.source.get());
    s.progress = chain == nullptr || chain->size() == 0
        ? 0.0
        : static_cast<double>(chain->position()) / static_cast<double>(chain->size());
    s.failure = replay.failure;
    s.source_error = replay.source_error;
    s.trading_state = g.book.trading_state();
    s.bid_levels = g.book.depth(Side::Buy, s.bids);
    s.ask_levels = g.book.depth(Side::Sell, s.asks);

    const std::uint64_t fresh = trades_total_ - trades_published_;
    s.trade_count = static_cast<std::size_t>(std::min<std::uint64_t>(fresh, snapshot_trades));
    s.trades_truncated = fresh > snapshot_trades;
    for (std::size_t i = 0; i < s.trade_count; ++i) {
        s.trades[i] = trades_[(trades_total_ - s.trade_count + i) % snapshot_trades];
    }
    s.has_candle = candles_.current(s.candle);
    const std::span<const Candle> closed = candles_.closed().subspan(candles_published_);
    s.closed_count = std::min(closed.size(), snapshot_candles);
    s.candles_truncated = closed.size() > snapshot_candles;
    std::copy(closed.end() - static_cast<std::ptrdiff_t>(s.closed_count), closed.end(), s.closed_candles.begin());

    const Portfolio& p = g.portfolio;
    s.account = Account{.position = p.position(),
                        .cash = p.cash(),
                        .realised = p.realised_pnl(),
                        .unrealised = p.unrealised_pnl(),
                        .fees = p.fees(),
                        .pnl = p.pnl(),
                        .max_drawdown = p.max_drawdown()};
    const std::span<const ShadowOrder> resting = g.execution.resting_orders();
    s.my_order_count = std::min(resting.size(), snapshot_orders);
    s.my_orders_truncated = resting.size() > snapshot_orders;
    for (std::size_t i = 0; i < s.my_order_count; ++i) {
        const ShadowOrder& o = resting[i];
        std::uint32_t ahead = 0;
        g.book.visit_orders(o.side, o.price, [&](const Order& order) {
            if (order.sequence() > o.anchor_sequence) {
                return false;
            }
            ++ahead;
            return true;
        });
        s.my_orders[i] = MyOrder{.id = o.id,
                                 .side = o.side,
                                 .price = o.price,
                                 .remaining = o.remaining,
                                 .ahead_quantity = o.ahead_quantity,
                                 .ahead_orders = ahead};
    }
    const Timestamp elapsed = wall_now - last_snapshot_wall_;
    const std::uint64_t rate = snapshot_seq_ != 0 && elapsed > 0
        ? (s.event_seq - last_snapshot_events_) * 1'000'000'000 / static_cast<std::uint64_t>(elapsed)
        : 0;
    s.metrics = SnapshotMetrics{.events_per_second = rate,
                                .dropped_snapshots = dropped_snapshots_,
                                .command_queue_depth = last_drained_,
                                .log_failed = log_ != nullptr && log_->failed()};

    next_snapshot_wall_ = wall_now + snapshot_interval;
    // The history first: it never lags a snapshot a client has seen.
    if (!offer_history() || !snapshots_.try_publish(s)) {
        ++dropped_snapshots_;
        return;  // the trades and bars stay for the next snapshot
    }
    ++snapshot_seq_;
    trades_published_ = trades_total_;
    candles_published_ = candles_.closed().size();
    last_snapshot_events_ = s.event_seq;
    last_snapshot_wall_ = wall_now;
    dirty_ = false;
}

}  // namespace simulator::server
