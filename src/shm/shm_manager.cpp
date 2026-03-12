#include "shm/shm_manager.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>

#include "common/error.hpp"
#include "common/log.hpp"
#include "shm/basecore_shm_bridge.hpp"
#include "shm/orders_shm.hpp"

namespace acct_service {

namespace {

bool report_shm_error(ErrorCode code, std::string_view name, std::string_view detail, int err = 0) {
    ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::shm, code, "shm_manager", detail, err);
    if (!name.empty()) {
        std::string text = std::string(detail) + " [" + std::string(name) + "]";
        status.message.assign(text);
    }
    record_error(status);
    ACCT_LOG_ERROR_STATUS(status);
    return false;
}

ErrorCode classify_open_error(int err) noexcept {
    return err == EOVERFLOW ? ErrorCode::ShmResizeFailed : ErrorCode::ShmOpenFailed;
}

bool report_unsupported_backend(std::string_view name) {
    return report_shm_error(
        ErrorCode::ShmOpenFailed, name, "SHM_USE_FILE=1 is unsupported for account-service data shm");
}

bool report_open_failure(std::string_view name, std::string_view detail) {
    const int err = errno;
    return report_shm_error(classify_open_error(err), name, detail, err);
}

bool report_create_failure(std::string_view name, std::string_view detail) {
    const int err = errno;
    return report_shm_error(ErrorCode::ShmOpenFailed, name, detail, err);
}

void cleanup_new_region(std::string_view name) noexcept {
    (void)shm::ShmGenericWriter::unlink(name);
}

bool open_existing_region(
    shm::ShmGenericWriter& writer, std::string_view name, std::size_t size, std::string_view detail) {
    errno = 0;
    if (!basecore_shm_bridge::open_writer(writer, name, size)) {
        return report_open_failure(name, detail);
    }
    return true;
}

}  // namespace

// 构造函数
SHMManager::SHMManager() = default;

// 析构函数
SHMManager::~SHMManager() noexcept = default;

// 移动构造函数
SHMManager::SHMManager(SHMManager&& other) noexcept = default;

// 移动赋值运算符
SHMManager& SHMManager::operator=(SHMManager&& other) noexcept = default;

// 内部实现：打开或创建共享内存
void* SHMManager::open_impl(std::string_view name, std::size_t size, shm_mode mode) {
    // 如果已经打开了其他共享内存，先关闭
    if (is_open()) {
        close();
    }

    if (basecore_shm_bridge::is_file_backend_requested()) {
        (void)report_unsupported_backend(name);
        return nullptr;
    }

    last_open_is_new_ = false;
    bool is_new = false;

    switch (mode) {
        case shm_mode::Create: {
            errno = 0;
            if (!basecore_shm_bridge::create_region(name, size)) {
                (void)report_create_failure(name, "create shm failed");
                return nullptr;
            }
            if (!open_existing_region(writer_, name, size, "open created shm failed")) {
                cleanup_new_region(name);
                return nullptr;
            }
            is_new = true;
            break;
        }
        case shm_mode::Open:
            if (!open_existing_region(writer_, name, size, "open existing shm failed")) {
                return nullptr;
            }
            is_new = false;
            break;
        case shm_mode::OpenOrCreate: {
            errno = 0;
            if (basecore_shm_bridge::create_region(name, size)) {
                if (!open_existing_region(writer_, name, size, "open newly created shm failed")) {
                    cleanup_new_region(name);
                    return nullptr;
                }
                is_new = true;
                break;
            }

            const int create_err = errno;
            errno = 0;
            if (!basecore_shm_bridge::open_writer(writer_, name, size)) {
                if (create_err != EEXIST && create_err != 0) {
                    (void)report_shm_error(
                        ErrorCode::ShmOpenFailed, name, "create shm for open_or_create failed", create_err);
                } else {
                    (void)report_open_failure(name, "open existing shm after create_only failed");
                }
                return nullptr;
            }
            is_new = false;
            break;
        }
    }

    last_open_is_new_ = is_new;
    return writer_.data();
}

// 初始化共享内存头部
void SHMManager::init_header(SHMHeader *header, AccountId account_id) {
    (void)account_id;
    header->magic = SHMHeader::kMagic;
    header->version = SHMHeader::kVersion;
    header->create_time = now_ns();
    header->last_update = now_ns();
    header->next_order_id.store(1, std::memory_order_relaxed);
}

// 验证共享内存头部
bool SHMManager::validate_header(const SHMHeader *header) {
    if (header->magic != SHMHeader::kMagic) {
        (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name(), "invalid shm magic");
        return false;
    }
    if (header->version != SHMHeader::kVersion) {
        (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name(), "invalid shm version");
        return false;
    }
    return true;
}

// 创建/打开上游共享内存
upstream_shm_layout *SHMManager::open_upstream(std::string_view name, shm_mode mode, AccountId account_id) {
    constexpr std::size_t size = sizeof(upstream_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<upstream_shm_layout *>(ptr);

    if (last_open_is_new_) {
        init_header(&layout->header, account_id);
    } else {
        if (!validate_header(&layout->header)) {
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开下游共享内存
downstream_shm_layout *SHMManager::open_downstream(std::string_view name, shm_mode mode, AccountId account_id) {
    constexpr std::size_t size = sizeof(downstream_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<downstream_shm_layout *>(ptr);

    if (last_open_is_new_) {
        init_header(&layout->header, account_id);
    } else {
        if (!validate_header(&layout->header)) {
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开成交回报共享内存
trades_shm_layout *SHMManager::open_trades(std::string_view name, shm_mode mode, AccountId account_id) {
    constexpr std::size_t size = sizeof(trades_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<trades_shm_layout *>(ptr);

    if (last_open_is_new_) {
        init_header(&layout->header, account_id);
    } else {
        if (!validate_header(&layout->header)) {
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开订单池共享内存
orders_shm_layout* SHMManager::open_orders(std::string_view name, shm_mode mode, AccountId account_id) {
    (void)account_id;
    constexpr std::size_t size = sizeof(orders_shm_layout);
    void* ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto* layout = static_cast<orders_shm_layout*>(ptr);

    char expected_trading_day[9] = "00000000";
    if (!extract_trading_day_from_name(name, expected_trading_day)) {
        std::memcpy(expected_trading_day, "00000000", 9);
    }

    if (last_open_is_new_) {
        layout->header.magic = OrdersHeader::kMagic;
        layout->header.version = OrdersHeader::kVersion;
        layout->header.header_size = static_cast<uint32_t>(sizeof(OrdersHeader));
        layout->header.total_size = static_cast<uint32_t>(sizeof(orders_shm_layout));
        layout->header.capacity = static_cast<uint32_t>(kDailyOrderPoolCapacity);
        layout->header.init_state = 0;
        layout->header.create_time = now_ns();
        layout->header.last_update = layout->header.create_time;
        layout->header.next_index.store(0, std::memory_order_relaxed);
        layout->header.full_reject_count.store(0, std::memory_order_relaxed);
        std::memcpy(layout->header.trading_day, expected_trading_day, 9);
        layout->header.init_state = 1;
    } else {
        if (layout->header.magic != OrdersHeader::kMagic) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid orders shm magic");
            close();
            return nullptr;
        }
        if (layout->header.version != OrdersHeader::kVersion) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid orders shm version");
            close();
            return nullptr;
        }
        if (layout->header.header_size != static_cast<uint32_t>(sizeof(OrdersHeader))) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid orders shm header size");
            close();
            return nullptr;
        }
        if (layout->header.total_size != static_cast<uint32_t>(sizeof(orders_shm_layout))) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid orders shm total size");
            close();
            return nullptr;
        }
        if (layout->header.capacity != static_cast<uint32_t>(kDailyOrderPoolCapacity)) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid orders shm capacity");
            close();
            return nullptr;
        }
        if (layout->header.init_state != 1) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "orders shm not initialized");
            close();
            return nullptr;
        }
        if (std::memcmp(layout->header.trading_day, expected_trading_day, 8) != 0) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "orders shm trading day mismatch");
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开持仓共享内存
positions_shm_layout *SHMManager::open_positions(std::string_view name, shm_mode mode, AccountId account_id) {
    (void)account_id;
    constexpr std::size_t size = sizeof(positions_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<positions_shm_layout *>(ptr);

    // 持仓共享内存使用 positions_header，需要单独处理
    if (last_open_is_new_) {
        layout->header.magic = PositionsHeader::kMagic;
        layout->header.version = PositionsHeader::kVersion;
        layout->header.header_size = static_cast<uint32_t>(sizeof(PositionsHeader));
        layout->header.total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
        layout->header.capacity = static_cast<uint32_t>(kMaxPositions);
        layout->header.init_state = 0;
        layout->header.create_time = now_ns();
        layout->header.last_update = now_ns();
        layout->header.id.store(1, std::memory_order_relaxed);
        layout->position_count.store(0, std::memory_order_relaxed);
    } else {
        if (layout->header.magic != PositionsHeader::kMagic) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid positions shm magic");
            close();
            return nullptr;
        }
        if (layout->header.version != PositionsHeader::kVersion) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid positions shm version");
            close();
            return nullptr;
        }
        const uint32_t expected_header_size = static_cast<uint32_t>(sizeof(PositionsHeader));
        if (layout->header.header_size != expected_header_size) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid positions header size");
            close();
            return nullptr;
        }
        const uint32_t expected_total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
        if (layout->header.total_size != expected_total_size) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid positions total size");
            close();
            return nullptr;
        }
        const uint32_t expected_capacity = static_cast<uint32_t>(kMaxPositions);
        if (layout->header.capacity != expected_capacity) {
            (void)report_shm_error(ErrorCode::ShmHeaderInvalid, name, "invalid positions capacity");
            close();
            return nullptr;
        }
    }

    return layout;
}

// 关闭并解除映射
void SHMManager::close() noexcept {
    writer_.close();
    last_open_is_new_ = false;
}

// 删除共享内存对象
bool SHMManager::unlink(std::string_view name) {
    if (basecore_shm_bridge::is_file_backend_requested()) {
        (void)report_unsupported_backend(name);
        return false;
    }

    if (::shm_unlink(std::string(name).c_str()) == 0) {
        return true;
    }

    (void)report_shm_error(ErrorCode::ShmOpenFailed, name, "shm_unlink failed", errno);
    return false;
}

// 检查是否已打开
bool SHMManager::is_open() const noexcept { return writer_.is_open(); }

// 获取共享内存名称
const std::string& SHMManager::name() const noexcept { return writer_.name(); }

}  // namespace acct_service
