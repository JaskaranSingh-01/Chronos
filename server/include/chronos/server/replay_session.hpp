#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "chronos/server/candles.hpp"
#include "chronos/server/channels.hpp"
#include "chronos/server/command.hpp"
#include "chronos/server/generation.hpp"

namespace simulator::server {

enum class SessionState : std::uint8_t {
    Paused,
    Playing,
    Stepping,
    Seeking,
    Ended
};

enum class ControlType : std::uint8_t {
    Play,
    Pause,
    Step,
    Speed,
    Seek,
    Restart,
    CancelSeek
};

// A replay control from the client.
struct Control
{
    std::uint64_t control_seq{0};  // set by ReplaySession::control
    ControlType type{ControlType::Pause};
    std::uint64_t steps{0};  // Step: market events to apply
    std::uint32_t speed{1};  // Speed: exchange ns per wall ns; 0 = as fast as possible
    Timestamp seek_ts{0};    // Seek: the exchange time to stop at
};

enum class RecordKind : std::uint8_t {
    Lifecycle,  // one step of a command
    Report,     // an execution report delivered to the session's strategy
    Ack,             // a control, applied
    GenerationStart  // a new generation and the strategy it starts with
};

// A command's steps; Applied means it reached the gateway, and an order's or cancel's verdict follows.
enum class Lifecycle : std::uint8_t {
    Received,
    Admitted,
    RejectedAtAdmission,
    Applied,
    RejectedAtApply,
    SimulatorAccepted,
    SimulatorRejected
};

// One entry of the report ring: a command's step, a delivered report, a control's ack or a generation's start.
// Engine-step fields are the boundary at which the record was published; a verdict follows the step that delivered it.
struct Record
{
    std::uint64_t seq{0};  // set by the ring; clients resume from it
    std::uint64_t generation{0};
    RecordKind kind{RecordKind::Lifecycle};
    Lifecycle step{Lifecycle::Received};  // Lifecycle
    ErrorCode reason{ErrorCode::None};    // the Rejected* steps
    Command command{};                    // Lifecycle; GenerationStart: command.strategy is the starting strategy
    std::uint64_t batch_seq{0};           // engine steps: the batch being applied
    Timestamp exchange_ts{0};             // engine steps: the clock; for Applied, the applied exchange time
    std::uint64_t market_event_seq{0};    // engine steps: market events applied so far
    OrderId order_id{0};                  // Applied orders and cancels, their verdicts, and Report
    ExecutionReport report{};             // Report
    Control control{};                    // Ack
    SessionState state{SessionState::Paused};  // Ack: the state after the control
};

// submit()'s and control()'s answer.
struct Admission
{
    std::uint64_t seq{0};  // the command_seq or control_seq given
    ErrorCode error{ErrorCode::None};
};

inline constexpr std::size_t snapshot_levels = 20;
inline constexpr std::size_t snapshot_trades = 512;
inline constexpr std::size_t snapshot_candles = 64;
inline constexpr std::size_t snapshot_orders = 64;
// Wall ns between snapshots: 30 a second.
inline constexpr Timestamp snapshot_interval = 1'000'000'000 / 30;

// One of our resting orders and its place in the queue.
struct MyOrder
{
    OrderId id{0};
    Side side{Side::Buy};
    Price price{};
    std::uint64_t remaining{0};
    std::uint64_t ahead_quantity{0};
    std::uint32_t ahead_orders{0};  // historical orders ahead of ours: the queue rank
};

struct Account
{
    std::int64_t position{0};
    std::int64_t cash{0};
    std::int64_t realised{0};
    std::int64_t unrealised{0};
    std::int64_t fees{0};
    std::int64_t pnl{0};
    std::int64_t max_drawdown{0};
};

struct SnapshotMetrics
{
    std::uint64_t events_per_second{0};  // market events per wall second since the last snapshot
    std::uint64_t dropped_snapshots{0};
    std::size_t command_queue_depth{0};  // commands the last tick drained
    bool log_failed{false};              // the UI shows the log status
};

// The session at one boundary, built by the engine between steps. Entries past each count are unspecified.
struct Snapshot
{
    std::uint64_t generation{0};
    std::uint64_t snapshot_seq{0};
    Timestamp exchange_ts{0};
    std::uint64_t event_seq{0};  // market events applied
    std::uint64_t last_report_seq{0};
    std::uint64_t last_command_seq{0};  // the last command the engine applied or refused
    SessionState state{SessionState::Paused};
    std::uint32_t speed{1};
    double progress{0};  // the source's position over SessionConfig::source_size; 0 if that is unknown
    ReplayFailure failure{ReplayFailure::None};
    SourceError source_error{SourceError::None};
    std::array<LevelInfo, snapshot_levels> bids{};  // best first
    std::array<LevelInfo, snapshot_levels> asks{};
    std::size_t bid_levels{0};
    std::size_t ask_levels{0};
    std::array<Trade, snapshot_trades> trades{};  // market trades since the last snapshot, oldest first
    std::size_t trade_count{0};
    bool trades_truncated{false};  // more than snapshot_trades: the oldest are left out
    Candle candle{};               // the current 1 s bar
    bool has_candle{false};
    std::array<Candle, snapshot_candles> closed_candles{};  // bars closed since the last snapshot, oldest first
    std::size_t closed_count{0};
    bool candles_truncated{false};
    Account account{};
    std::array<MyOrder, snapshot_orders> my_orders{};  // per side, in priority order
    std::size_t my_order_count{0};
    bool my_orders_truncated{false};
    SnapshotMetrics metrics{};
};

class LogQueue;

// One interactive replay: any thread admits input; the engine thread drives it through tick(wall_now), never a clock.
class ReplaySession
{
public:
    static constexpr std::size_t command_capacity = 1'024;
    static constexpr std::size_t control_capacity = 64;
    static constexpr std::size_t ring_capacity = 65'536;
    // A tick yields after this many market events, at the next new timestamp, so controls are seen at full speed.
    static constexpr std::size_t events_per_tick = 4'096;
    // The largest Speed a client may ask for (0 = as fast as possible); keeps the pacing product within int64.
    static constexpr std::uint32_t max_speed = 1'000;
    static constexpr Timestamp never = std::numeric_limits<Timestamp>::max();

private:
    struct Awaiting
    {
        OrderId order_id{0};
        bool cancel{false};
        Command command{};
    };

    SessionConfig config_;
    SourceFactory factory_;
    std::optional<Generation> engine_;  // the current generation

    // The admission point: any thread, under inbox_mutex_.
    std::mutex inbox_mutex_;
    std::condition_variable inbox_cv_;
    FixedQueue<Command> commands_{command_capacity};
    FixedQueue<Control> controls_{control_capacity};
    std::uint64_t next_command_seq_{1};
    std::uint64_t next_control_seq_{1};

    std::atomic<SessionState> state_{SessionState::Paused};
    std::atomic<std::uint64_t> generation_{1};
    std::atomic<Timestamp> seek_position_{0};
    std::mutex publish_mutex_;  // makes a record's ring seq and its place in the log one step
    RecordRing<Record> ring_{ring_capacity};
    LogQueue* log_{nullptr};  // nullptr: this session keeps no log
    LatestSlot<Snapshot> snapshots_;

    // The engine thread only.
    std::vector<Command> batch_;
    std::vector<Control> controls_batch_;
    std::vector<Awaiting> awaiting_;  // applied orders and cancels waiting for the simulator's verdict
    std::uint64_t batch_seq_{0};
    std::uint64_t last_command_seq_{0};
    std::uint64_t steps_left_{0};
    Timestamp seek_target_{0};
    std::uint32_t speed_{1};
    Timestamp anchor_wall_{0};  // pacing: the exchange clock was anchor_exchange_ at wall time anchor_wall_
    Timestamp anchor_exchange_{0};
    Snapshot snapshot_;  // built here, then copied into snapshots_
    CandleBuilder candles_;
    std::array<Trade, snapshot_trades> trades_{};  // market trades since the last snapshot, a ring
    std::uint64_t trades_seen_{0};
    std::size_t candles_published_{0};
    std::uint64_t snapshot_seq_{0};
    std::uint64_t dropped_snapshots_{0};
    std::uint64_t last_snapshot_events_{0};
    Timestamp last_snapshot_wall_{0};
    Timestamp next_snapshot_wall_{0};
    std::size_t last_drained_{0};
    bool dirty_{true};  // something changed since the last published snapshot

    ErrorCode admission(const Command& command, std::uint64_t generation) const noexcept;
    ErrorCode refusal(const Command& command) const noexcept;
    void publish(Record record, bool reserved = false) noexcept;
    void lifecycle(Lifecycle step, const Command& command, ErrorCode reason, OrderId order_id,
                   bool reserved = false) noexcept;
    void publish_delivered() noexcept;
    void publish_start() noexcept;
    void load() noexcept;
    void restart() noexcept;
    void end() noexcept;
    void drain_inbox() noexcept;
    void apply_control(const Control& control, Timestamp wall_now) noexcept;
    bool play(Timestamp wall_now) noexcept;
    bool step_some() noexcept;
    bool seek_some() noexcept;
    void step_market() noexcept;
    void apply_batch() noexcept;
    Timestamp due_wall() const noexcept;
    void publish_snapshot(Timestamp wall_now) noexcept;

public:
    ReplaySession(SessionConfig config, SourceFactory factory, LogQueue* log = nullptr);

    ReplaySession(const ReplaySession&) = delete;
    ReplaySession& operator=(const ReplaySession&) = delete;

    // Any thread. The single admission point: numbers the command and queues it, or refuses it.
    Admission submit(Command command, Timestamp received_wall_ts) noexcept;
    // Any thread. Queues a control; the engine applies it at its next tick and acks it on the ring.
    Admission control(Control control) noexcept;
    // Any thread. Records from max(from_seq, first_seq) on; a from_seq below first_seq is a gap.
    RecordRing<Record>::Read reports_since(std::uint64_t from_seq, std::span<Record> out) const noexcept;
    SessionState state() const noexcept;
    std::uint64_t generation() const noexcept;
    // Any thread. The exchange time a seek has reached; the server reports it as progress.
    Timestamp seek_position() const noexcept;

    // Any thread. Calls f(const Snapshot&) with the latest snapshot held; the engine drops snapshots meanwhile.
    template <class F>
    bool read_snapshot(F&& f) const
    {
        return snapshots_.read(std::forward<F>(f));
    }

    // Engine thread: controls, replay to `wall_now`, the batch; returns the next due wall time (never = wait for input).
    Timestamp tick(Timestamp wall_now) noexcept;
    // Engine thread. When the next snapshot is due; never while nothing is new.
    Timestamp snapshot_due() const noexcept;
    // Engine thread: the exchange clock and the current generation's state.
    Timestamp clock() noexcept;
    const MarketBook& book() const noexcept;
    const Portfolio& portfolio() const noexcept;
    const ExecutionSimulator& execution() const noexcept;
    const LoopReport& loop_report() const noexcept;
};

}  // namespace simulator::server
