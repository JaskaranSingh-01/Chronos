#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chronos/server/command_log.hpp"
#include "chronos/server/replay_session.hpp"

namespace simulator::server {

struct Hub;

// One WebSocket connection's side of the protocol, without the socket: text in, text out.
class Peer
{
public:
    static constexpr Timestamp second = 1'000'000'000;       // wall ns
    static constexpr Timestamp auth_timeout = 5 * second;    // for the auth message, when there is a token
    static constexpr std::uint32_t submits_per_second = 50;  // each submit costs two log records
    static constexpr std::size_t reports_per_poll = 4'096;   // a ring read copies under the engine's publish lock

private:
    struct PendingAck
    {
        std::uint64_t command_seq{0};
        std::uint64_t record_seq{0};  // of its Admitted record
        std::uint64_t generation{0};
        std::optional<std::uint64_t> client_id{};  // an order's, for its log_unavailable
    };

    Hub& hub_;
    Timestamp opened_;
    bool authenticated_{false};
    bool hello_sent_{false};
    bool state_due_{true};  // a full state goes out at the next poll with a snapshot
    bool full_resync_{false};
    std::uint64_t next_report_{0};  // the next ring seq to send; 0 until a state or a resync sets it
    std::uint64_t sent_snapshot_{0};
    std::uint64_t sent_generation_{0};
    std::vector<PendingAck> acks_;
    Timestamp window_start_{0};
    std::uint32_t window_submits_{0};

    void authenticate() noexcept;

public:
    Peer(Hub& hub, Timestamp now);
    ~Peer();

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    // Handles one client message, appending the replies to `out`; false: close the connection.
    bool receive(std::string_view text, Timestamp now, std::vector<std::string>& out);
    // Once a frame: hello, then any gap, the state or the latest snapshot, new reports, acks and seek progress.
    // False: close the connection, as no auth came in time.
    bool poll(Timestamp now, std::vector<std::string>& out);
};

// What every connection to one session shares. Call every Peer of one Hub under one lock: the scratch poll() copies
// into and encodes from within one call is shared too, so a connection costs a Peer, not 1.3 MB.
struct Hub
{
    ReplaySession& session;
    const LogQueue* log{nullptr};      // set with writer; without it a log failure is never seen
    const LogWriter* writer{nullptr};  // set: commands are acked once their record is in the log
    std::string session_id{};
    std::string token{};  // empty: no auth message is needed
    bool fsync{false};
    SessionConfig config{};  // what hello tells a client: its scales, fees and limits
    std::vector<std::string> symbols{};  // the tickers a symbol switch may name
    const Peer* controller{nullptr};     // the one connection that may control and trade
    std::vector<Record> reports = std::vector<Record>(Peer::reports_per_poll);  // poll()'s ring chunk
    Snapshot snapshot{};                                                         // poll()'s copy of the latest one
};

}  // namespace simulator::server
