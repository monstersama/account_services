#include "market_data/market_data_service.hpp"

#include <utility>

#include "common/log.hpp"
#include "common/security_identity.hpp"

namespace acct_service {

bool PredictionView::has_prediction() const noexcept { return state != PredictionState::None; }

bool PredictionView::has_fresh_prediction() const noexcept { return state == PredictionState::Fresh; }

MarketDataService::MarketDataService(const MarketDataConfig& config) : config_(config) {}

// 打开 reader 并保持 ready 状态，未启用时显式走 no-op 路径。
bool MarketDataService::initialize() {
    ready_ = false;
    reader_.close();

    if (!config_.enabled) {
        return true;
    }
    if (config_.snapshot_shm_name.empty()) {
        return false;
    }
    if (!reader_.open(config_.snapshot_shm_name)) {
        if (config_.allow_order_price_fallback) {
            ACCT_LOG_WARN("MarketDataService",
                          "market data reader open failed; managed execution will fall back to parent order price");
            return true;
        }
        return false;
    }

    ready_ = reader_.is_open() && reader_.header() != nullptr;
    if (!ready_ && config_.allow_order_price_fallback) {
        ACCT_LOG_WARN("MarketDataService",
                      "market data reader is not ready; managed execution will fall back to parent order price");
        return true;
    }
    return ready_;
}

// 停机时关闭映射，避免把第三方 reader 生命周期散落到调用方。
void MarketDataService::close() noexcept {
    reader_.close();
    ready_ = false;
}

bool MarketDataService::is_enabled() const noexcept { return config_.enabled; }

bool MarketDataService::is_ready() const noexcept { return ready_; }

bool MarketDataService::allow_order_price_fallback() const noexcept { return config_.allow_order_price_fallback; }

// 列出当前快照源中的全部 symbol，未 ready 时返回空集合。
std::vector<std::string> MarketDataService::list_symbols() const {
    if (!ready_) {
        return {};
    }
    return reader_.list_symbols();
}

// 统一把项目内部证券键转换为 snapshot_reader 的 symbol 形态。
bool MarketDataService::normalize_symbol(std::string_view internal_security_id, std::string& out_symbol) {
    InternalSecurityId normalized_security_id;
    if (!normalize_internal_security_id(internal_security_id, normalized_security_id)) {
        return false;
    }
    out_symbol.assign(normalized_security_id.view());
    return true;
}

// 统一 prediction_state 语义，避免上层直接依赖第三方协议枚举。
PredictionState MarketDataService::map_prediction_state(snapshot_shm::SnapshotPredictionState state) noexcept {
    switch (state) {
        case snapshot_shm::SnapshotPredictionState::kFresh:
            return PredictionState::Fresh;
        case snapshot_shm::SnapshotPredictionState::kCarried:
            return PredictionState::Carried;
        case snapshot_shm::SnapshotPredictionState::kNone:
        default:
            return PredictionState::None;
    }
}

// 读取稳定快照并把第三方 payload 投影成项目内部统一视图。
bool MarketDataService::read(std::string_view internal_security_id, MarketDataView& out_view) const {
    if (!ready_) {
        return false;
    }

    std::string symbol;
    if (!normalize_symbol(internal_security_id, symbol)) {
        return false;
    }

    signal_engine::snapshot_reader::ReadResult result;
    if (!reader_.read(symbol, result)) {
        return false;
    }

    out_view = MarketDataView{};
    out_view.symbol = std::move(result.symbol);
    out_view.seq = result.seq;
    out_view.snapshot = result.payload.snapshot;
    out_view.prediction.signal = result.payload.signal;
    out_view.prediction.flags = result.payload.flags;
    out_view.prediction.state = map_prediction_state(result.payload.prediction_state);
    out_view.prediction.publish_seq_no = result.payload.publish_seq_no;
    out_view.prediction.publish_mono_ns = result.payload.publish_mono_ns;
    return true;
}

}  // namespace acct_service
