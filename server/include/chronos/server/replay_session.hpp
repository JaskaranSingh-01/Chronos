#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "chronos/server/channels.hpp"
#include "chronos/server/command.hpp"
#include "chronos/server/generation.hpp"

namespace simulator::server {

enum class SessionState : std::uint8_t {
    Paused,
    Playing,
    Stepping,
    Ended
};

enum class ControlType : std::uint8_t {
    Play,
    Pause,
    Step,
    Speed
};

// A replay control from the client.
struct Control
{
    std::uint64_t control_seq{0};  // set by ReplaySession::control
    ControlType type{ControlType::Pause};
    std::uint64_t steps{0};  // Step: market events to apply
    std::uint32_t speed{1};  // Speed: exchange ns per wall ns; 0 = as fast as possible
};

enum class RecordKind : std::uint8_t {
    Lifecycle,  // one step of a command
    Report,     // an execution report delivered to the session's strategy
    Ack         // a control, applied
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

// One entry of the report ring: a command's step, a delivered report or a control's ack.
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
    Control control{};                    // Ack
    SessionState state{SessionState::Paused};  // Ack: the state after the control
};

// submit()'s and control()'s answer.
struct Admission
{
    std::uint64_t seq{0};  // the command_seq or control_seq given
    ErrorCode error{ErrorCode::None};
};

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
    RecordRing<Record> ring_{ring_capacity};

    // The engine thread only.
    std::vector<Command> batch_;
    std::vector<Control> controls_batch_;
    std::vector<Awaiting> awaiting_;  // applied orders and cancels waiting for the simulator's verdict
    std::uint64_t batch_seq_{0};
    std::uint64_t last_command_seq_{0};
    std::uint64_t steps_left_{0};
    std::uint32_t speed_{1};
    Timestamp anchor_wall_{0};  // pacing: the exchange clock was anchor_exchange_ at wall time anchor_wall_
    Timestamp anchor_exchange_{0};

    ErrorCode admission(const Command& command, std::uint64_t generation) const noexcept;
    ErrorCode refusal(const Command& command) const noexcept;
    void publish(const Record& record) noexcept;
    void lifecycle(Lifecycle step, const Command& command, ErrorCode reason, OrderId order_id) noexcept;
    void publish_delivered() noexcept;
    void load() noexcept;
    void end() noexcept;
    void drain_inbox() noexcept;
    void apply_control(const Control& control, Timestamp wall_now) noexcept;
    bool play(Timestamp wall_now) noexcept;
    bool step_some() noexcept;
    void step_market() noexcept;
    void apply_batch() noexcept;
    Timestamp due_wall() const noexcept;

public:
    ReplaySession(SessionConfig config, SourceFactory factory);

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

    // Engine thread: controls, replay to `wall_now`, the batch; returns the next due wall time (never = wait for input).
    Timestamp tick(Timestamp wall_now) noexcept;
    // Engine thread: the exchange clock and the current generation's state.
    Timestamp clock() noexcept;
    const MarketBook& book() const noexcept;
    const Portfolio& portfolio() const noexcept;
    const ExecutionSimulator& execution() const noexcept;
    const LoopReport& loop_report() const noexcept;
};

}  // namespace simulator::server
