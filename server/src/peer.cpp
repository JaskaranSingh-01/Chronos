#include "chronos/server/peer.hpp"

#include <algorithm>

#include "chronos/server/protocol.hpp"
#include "chronos/server/security.hpp"

namespace simulator::server {

Peer::Peer(Hub& hub, Timestamp now)
    : hub_(hub), opened_(now)
{
    if (hub_.token.empty()) {
        authenticate();
    }
}

Peer::~Peer()
{
    if (hub_.controller == this) {
        hub_.controller = nullptr;  // the next connection to authenticate takes it
    }
}

// The first connection to authenticate controls the session; later ones view it.
void Peer::authenticate() noexcept
{
    authenticated_ = true;
    if (hub_.controller == nullptr) {
        hub_.controller = this;
    }
}

bool Peer::receive(std::string_view text, Timestamp now, std::vector<std::string>& out)
{
    if (!authenticated_ && now - opened_ >= auth_timeout) {
        return false;  // poll() closes at the same point: an auth that late is no auth
    }
    ClientMessage message;
    std::string error;
    const bool valid = decode(text, message, error);
    if (!authenticated_) {  // nothing but the right auth is processed before it
        if (!valid || message.type != ClientType::Auth || !token_equal(message.token, hub_.token)) {
            return false;
        }
        authenticate();
        return true;
    }
    ReplaySession& session = hub_.session;
    const std::uint64_t generation = session.generation();
    const auto refuse = [&](std::string_view code, std::string_view why, std::uint64_t command_seq = 0) {
        out.push_back(encode_error(hub_.session_id, generation, code, why, command_seq, message.client_id));
        return true;
    };
    if (!valid) {
        return refuse("bad_request", error);
    }
    if (message.type == ClientType::Auth) {
        return refuse("bad_request", "already authenticated");
    }
    if (message.session_id != hub_.session_id) {
        return refuse("stale_generation", "a message for another session");
    }
    if (message.type == ClientType::Resync) {
        const std::uint64_t next = session.reports_since(0, {}).last_seq + 1;
        next_report_ = std::clamp<std::uint64_t>(message.from_report_seq, 1, next);
        return true;
    }
    if (hub_.controller != this) {
        return refuse("not_controller", "this connection is a viewer");
    }
    if (message.type == ClientType::Control) {
        Control& control = message.control;
        if (message.generation != generation) {
            return refuse("stale_generation", "a control for an earlier generation");
        }
        std::string why;
        if (control.type == ControlType::Watch &&
            (hub_.lab == nullptr || hub_.library == nullptr || !hub_.lab->watch(control, *hub_.library, why))) {
            return refuse("bad_request", why.empty() ? "no Lab" : why);
        }
        if (control.type == ControlType::Symbol) {
            if (control.spec.days.count == 0) {
                control.spec.days = hub_.days;  // written out, so the engine never resolves "the current days"
            }
            if (control.spec.days.count == 0) {
                return refuse("bad_request", "no day to load");
            }
            std::string missing;  // the days whose stock directory lacks the symbol
            for (std::size_t i = 0; i < control.spec.days.count; ++i) {
                const std::chrono::year_month_day date = control.spec.days.day[i];
                const DayHandle day = hub_.library != nullptr ? hub_.library->find(date) : nullptr;
                if (day == nullptr || !day->itch50) {
                    return refuse("bad_request",
                                  iso_date(date) + (day == nullptr ? ": no such day" : ": not ITCH 5.0"));
                }
                if (!std::binary_search(day->symbols.begin(), day->symbols.end(), name(control.spec.ticker))) {
                    missing += (missing.empty() ? "" : ", ") + iso_date(date);
                }
            }
            if (!missing.empty()) {
                return refuse("symbol_missing",
                              std::string{name(control.spec.ticker)} + " is not listed on " + missing);
            }
        }
        const Admission admission = session.control(control);
        if (admission.error != ErrorCode::None) {
            return refuse(to_string(admission.error), "control refused");
        }
        if (control.type == ControlType::Symbol || control.type == ControlType::Watch) {
            hub_.days = control.spec.days;
        }
        return true;
    }
    if (now - window_start_ >= second) {
        window_start_ = now;
        window_submits_ = 0;
    }
    if (++window_submits_ > submits_per_second) {
        return refuse("rate_limited", "more than " + std::to_string(submits_per_second) + " commands a second");
    }
    const Admission admission = session.submit(message.command, now);
    if (admission.error != ErrorCode::None) {
        return refuse(to_string(admission.error), "command refused", admission.seq);
    }
    if (hub_.writer != nullptr) {
        acks_.push_back({.command_seq = admission.seq,
                         .record_seq = admission.record_seq,
                         .generation = generation,
                         .client_id = message.client_id});
    }
    return true;
}

bool Peer::poll(Timestamp now, std::vector<std::string>& out)
{
    if (!authenticated_) {
        return now - opened_ < auth_timeout;
    }
    ReplaySession& session = hub_.session;
    if (!hello_sent_) {
        out.push_back(encode_hello(hub_.session_id, session.generation(), hub_.controller == this, hub_.config));
        hello_sent_ = true;
    }

    // Reports from the cursor, once the first state is out; one that left the ring gets a gap, then a full state.
    if (next_report_ != 0 && sent_snapshot_ != 0) {
        const RecordRing<Record>::Read read = session.reports_since(next_report_, hub_.reports);
        if (next_report_ < read.first_seq) {
            out.push_back(encode_gap(hub_.session_id, session.generation(), next_report_, read.first_seq - 1));
            state_due_ = true;
            full_resync_ = true;
            // ponytail: a snapshot that trails the ring repeats gap + state each frame; send the chunk and advance
            next_report_ = read.first_seq;
        } else if (read.count > 0) {
            for (std::size_t i = 0; i < read.count; ++i) {
                out.push_back(encode_record(hub_.session_id, hub_.reports[i]));
            }
            next_report_ = hub_.reports[read.count - 1].seq + 1;
        }
    }

    // Only the latest snapshot: the ones published since the last poll are dropped.
    bool copied = false;
    session.read_snapshot([&](const Snapshot& latest) {
        if (state_due_ || latest.snapshot_seq != sent_snapshot_) {
            hub_.snapshot = latest;
            copied = true;
        }
    });
    if (copied) {
        const Snapshot& snapshot = hub_.snapshot;
        const bool state = state_due_ || snapshot.generation != sent_generation_;
        out.push_back(encode_snapshot(hub_.session_id, snapshot, state, full_resync_));
        if (state_due_ && (next_report_ == 0 || full_resync_)) {
            next_report_ = std::max(next_report_, snapshot.last_report_seq + 1);  // the state holds what came before
        }
        state_due_ = false;
        full_resync_ = false;
        sent_snapshot_ = snapshot.snapshot_seq;
        sent_generation_ = snapshot.generation;
    }

    // A command is acked once its Admitted record is in the log.
    if (hub_.writer != nullptr) {
        // failed() first: a command persisted before the log failed is acked, never answered log_unavailable
        const bool failed = hub_.log != nullptr && hub_.log->failed();
        const std::uint64_t persisted = hub_.writer->persisted_seq();
        std::size_t kept = 0;
        for (const PendingAck& ack : acks_) {
            if (persisted >= ack.record_seq) {
                out.push_back(encode_command_ack(hub_.session_id, ack.generation, ack.command_seq, hub_.fsync));
            } else if (failed) {
                out.push_back(encode_error(hub_.session_id, ack.generation, to_string(ErrorCode::LogUnavailable),
                                           "the command could not be logged", ack.command_seq, ack.client_id));
            } else {
                acks_[kept++] = ack;
            }
        }
        acks_.resize(kept);
    }

    if (session.state() == SessionState::Seeking) {
        out.push_back(encode_progress(hub_.session_id, session.generation(), session.seek_position()));
    }
    return true;
}

}  // namespace simulator::server
