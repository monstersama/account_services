#include "api/order_monitor_api.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>

#include "common/constants.hpp"
#include "shm/shm_layout.hpp"

namespace {

static_assert(ACCT_MON_SECURITY_ID_LEN == acct_service::kSecurityIdSize, "security id size mismatch");
static_assert(ACCT_MON_INTERNAL_SECURITY_ID_LEN == acct_service::kInternalSecurityIdSize,
              "internal security id size mismatch");
static_assert(ACCT_MON_BROKER_ORDER_ID_LEN == acct_service::kBrokerOrderIdSize, "broker order id size mismatch");

bool is_valid_trading_day(std::string_view trading_day) noexcept {
    if (trading_day.size() != 8) {
        return false;
    }
    for (const char ch : trading_day) {
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

std::string make_orders_shm_name(std::string_view base_name, std::string_view trading_day) {
    std::string name(base_name);
    name += "_";
    name.append(trading_day.data(), trading_day.size());
    return name;
}

std::string default_trading_day() {
    const char* env_day = std::getenv("ACCT_TRADING_DAY");
    if (env_day && is_valid_trading_day(env_day)) {
        return std::string(env_day);
    }
    return "19700101";
}

bool resolve_options(const acct_orders_mon_options_t* options, std::string& base_name, std::string& trading_day) {
    base_name = acct_service::kOrdersShmName;
    trading_day = default_trading_day();

    if (!options) {
        return true;
    }

    if (options->orders_shm_name && options->orders_shm_name[0] != '\0') {
        base_name = options->orders_shm_name;
    }
    if (options->trading_day && options->trading_day[0] != '\0') {
        trading_day = options->trading_day;
    }

    if (base_name.empty()) {
        return false;
    }
    return is_valid_trading_day(trading_day);
}

bool is_index_visible(const acct_service::orders_shm_layout* shm, uint32_t index) noexcept {
    if (!shm) {
        return false;
    }
    const uint32_t upper = shm->header.next_index.load(std::memory_order_acquire);
    return index < upper && index < shm->header.capacity;
}

void fill_snapshot(acct_orders_mon_snapshot_t& out, uint32_t index, uint64_t seq, uint64_t last_update_ns,
                   acct_service::order_slot_stage_t stage, acct_service::order_slot_source_t source,
                   const acct_service::order_request& request) {
    out = acct_orders_mon_snapshot_t{};
    out.index = index;
    out.seq = seq;
    out.last_update_ns = last_update_ns;

    out.stage = static_cast<uint8_t>(stage);
    out.source = static_cast<uint8_t>(source);
    out.order_type = static_cast<uint8_t>(request.order_type);
    out.trade_side = static_cast<uint8_t>(request.trade_side);
    out.market = static_cast<uint8_t>(request.market);
    out.order_status = static_cast<uint8_t>(request.order_status.load(std::memory_order_acquire));
    std::memcpy(out.internal_security_id, request.internal_security_id.data, sizeof(out.internal_security_id));

    out.internal_order_id = request.internal_order_id;
    out.orig_internal_order_id = request.orig_internal_order_id;

    out.md_time_driven = request.md_time_driven;
    out.md_time_entrust = request.md_time_entrust;
    out.md_time_cancel_sent = request.md_time_cancel_sent;
    out.md_time_cancel_done = request.md_time_cancel_done;
    out.md_time_broker_response = request.md_time_broker_response;
    out.md_time_market_response = request.md_time_market_response;
    out.md_time_traded_first = request.md_time_traded_first;
    out.md_time_traded_latest = request.md_time_traded_latest;

    out.volume_entrust = request.volume_entrust;
    out.volume_traded = request.volume_traded;
    out.volume_remain = request.volume_remain;
    out.dprice_entrust = request.dprice_entrust;
    out.dprice_traded = request.dprice_traded;
    out.dvalue_traded = request.dvalue_traded;
    out.dfee_estimate = request.dfee_estimate;
    out.dfee_executed = request.dfee_executed;
    out.broker_order_id_u64 = request.broker_order_id.as_uint;

    std::memcpy(out.security_id, request.security_id.data, sizeof(out.security_id));
    std::memcpy(out.broker_order_id, request.broker_order_id.as_str.data, sizeof(out.broker_order_id));
}

bool try_read_stable_snapshot(const acct_service::orders_shm_layout* shm, uint32_t index,
                              acct_orders_mon_snapshot_t& out_snapshot) {
    if (!is_index_visible(shm, index)) {
        return false;
    }

    const acct_service::order_slot& slot = shm->slots[index];
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint64_t seq0 = slot.seq.load(std::memory_order_acquire);
        if ((seq0 & 1ULL) != 0U) {
            continue;
        }

        const uint64_t last_update_ns = slot.last_update_ns;
        const acct_service::order_slot_stage_t stage = slot.stage;
        const acct_service::order_slot_source_t source = slot.source;
        const acct_service::order_request request = slot.request;

        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t seq1 = slot.seq.load(std::memory_order_acquire);
        if (seq0 == seq1 && (seq1 & 1ULL) == 0U) {
            fill_snapshot(out_snapshot, index, seq1, last_update_ns, stage, source, request);
            return true;
        }
    }

    return false;
}

bool validate_header(const acct_service::orders_shm_layout* shm, std::string_view expected_trading_day) noexcept {
    if (!shm) {
        return false;
    }

    const acct_service::orders_header& header = shm->header;
    if (header.magic != acct_service::orders_header::kMagic) {
        return false;
    }
    if (header.version != acct_service::orders_header::kVersion) {
        return false;
    }
    if (header.header_size != static_cast<uint32_t>(sizeof(acct_service::orders_header))) {
        return false;
    }
    if (header.total_size != static_cast<uint32_t>(sizeof(acct_service::orders_shm_layout))) {
        return false;
    }
    if (header.capacity != static_cast<uint32_t>(acct_service::kDailyOrderPoolCapacity)) {
        return false;
    }
    if (header.init_state != 1U) {
        return false;
    }
    return std::memcmp(header.trading_day, expected_trading_day.data(), ACCT_MON_TRADING_DAY_LEN) == 0;
}

}  // namespace

struct acct_orders_monitor_context {
    acct_orders_monitor_context() = default;

    int fd = -1;
    const acct_service::orders_shm_layout* orders_shm = nullptr;

    std::string orders_base_name;
    std::string trading_day;
    std::string orders_dated_name;

    bool initialized = false;

    ~acct_orders_monitor_context() noexcept {
        if (orders_shm) {
            (void)munmap(const_cast<acct_service::orders_shm_layout*>(orders_shm),
                         sizeof(acct_service::orders_shm_layout));
        }
        if (fd >= 0) {
            (void)::close(fd);
        }
    }

    acct_orders_monitor_context(const acct_orders_monitor_context&) = delete;
    acct_orders_monitor_context& operator=(const acct_orders_monitor_context&) = delete;
};

extern "C" {

ACCT_MON_API acct_mon_error_t acct_orders_mon_open(const acct_orders_mon_options_t* options,
                                                   acct_orders_mon_ctx_t* out_ctx) {
    if (!out_ctx) {
        return ACCT_MON_ERR_INVALID_PARAM;
    }
    *out_ctx = nullptr;

    std::string base_name;
    std::string trading_day;
    if (!resolve_options(options, base_name, trading_day)) {
        return ACCT_MON_ERR_INVALID_PARAM;
    }

    auto ctx = std::unique_ptr<acct_orders_monitor_context>(new (std::nothrow) acct_orders_monitor_context());
    if (!ctx) {
        return ACCT_MON_ERR_INTERNAL;
    }

    ctx->orders_dated_name = make_orders_shm_name(base_name, trading_day);
    ctx->fd = shm_open(ctx->orders_dated_name.c_str(), O_RDONLY | O_CLOEXEC, 0644);
    if (ctx->fd < 0) {
        return ACCT_MON_ERR_SHM_FAILED;
    }

    struct stat st{};
    if (fstat(ctx->fd, &st) < 0) {
        return ACCT_MON_ERR_SHM_FAILED;
    }

    if (static_cast<std::size_t>(st.st_size) != sizeof(acct_service::orders_shm_layout)) {
        return ACCT_MON_ERR_SHM_FAILED;
    }

    void* ptr = mmap(nullptr, sizeof(acct_service::orders_shm_layout), PROT_READ, MAP_SHARED, ctx->fd, 0);
    if (ptr == MAP_FAILED) {
        return ACCT_MON_ERR_SHM_FAILED;
    }

    ctx->orders_shm = static_cast<const acct_service::orders_shm_layout*>(ptr);
    if (!validate_header(ctx->orders_shm, trading_day)) {
        return ACCT_MON_ERR_SHM_FAILED;
    }

    ctx->orders_base_name = base_name;
    ctx->trading_day = trading_day;
    ctx->initialized = true;
    *out_ctx = ctx.release();
    return ACCT_MON_OK;
}

ACCT_MON_API acct_mon_error_t acct_orders_mon_close(acct_orders_mon_ctx_t ctx) {
    if (!ctx) {
        return ACCT_MON_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    delete context;
    return ACCT_MON_OK;
}

ACCT_MON_API acct_mon_error_t acct_orders_mon_info(acct_orders_mon_ctx_t ctx, acct_orders_mon_info_t* out_info) {
    if (!ctx || !out_info) {
        return ACCT_MON_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized || !context->orders_shm) {
        return ACCT_MON_ERR_NOT_INITIALIZED;
    }

    const acct_service::orders_header& header = context->orders_shm->header;
    out_info->magic = header.magic;
    out_info->version = header.version;
    out_info->capacity = header.capacity;
    out_info->next_index = header.next_index.load(std::memory_order_acquire);
    out_info->full_reject_count = header.full_reject_count.load(std::memory_order_acquire);
    out_info->create_time_ns = header.create_time;
    out_info->last_update_ns = header.last_update;
    std::memset(out_info->trading_day, 0, sizeof(out_info->trading_day));
    std::memcpy(out_info->trading_day, header.trading_day, ACCT_MON_TRADING_DAY_LEN);
    return ACCT_MON_OK;
}

ACCT_MON_API acct_mon_error_t acct_orders_mon_read(acct_orders_mon_ctx_t ctx, uint32_t index,
                                                   acct_orders_mon_snapshot_t* out_snapshot) {
    if (!ctx || !out_snapshot) {
        return ACCT_MON_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized || !context->orders_shm) {
        return ACCT_MON_ERR_NOT_INITIALIZED;
    }

    if (!is_index_visible(context->orders_shm, index)) {
        return ACCT_MON_ERR_NOT_FOUND;
    }

    if (!try_read_stable_snapshot(context->orders_shm, index, *out_snapshot)) {
        return ACCT_MON_ERR_RETRY;
    }

    return ACCT_MON_OK;
}

ACCT_MON_API const char* acct_orders_mon_strerror(acct_mon_error_t err) {
    switch (err) {
        case ACCT_MON_OK:
            return "Success";
        case ACCT_MON_ERR_NOT_INITIALIZED:
            return "Context not initialized";
        case ACCT_MON_ERR_INVALID_PARAM:
            return "Invalid parameter";
        case ACCT_MON_ERR_SHM_FAILED:
            return "Shared memory operation failed";
        case ACCT_MON_ERR_NOT_FOUND:
            return "Order index not found";
        case ACCT_MON_ERR_RETRY:
            return "Snapshot not stable, retry";
        case ACCT_MON_ERR_INTERNAL:
            return "Internal error";
        default:
            return "Unknown error";
    }
}

}  // extern "C"
