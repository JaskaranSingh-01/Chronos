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
    Symbol,
    CancelSeek,
    Watch
};

// A Watch in Terminal of a Lab run: the run, and what each of its generations' verdict compares.
struct Watched
{
    RunId run_id{};                 // all NUL when the generation watches no run
    Digests expected{};             // the run's
    std::array<char, 32> inputs{};  // inputs_hash() of the run's days
    bool same_code{false};          // the run's source digest is this build's
};

// A replay control from the client.
struct Control
{
    std::uint64_t control_seq{0};  // set by ReplaySession::control
    ControlType type{ControlType::Pause};
    std::uint64_t steps{0};  // Step: market events to apply
    std::uint32_t speed{1};  // Speed: exchange ns per wall ns; 0 = as fast as possible
    Timestamp seek_ts{0};    // Seek: the exchange time to stop at
    RunSpec spec{};          // Symbol: the ticker and days the next generation replays; Watch: the run's spec
    Watched watch{};         // Watch; GenerationStart: the run the generation watches
};

enum class RecordKind : std::uint8_t {
    Lifecycle,  // one step of a command
    Report,     // an execution report delivered to the session's strategy
    Ack,              // a control, applied
    GenerationStart,  // a new generation and the spec it replays
    GenerationEnd,    // a generation's last boundary and digests: the log's trailer, at a restart and at close()
    Verdict           // a Watch's generation ended: its outcome and digests, for clients only
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
    Command command{};                    // Lifecycle
    std::uint64_t batch_seq{0};           // engine steps: the batch being applied
    Timestamp exchange_ts{0};             // engine steps: the clock; for Applied, the applied exchange time
    std::uint64_t market_event_seq{0};    // engine steps: market events applied so far
    OrderId order_id{0};                  // Applied orders and cancels, their verdicts, and Report
    ExecutionReport report{};             // Report
    Control control{};                    // Ack; GenerationStart: control.spec is the generation's
    SessionState state{SessionState::Paused};  // Ack: the state after the control
    Digests digests{};                         // GenerationEnd, Verdict
    Outcome outcome{Outcome::Mismatch};        // Verdict; control.watch holds the run and its digests
};

// submit()'s and control()'s answer.
struct Admission
{
    std::uint64_t seq{0};  // the command_seq or control_seq given
    ErrorCode error{ErrorCode::None};
    std::uint64_t record_seq{0};  // submit(): the ring seq of its Admitted or RejectedAtAdmission record
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
    RunSpec spec{};  // the generation's
    RunId run_id{};  // the run it watches
    std::uint64_t snapshot_seq{0};
    Timestamp exchange_ts{0};
    std::uint64_t event_seq{0};  // market events applied
    std::uint64_t last_report_seq{0};
    std::uint64_t last_command_seq{0};  // the last command the engine applied or refused
    SessionState state{SessionState::Paused};
    std::uint32_t speed{1};
    double progress{0};  // a day chain's position over its size; 0 for any other source
    ReplayFailure failure{ReplayFailure::None};
    SourceError source_error{SourceError::None};
    TradingState trading_state{TradingState::Trading};  // the book's: anything but Trading means no matching
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
class SessionRunner;

// One interactive replay: any thread admits input; the engine thread drives it through tick(wall_now), never a clock.
class ReplaySession
{
public:
    static constexpr std::size_t command_capacity = 1'024;
    static constexpr std::size_t control_capacity = 64;
    // ponytail: ~712 B records (~47 MB), as a Control carries a RunSpec and a Watch; key loads by generation then.
    static constexpr std::size_t ring_capacity = 65'536;
    // The most market events a tick applies, so controls are seen at full speed: Playing yields at the next new
    // timestamp, but Step and Seek stop at the budget, possibly mid-timestamp.
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
    SourceFactory watch_factory_;
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
    Watched watch_{};  // the run the current generation watches
    std::uint64_t batch_seq_{0};
    std::uint64_t last_command_seq_{0};
    std::uint64_t steps_left_{0};
    Timestamp seek_target_{0};
    std::uint32_t speed_{1};
    Timestamp anchor_wall_{0};  // pacing: the exchange clock was anchor_exchange_ at wall time anchor_wall_
    Timestamp anchor_exchange_{0};
    Snapshot snapshot_;  // built here, then copied into snapshots_
    CandleBuilder candles_;
    std::array<Trade, snapshot_trades> trades_{};  // the generation's latest market trades, a ring
    std::uint64_t trades_total_{0};  // the generation's market trades
    std::uint64_t trades_published_{0};  // trades_total_ at the last published snapshot
    std::size_t candles_published_{0};
    std::uint64_t snapshot_seq_{0};
    std::uint64_t dropped_snapshots_{0};
    std::uint64_t last_snapshot_events_{0};
    Timestamp last_snapshot_wall_{0};
    Timestamp next_snapshot_wall_{0};
    std::size_t last_drained_{0};
    bool dirty_{true};  // something changed since the last published snapshot

    // For /api/history: the engine copies its bars and trades in at each snapshot without waiting; any thread reads.
    struct History
    {
        std::uint64_t generation{0};
        std::vector<Candle> candles;  // closed 1 s bars, oldest first
        std::array<Trade, snapshot_trades> trades{};  // a ring: trade i at i % snapshot_trades
        std::uint64_t trades_total{0};
    };
    mutable std::mutex history_mutex_;
    History history_;

    ErrorCode admission(const Command& command, std::uint64_t generation) const noexcept;
    ErrorCode refusal(const Command& command) const noexcept;
    std::uint64_t publish(Record record, bool reserved = false) noexcept;
    void lifecycle(Lifecycle step, const Command& command, ErrorCode reason, OrderId order_id,
                   bool reserved = false) noexcept;
    void publish_delivered() noexcept;
    void publish_start() noexcept;
    void load() noexcept;
    RunSpec carried() const noexcept;
    void restart(const RunSpec& spec) noexcept;
    void end() noexcept;
    void drain_inbox() noexcept;
    void apply_control(const Control& control, Timestamp wall_now) noexcept;
    bool play(Timestamp wall_now) noexcept;
    bool step_some() noexcept;
    bool seek_some() noexcept;
    void step_market() noexcept;
    void apply_batch() noexcept;
    Timestamp due_wall() const noexcept;
    bool log_backed_up() const noexcept;
    bool offer_history() noexcept;
    void publish_snapshot(Timestamp wall_now) noexcept;

    friend class SessionRunner;

    // The runner waits on inbox_cv() under inbox_mutex() until has_input() or its wake time.
    std::mutex& inbox_mutex() noexcept
    {
        return inbox_mutex_;
    }

    std::condition_variable& inbox_cv() noexcept
    {
        return inbox_cv_;
    }

    // Call with inbox_mutex() held.
    bool has_input() const noexcept
    {
        return !commands_.empty() || !controls_.empty();
    }

public:
    // `watch` builds a Watch's generations, keeping each segment's hash as read; without it, a Watch is refused.
    ReplaySession(SessionConfig config, SourceFactory factory, LogQueue* log = nullptr, SourceFactory watch = {});

    ReplaySession(const ReplaySession&) = delete;
    ReplaySession& operator=(const ReplaySession&) = delete;

    // Any thread. The single admission point: numbers the command and queues it, or refuses it.
    Admission submit(Command command, Timestamp received_wall_ts) noexcept;
    // Any thread. Queues a control; the engine applies it at its next tick and acks it on the ring.
    Admission control(Control control) noexcept;
    // Any thread. Records from max(from_seq, first_seq) on; records before first_seq were overwritten, so a gap is
    // max(from_seq, 1) < first_seq.
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

    // Any thread. `generation`'s closed 1 s bars from `from_ts` on and its latest trades (at most snapshot_trades) at
    // or after it, as of the last snapshot published; false if the history holds another generation.
    bool read_history(std::uint64_t generation, Timestamp from_ts, std::vector<Candle>& candles,
                      std::vector<Trade>& trades) const;

    // Engine thread: controls, replay to `wall_now`, the batch; returns the next due wall time or never.
    Timestamp tick(Timestamp wall_now) noexcept;
    // Engine thread. When the next snapshot is due: every frame while Playing, else only once something is new.
    Timestamp snapshot_due() const noexcept;
    // Engine thread: brings the generation to its clock and publishes its trailer. restart() calls it, and the owner
    // once, after the runner has stopped.
    void close() noexcept;
    // Engine thread: the exchange clock and the current generation's state.
    Timestamp clock() noexcept;
    const MarketBook& book() const noexcept;
    const Portfolio& portfolio() const noexcept;
    const ExecutionSimulator& execution() const noexcept;
    const LoopReport& loop_report() const noexcept;
};

}  // namespace simulator::server
