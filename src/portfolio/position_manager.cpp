#include "portfolio/position_manager.hpp"

#include <string>

#include "common/constants.hpp"
#include "common/error.hpp"
#include "common/log.hpp"
#include "common/security_identity.hpp"
#include "common/types.hpp"

namespace acct_service {

namespace {

constexpr dvalue_t kDefaultInitialFund = 100000000;
constexpr std::size_t kMaxSecurityPositions = kMaxPositions - kFirstSecurityPositionIndex;

bool header_compatible(const positions_header& header) {
    if (header.magic != positions_header::kMagic || header.version != positions_header::kVersion) {
        return false;
    }
    if (header.header_size != static_cast<uint32_t>(sizeof(positions_header))) {
        return false;
    }
    if (header.total_size != static_cast<uint32_t>(sizeof(positions_shm_layout))) {
        return false;
    }
    if (header.capacity != static_cast<uint32_t>(kMaxPositions)) {
        return false;
    }
    return true;
}

void clear_position(position& pos) {
    pos.locked.store(0, std::memory_order_relaxed);
    pos.available = 0;
    pos.volume_available_t0 = 0;
    pos.volume_available_t1 = 0;
    pos.volume_buy = 0;
    pos.dvalue_buy = 0;
    pos.volume_buy_traded = 0;
    pos.dvalue_buy_traded = 0;
    pos.volume_sell = 0;
    pos.dvalue_sell = 0;
    pos.volume_sell_traded = 0;
    pos.dvalue_sell_traded = 0;
    pos.count_order = 0;
    pos.id.clear();
    pos.name.clear();
}

std::size_t clamp_security_count(std::size_t count) {
    if (count > kMaxSecurityPositions) {
        return kMaxSecurityPositions;
    }
    return count;
}

uint32_t next_security_id(std::size_t security_count) {
    return static_cast<uint32_t>(security_count + kFirstSecurityPositionIndex);
}

position* fund_position(positions_shm_layout* shm) {
    if (!shm) {
        return nullptr;
    }
    return &shm->positions[kFundPositionIndex];
}

const position* security_position_by_row(const positions_shm_layout* shm, std::size_t row_index) {
    if (!shm || row_index < kFirstSecurityPositionIndex) {
        return nullptr;
    }
    if (row_index >= kMaxPositions) {
        return nullptr;
    }

    const std::size_t count = clamp_security_count(shm->position_count.load(std::memory_order_acquire));
    if (row_index > count) {
        return nullptr;
    }
    return &shm->positions[row_index];
}

position* security_position_by_row(positions_shm_layout* shm, std::size_t row_index) {
    return const_cast<position*>(security_position_by_row(static_cast<const positions_shm_layout*>(shm), row_index));
}

void ensure_fund_identity(position& fund_pos) {
    fund_pos.id.assign(kFundPositionId);
    fund_pos.name.assign(kFundPositionId);
}

void set_default_fund(position& fund_pos) {
    fund_info defaults;
    defaults.total_asset = kDefaultInitialFund;
    defaults.available = kDefaultInitialFund;
    defaults.frozen = 0;
    defaults.market_value = 0;
    store_fund_info(fund_pos, defaults);
}

}  // namespace

position_manager::position_manager(positions_shm_layout* shm) : shm_(shm) {}

bool position_manager::initialize(account_id_t account_id) {
    (void)account_id;

    if (!shm_) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::portfolio, error_code::ComponentUnavailable, "position_manager", "positions shm is null", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    security_to_row_.clear();

    if (!header_compatible(shm_->header)) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::portfolio, error_code::ShmHeaderInvalid, "position_manager", "positions shm header incompatible", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (shm_->header.init_state != 1) {
        const std::size_t existing = shm_->position_count.load(std::memory_order_relaxed);
        if (existing != 0) {
            error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::ShmHeaderCorrupted,
                "position_manager", "positions init_state is 0 while count is non-zero", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        shm_->header.magic = positions_header::kMagic;
        shm_->header.version = positions_header::kVersion;
        shm_->header.header_size = static_cast<uint32_t>(sizeof(positions_header));
        shm_->header.total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
        shm_->header.capacity = static_cast<uint32_t>(kMaxPositions);
        shm_->header.init_state = 0;
        shm_->header.create_time = now_ns();
        shm_->header.last_update = shm_->header.create_time;

        shm_->position_count.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < kMaxPositions; ++i) {
            clear_position(shm_->positions[i]);
        }

        position* fund_pos = fund_position(shm_);
        if (!fund_pos) {
            error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::ShmHeaderCorrupted,
                "position_manager", "fund position row is unavailable", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }
        ensure_fund_identity(*fund_pos);
        set_default_fund(*fund_pos);

        shm_->header.id.store(next_security_id(0), std::memory_order_relaxed);
        shm_->header.init_state = 1;
        shm_->header.last_update = now_ns();
        return true;
    }

    position* fund_pos = fund_position(shm_);
    if (!fund_pos) {
        error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::ShmHeaderCorrupted,
            "position_manager", "fund position row is unavailable in initialized shm", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }
    ensure_fund_identity(*fund_pos);

    std::size_t count = shm_->position_count.load(std::memory_order_acquire);
    const std::size_t clamped_count = clamp_security_count(count);
    if (clamped_count != count) {
        count = clamped_count;
        shm_->position_count.store(count, std::memory_order_relaxed);
    }

    for (std::size_t row_index = kFirstSecurityPositionIndex; row_index <= count && row_index < kMaxPositions;
         ++row_index) {
        const position& pos = shm_->positions[row_index];
        if (pos.id.empty()) {
            continue;
        }
        internal_security_id_t security_id;
        security_id.assign(pos.id.view());
        security_to_row_[security_id] = row_index;
    }

    shm_->header.id.store(next_security_id(count), std::memory_order_relaxed);
    shm_->header.last_update = now_ns();
    return true;
}

dvalue_t position_manager::get_available_fund() const noexcept {
    if (!shm_) {
        return 0;
    }
    position& fund_pos = shm_->positions[kFundPositionIndex];
    position_lock guard(fund_pos);
    return static_cast<dvalue_t>(fund_available_field(fund_pos));
}

bool position_manager::freeze_fund(dvalue_t amount, internal_order_id_t order_id) {
    (void)order_id;
    if (!shm_) {
        return false;
    }

    position& fund_pos = shm_->positions[kFundPositionIndex];
    position_lock guard(fund_pos);

    const uint64_t available = fund_available_field(fund_pos);
    const uint64_t frozen = fund_frozen_field(fund_pos);
    if (available < amount) {
        return false;
    }
    const uint64_t new_frozen = frozen + amount;
    if (new_frozen < frozen) {
        return false;
    }

    fund_available_field(fund_pos) = available - amount;
    fund_frozen_field(fund_pos) = new_frozen;
    shm_->header.last_update = now_ns();
    return true;
}

bool position_manager::unfreeze_fund(dvalue_t amount, internal_order_id_t order_id) {
    (void)order_id;
    if (!shm_) {
        return false;
    }

    position& fund_pos = shm_->positions[kFundPositionIndex];
    position_lock guard(fund_pos);

    const uint64_t available = fund_available_field(fund_pos);
    const uint64_t frozen = fund_frozen_field(fund_pos);
    if (frozen < amount) {
        return false;
    }
    const uint64_t new_available = available + amount;
    if (new_available < available) {
        return false;
    }

    fund_frozen_field(fund_pos) = frozen - amount;
    fund_available_field(fund_pos) = new_available;
    shm_->header.last_update = now_ns();
    return true;
}

bool position_manager::deduct_fund(dvalue_t amount, dvalue_t fee, internal_order_id_t order_id) {
    (void)order_id;
    if (!shm_) {
        return false;
    }

    const dvalue_t total = amount + fee;
    if (total < amount) {
        return false;
    }

    position& fund_pos = shm_->positions[kFundPositionIndex];
    position_lock guard(fund_pos);

    const uint64_t frozen = fund_frozen_field(fund_pos);
    if (frozen < total) {
        return false;
    }

    const uint64_t market_value = fund_market_value_field(fund_pos);
    const uint64_t new_market_value = market_value + amount;
    if (new_market_value < market_value) {
        return false;
    }

    const uint64_t total_asset = fund_total_asset_field(fund_pos);
    const uint64_t new_total_asset = total_asset < fee ? 0 : total_asset - fee;

    fund_frozen_field(fund_pos) = frozen - total;
    fund_total_asset_field(fund_pos) = new_total_asset;
    fund_market_value_field(fund_pos) = new_market_value;
    shm_->header.last_update = now_ns();
    return true;
}

bool position_manager::add_fund(dvalue_t amount, internal_order_id_t order_id) {
    (void)order_id;
    if (!shm_) {
        return false;
    }

    position& fund_pos = shm_->positions[kFundPositionIndex];
    position_lock guard(fund_pos);

    const uint64_t available = fund_available_field(fund_pos);
    const uint64_t total_asset = fund_total_asset_field(fund_pos);
    const uint64_t new_available = available + amount;
    if (new_available < available) {
        return false;
    }
    const uint64_t new_total_asset = total_asset + amount;
    if (new_total_asset < total_asset) {
        return false;
    }

    fund_available_field(fund_pos) = new_available;
    fund_total_asset_field(fund_pos) = new_total_asset;
    shm_->header.last_update = now_ns();
    return true;
}

const position* position_manager::get_position(internal_security_id_t security_id) const {
    const auto it = security_to_row_.find(security_id);
    if (it == security_to_row_.end()) {
        return nullptr;
    }
    return security_position_by_row(shm_, it->second);
}

position* position_manager::get_position_mut(internal_security_id_t security_id) {
    const auto it = security_to_row_.find(security_id);
    if (it == security_to_row_.end()) {
        return nullptr;
    }
    return security_position_by_row(shm_, it->second);
}

volume_t position_manager::get_sellable_volume(internal_security_id_t security_id) const {
    const position* pos = get_position(security_id);
    if (!pos) {
        return 0;
    }

    auto& mutable_pos = const_cast<position&>(*pos);
    position_lock guard(mutable_pos);
    return mutable_pos.volume_available_t0 + mutable_pos.volume_available_t1;
}

bool position_manager::freeze_position(internal_security_id_t security_id, volume_t volume,
    internal_order_id_t order_id) {
    (void)order_id;
    position* pos = get_position_mut(security_id);
    if (!pos) {
        return false;
    }

    position_lock guard(*pos);
    const volume_t sellable = pos->volume_available_t0 + pos->volume_available_t1;
    if (sellable < volume) {
        return false;
    }

    volume_t remaining = volume;
    if (pos->volume_available_t1 >= remaining) {
        pos->volume_available_t1 -= remaining;
        remaining = 0;
    } else {
        remaining -= pos->volume_available_t1;
        pos->volume_available_t1 = 0;
        if (pos->volume_available_t0 < remaining) {
            return false;
        }
        pos->volume_available_t0 -= remaining;
    }

    pos->volume_sell += volume;
    pos->count_order += 1;
    shm_->header.last_update = now_ns();
    return true;
}

bool position_manager::unfreeze_position(internal_security_id_t security_id, volume_t volume,
    internal_order_id_t order_id) {
    (void)order_id;
    position* pos = get_position_mut(security_id);
    if (!pos) {
        return false;
    }

    position_lock guard(*pos);
    if (pos->volume_sell < volume) {
        return false;
    }
    pos->volume_sell -= volume;
    pos->volume_available_t0 += volume;
    shm_->header.last_update = now_ns();
    return true;
}

bool position_manager::deduct_position(internal_security_id_t security_id, volume_t volume, dvalue_t value,
    internal_order_id_t order_id) {
    (void)order_id;
    position* pos = get_position_mut(security_id);
    if (!pos) {
        return false;
    }

    position_lock guard(*pos);
    if (pos->volume_sell >= volume) {
        // 常规路径：已冻结卖出数量，成交后只需释放冻结计数。
        pos->volume_sell -= volume;
    } else {
        // 兼容路径：若前置未冻结，回退到可卖仓位扣减，避免成交回报阶段直接失败。
        volume_t remaining = volume;
        if (pos->volume_sell > 0) {
            remaining -= pos->volume_sell;
            pos->volume_sell = 0;
        }

        const volume_t sellable = pos->volume_available_t0 + pos->volume_available_t1;
        if (sellable < remaining) {
            return false;
        }

        if (pos->volume_available_t1 >= remaining) {
            pos->volume_available_t1 -= remaining;
            remaining = 0;
        } else {
            remaining -= pos->volume_available_t1;
            pos->volume_available_t1 = 0;
            if (pos->volume_available_t0 < remaining) {
                return false;
            }
            pos->volume_available_t0 -= remaining;
        }
    }

    pos->volume_sell_traded += volume;
    pos->dvalue_sell_traded += value;
    shm_->header.last_update = now_ns();
    return true;
}

bool position_manager::add_position(internal_security_id_t security_id, volume_t volume, dprice_t price,
    internal_order_id_t order_id) {
    (void)order_id;
    position* pos = get_position_mut(security_id);
    if (!pos) {
        return false;
    }

    position_lock guard(*pos);
    const dvalue_t value = (volume == 0 || price == 0) ? 0 : volume * price;
    pos->volume_buy += volume;
    pos->dvalue_buy += value;
    pos->volume_buy_traded += volume;
    pos->dvalue_buy_traded += value;
    pos->volume_available_t1 += volume;
    shm_->header.last_update = now_ns();
    return true;
}

std::vector<const position*> position_manager::get_all_positions() const {
    std::vector<const position*> result;
    if (!shm_) {
        return result;
    }

    const std::size_t count = clamp_security_count(shm_->position_count.load(std::memory_order_acquire));
    result.reserve(count);
    for (std::size_t row_index = kFirstSecurityPositionIndex; row_index <= count && row_index < kMaxPositions;
         ++row_index) {
        const position* pos = &shm_->positions[row_index];
        if (pos->id.empty()) {
            continue;
        }
        result.push_back(pos);
    }
    return result;
}

fund_info position_manager::get_fund_info() const {
    fund_info fund;
    if (!shm_) {
        return fund;
    }

    position& fund_pos = shm_->positions[kFundPositionIndex];
    position_lock guard(fund_pos);
    return load_fund_info(fund_pos);
}

std::size_t position_manager::position_count() const noexcept {
    if (!shm_) {
        return 0;
    }
    return clamp_security_count(shm_->position_count.load(std::memory_order_acquire));
}

std::optional<internal_security_id_t> position_manager::find_security_id(std::string_view code) const {
    if (code.empty()) {
        return std::nullopt;
    }
    internal_security_id_t security_id;
    security_id.assign(code);
    auto it = security_to_row_.find(security_id);
    if (it == security_to_row_.end()) {
        return std::nullopt;
    }
    return security_id;
}

internal_security_id_t position_manager::add_security(std::string_view code, std::string_view name, market_t market) {
    if (!shm_ || code.empty()) {
        return {};
    }

    internal_security_id_t security_id;
    if (!build_internal_security_id(market, code, security_id)) {
        return {};
    }

    auto it = security_to_row_.find(security_id);
    if (it != security_to_row_.end()) {
        return security_id;
    }

    std::size_t count = shm_->position_count.load(std::memory_order_acquire);
    const std::size_t clamped_count = clamp_security_count(count);
    if (clamped_count != count) {
        count = clamped_count;
        shm_->position_count.store(count, std::memory_order_relaxed);
    }
    if (count >= kMaxSecurityPositions) {
        return {};
    }

    const std::size_t row_index = count + kFirstSecurityPositionIndex;
    if (row_index >= kMaxPositions) {
        return {};
    }

    position& pos = shm_->positions[row_index];
    clear_position(pos);
    pos.id.assign(security_id.view());
    pos.name.assign(name);

    security_to_row_[security_id] = row_index;

    const std::size_t new_count = count + 1;
    shm_->position_count.store(new_count, std::memory_order_release);
    shm_->header.id.store(next_security_id(new_count), std::memory_order_relaxed);
    shm_->header.last_update = now_ns();
    return security_id;
}

}  // namespace acct_service
