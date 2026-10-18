#include "chronos/server/session_runner.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace simulator::server {

SessionRunner::SessionRunner(ReplaySession& session)
    : session_(session), thread_([this] { run(); })
{}

SessionRunner::~SessionRunner()
{
    {
        const std::lock_guard lock{session_.inbox_mutex()};
        stop_ = true;
    }
    session_.inbox_cv().notify_all();
    thread_.join();
}

void SessionRunner::run() noexcept
{
    using clock = std::chrono::steady_clock;
    for (;;) {
        const Timestamp now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
        // Two statements: snapshot_due() must see tick()'s effects, and std::min's argument order is unspecified.
        const Timestamp next = session_.tick(now);
        const Timestamp wake = std::min(next, session_.snapshot_due());
        std::unique_lock lock{session_.inbox_mutex()};
        const auto woken = [this] { return stop_ || session_.has_input(); };
        if (wake == ReplaySession::never) {
            session_.inbox_cv().wait(lock, woken);
        } else if (wake > now) {
            const clock::time_point due{std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds{wake})};
            session_.inbox_cv().wait_until(lock, due, woken);
        }
        if (stop_) {
            return;
        }
    }
}

}  // namespace simulator::server
