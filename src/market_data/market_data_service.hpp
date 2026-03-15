#pragma once

#include <vector>
#include <string>
#include <string_view>

#include "core/config_manager.hpp"
#include "snapshot_reader.hpp"
#include "snapshot_shm.hpp"

namespace acct_service {

// 项目内统一的预测值状态，避免上层直接依赖第三方枚举。
enum class PredictionState : uint8_t {
    None = 0,
    Fresh = 1,
    Carried = 2,
};

// 对 snapshot_reader 预测字段的稳定视图封装。
struct PredictionView {
    float signal = 0.0F;
    uint32_t flags = 0;
    PredictionState state = PredictionState::None;
    uint64_t publish_seq_no = 0;
    uint64_t publish_mono_ns = 0;

    bool has_prediction() const noexcept;
    bool has_fresh_prediction() const noexcept;
};

// 对单标的行情与预测值的统一读视图。
struct MarketDataView {
    std::string symbol;
    uint64_t seq = 0;
    snapshot_shm::LobSnapshot snapshot{};
    PredictionView prediction{};
};

// 统一封装 snapshot_reader 的打开、符号规范化和稳定快照读取。
class MarketDataService {
public:
    explicit MarketDataService(const MarketDataConfig& config);
    ~MarketDataService() = default;

    MarketDataService(const MarketDataService&) = delete;
    MarketDataService& operator=(const MarketDataService&) = delete;

    // 打开行情共享内存；未启用配置时返回 true 但保持 not-ready。
    bool initialize();

    // 关闭共享内存映射，供服务停机时显式回收。
    void close() noexcept;

    // 是否显式启用了行情模块。
    bool is_enabled() const noexcept;

    // 是否已经成功打开并校验 reader。
    bool is_ready() const noexcept;

    // 返回当前 reader 暴露的全部 canonical symbol，供调试工具快速枚举可读合约。
    std::vector<std::string> list_symbols() const;

    // 按 canonical internal_security_id 读取一条稳定行情快照。
    bool read(std::string_view internal_security_id, MarketDataView& out_view) const;

private:
    // 将项目内部证券键规范化成 snapshot_reader 使用的 symbol。
    static bool normalize_symbol(std::string_view internal_security_id, std::string& out_symbol);

    // 把第三方预测状态投影成项目内部枚举，统一上层判断逻辑。
    static PredictionState map_prediction_state(snapshot_shm::SnapshotPredictionState state) noexcept;

    MarketDataConfig config_;
    mutable signal_engine::snapshot_reader::SnapshotReader reader_;
    bool ready_ = false;
};

}  // namespace acct_service
