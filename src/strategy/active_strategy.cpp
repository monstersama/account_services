#include "strategy/active_strategy.hpp"

#include <cmath>
#include <memory>
#include <string_view>

namespace acct_service {

namespace {

// 仅消费 fresh prediction 的内建主动策略：信号方向与订单方向一致时直接发当前片额。
class PredictionSignalStrategy final : public ActiveStrategy {
public:
    explicit PredictionSignalStrategy(double signal_threshold) : signal_threshold_(signal_threshold) {}

    // 返回配置使用的稳定策略名。
    const char* name() const noexcept override { return "prediction_signal"; }

    // 当 fresh prediction 满足方向阈值时，直接给出一笔子单，否则保持 no-op。
    ActiveDecision evaluate(const ActiveStrategyContext& context) override {
        ActiveDecision decision{};
        if (!context.market_data.prediction.has_fresh_prediction() || context.budget_volume == 0) {
            return decision;
        }

        const float signal = context.market_data.prediction.signal;
        const bool buy_hit =
            context.parent_request.trade_side == TradeSide::Buy && signal >= static_cast<float>(signal_threshold_);
        const bool sell_hit =
            context.parent_request.trade_side == TradeSide::Sell && signal <= static_cast<float>(-signal_threshold_);
        if (!buy_hit && !sell_hit) {
            return decision;
        }

        decision.should_submit = true;
        decision.volume = context.budget_volume;
        decision.price = context.parent_request.dprice_entrust;
        return decision;
    }

private:
    double signal_threshold_ = 0.0;
};

}  // namespace

// 根据服务级配置创建主动策略；unknown 名称显式返回空，避免静默选错实现。
std::unique_ptr<ActiveStrategy> StrategyRegistry::create(const ActiveStrategyConfig& config) {
    if (!config.enabled) {
        return nullptr;
    }

    const std::string_view name = config.name;
    if (name.empty() || name == "none") {
        return nullptr;
    }
    if (name == "prediction_signal") {
        return std::make_unique<PredictionSignalStrategy>(std::fabs(config.signal_threshold));
    }
    return nullptr;
}

}  // namespace acct_service
