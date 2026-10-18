#pragma once

#include <thread>

#include "chronos/server/replay_session.hpp"

namespace simulator::server {

// Runs a ReplaySession on its own thread, the session's only clock reader; sleeps on the inbox until work is due.
class SessionRunner
{
private:
    ReplaySession& session_;
    bool stop_{false};  // under session_.inbox_mutex()
    std::thread thread_;

    void run() noexcept;

public:
    explicit SessionRunner(ReplaySession& session);
    ~SessionRunner();

    SessionRunner(const SessionRunner&) = delete;
    SessionRunner& operator=(const SessionRunner&) = delete;
};

}  // namespace simulator::server
