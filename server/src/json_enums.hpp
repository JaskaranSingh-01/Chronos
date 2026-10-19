#pragma once

#include <nlohmann/json.hpp>

#include "chronos/server/itch_input.hpp"
#include "chronos/server/replay_session.hpp"

// The JSON names of the enums the command log and the WebSocket protocol share. Reading an unknown name gives the
// first enumerator, so a reader that must refuse one compares the name it read back.

namespace simulator {

NLOHMANN_JSON_SERIALIZE_ENUM(Side, {{Side::Buy, "buy"}, {Side::Sell, "sell"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ReportType,
                             {{ReportType::Accepted, "accepted"},
                              {ReportType::Rejected, "rejected"},
                              {ReportType::Fill, "fill"},
                              {ReportType::Cancelled, "cancelled"},
                              {ReportType::CancelRejected, "cancel_rejected"},
                              {ReportType::Expired, "expired"}})
NLOHMANN_JSON_SERIALIZE_ENUM(RejectReason,
                             {{RejectReason::None, "none"},
                              {RejectReason::WouldCross, "would_cross"},
                              {RejectReason::InvalidOrder, "invalid_order"},
                              {RejectReason::NotTrading, "not_trading"}})
NLOHMANN_JSON_SERIALIZE_ENUM(Liquidity,
                             {{Liquidity::None, "none"}, {Liquidity::Maker, "maker"}, {Liquidity::Taker, "taker"}})
NLOHMANN_JSON_SERIALIZE_ENUM(FillCause,
                             {{FillCause::None, "none"},
                              {FillCause::RuleAExecute, "a_e"},
                              {FillCause::RuleAExecuteWithPrice, "a_c"},
                              {FillCause::RuleDExecute, "d_e"},
                              {FillCause::RuleDExecuteWithPrice, "d_c"},
                              {FillCause::RuleCAdd, "c_add"},
                              {FillCause::RuleCReplace, "c_replace"}})
NLOHMANN_JSON_SERIALIZE_ENUM(TradingState,
                             {{TradingState::Trading, "trading"},
                              {TradingState::Halted, "halted"},
                              {TradingState::Paused, "paused"},
                              {TradingState::QuotationOnly, "quotation_only"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ReplayFailure,
                             {{ReplayFailure::None, "none"},
                              {ReplayFailure::SourceError, "source_error"},
                              {ReplayFailure::OutOfOrderTimestamp, "out_of_order_timestamp"},
                              {ReplayFailure::InvariantViolation, "invariant_violation"}})

}  // namespace simulator

namespace simulator::server {

NLOHMANN_JSON_SERIALIZE_ENUM(CommandType,
                             {{CommandType::Order, "order"},
                              {CommandType::Cancel, "cancel"},
                              {CommandType::Attach, "attach"},
                              {CommandType::Detach, "detach"}})
NLOHMANN_JSON_SERIALIZE_ENUM(
    OrderType, {{OrderType::PostOnly, "post_only"}, {OrderType::Ioc, "ioc"}, {OrderType::Market, "market"}})
NLOHMANN_JSON_SERIALIZE_ENUM(SessionStrategyKind,
                             {{SessionStrategyKind::None, "none"},
                              {SessionStrategyKind::MarketMaker, "market_maker"},
                              {SessionStrategyKind::ImbalanceTaker, "imbalance"}})
NLOHMANN_JSON_SERIALIZE_ENUM(Lifecycle,
                             {{Lifecycle::Received, "received"},
                              {Lifecycle::Admitted, "admitted"},
                              {Lifecycle::RejectedAtAdmission, "rejected_at_admission"},
                              {Lifecycle::Applied, "applied"},
                              {Lifecycle::RejectedAtApply, "rejected_at_apply"},
                              {Lifecycle::SimulatorAccepted, "simulator_accepted"},
                              {Lifecycle::SimulatorRejected, "simulator_rejected"}})
NLOHMANN_JSON_SERIALIZE_ENUM(SessionState,
                             {{SessionState::Paused, "paused"},
                              {SessionState::Playing, "playing"},
                              {SessionState::Stepping, "stepping"},
                              {SessionState::Seeking, "seeking"},
                              {SessionState::Ended, "ended"}})
NLOHMANN_JSON_SERIALIZE_ENUM(ControlType,
                             {{ControlType::Play, "play"},
                              {ControlType::Pause, "pause"},
                              {ControlType::Step, "step"},
                              {ControlType::Speed, "speed"},
                              {ControlType::Seek, "seek"},
                              {ControlType::Restart, "restart"},
                              {ControlType::Symbol, "symbol"},
                              {ControlType::CancelSeek, "cancel_seek"}})
NLOHMANN_JSON_SERIALIZE_ENUM(SessionSource, {{SessionSource::Feed, "feed"}, {SessionSource::Fallback, "fallback"}})
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FeedSession, open, close, source)

}  // namespace simulator::server
