#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "shm/shm_manager.hpp"
#include "common/error.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

// 临时覆盖进程 umask，作用域结束时恢复，便于验证权限逻辑不受 umask 干扰。
class ScopedUmaskOverride {
public:
    explicit ScopedUmaskOverride(mode_t value) : old_umask_(::umask(value)) {}
    ~ScopedUmaskOverride() { (void)::umask(old_umask_); }

private:
    mode_t old_umask_;
};

class ScopedEnvOverride {
public:
    ScopedEnvOverride(const char* key, const char* value) : key_(key) {
        const char* existing = std::getenv(key);
        if (existing != nullptr) {
            had_old_value_ = true;
            old_value_ = existing;
        }
        (void)::setenv(key, value, 1);
    }

    ~ScopedEnvOverride() {
        if (had_old_value_) {
            (void)::setenv(key_.c_str(), old_value_.c_str(), 1);
        } else {
            (void)::unsetenv(key_.c_str());
        }
    }

private:
    std::string key_;
    std::string old_value_;
    bool had_old_value_ = false;
};

std::string unique_shm_name(const char* tag) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return std::string("/") + tag + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(static_cast<unsigned long long>(ns)) + "_" +
           std::to_string(static_cast<unsigned long long>(seq));
}

void cleanup_shm(const std::string& name) {
    if (::shm_unlink(name.c_str()) < 0 && errno != ENOENT) {
        std::perror("shm_unlink");
    }
}

}  // namespace

TEST(create_and_open) {
    using namespace acct_service;

    const std::string name = unique_shm_name("shm_mgr_create_open");
    cleanup_shm(name);

    SHMManager creator;
    auto* layout_create = creator.open_upstream(name, shm_mode::Create, 1);
    assert(layout_create != nullptr);
    layout_create->header.next_order_id.store(123, std::memory_order_relaxed);

    SHMManager opener;
    auto* layout_open = opener.open_upstream(name, shm_mode::Open, 1);
    assert(layout_open != nullptr);
    const uint32_t val = layout_open->header.next_order_id.load(std::memory_order_relaxed);
    assert(val == 123);

    creator.close();
    opener.close();
    cleanup_shm(name);
}

TEST(open_or_create_no_reinit) {
    using namespace acct_service;

    const std::string name = unique_shm_name("shm_mgr_open_or_create");
    cleanup_shm(name);

    SHMManager first;
    auto* layout_first = first.open_upstream(name, shm_mode::OpenOrCreate, 1);
    assert(layout_first != nullptr);
    layout_first->header.next_order_id.store(77, std::memory_order_relaxed);

    SHMManager second;
    auto* layout_second = second.open_upstream(name, shm_mode::OpenOrCreate, 1);
    assert(layout_second != nullptr);
    const uint32_t val = layout_second->header.next_order_id.load(std::memory_order_relaxed);
    assert(val == 77);

    first.close();
    second.close();
    cleanup_shm(name);
}

TEST(open_orders_with_dated_name) {
    using namespace acct_service;

    const std::string name = unique_shm_name("shm_mgr_orders") + "_20260225";
    cleanup_shm(name);

    SHMManager creator;
    auto* orders = creator.open_orders(name, shm_mode::Create, 1);
    assert(orders != nullptr);
    assert(std::string(orders->header.trading_day) == "20260225");
    assert(orders->header.capacity == kDailyOrderPoolCapacity);

    SHMManager opener;
    auto* opened = opener.open_orders(name, shm_mode::Open, 1);
    assert(opened != nullptr);
    assert(std::string(opened->header.trading_day) == "20260225");

    creator.close();
    opener.close();
    cleanup_shm(name);
}

TEST(size_mismatch) {
    using namespace acct_service;

    const std::string name = unique_shm_name("shm_mgr_size_mismatch");
    cleanup_shm(name);

    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    assert(fd >= 0);

    const std::size_t small_size = sizeof(SHMHeader);
    const int trunc_rc = ::ftruncate(fd, static_cast<off_t>(small_size));
    assert(trunc_rc == 0);
    ::close(fd);

    SHMManager mgr;
    auto* layout = mgr.open_upstream(name, shm_mode::Open, 1);
    assert(layout == nullptr);
    assert(!acct_service::last_error().ok());
    assert(acct_service::last_error().code == acct_service::ErrorCode::ShmResizeFailed);
    assert(acct_service::classify(acct_service::last_error().domain, acct_service::last_error().code).severity ==
           acct_service::ErrorSeverity::Critical);

    mgr.close();
    cleanup_shm(name);
}

TEST(create_mode_is_0777_and_ignores_umask) {
    using namespace acct_service;

    const std::string name = unique_shm_name("shm_mgr_create_mode_env");
    cleanup_shm(name);

    ScopedUmaskOverride mask_override(0077);

    SHMManager creator;
    auto* layout = creator.open_upstream(name, shm_mode::Create, 1);
    assert(layout != nullptr);

    const int fd = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
    assert(fd >= 0);

    struct stat st;
    const int stat_rc = ::fstat(fd, &st);
    assert(stat_rc == 0);
    ::close(fd);

    assert((st.st_mode & 0777) == 0777);

    creator.close();
    cleanup_shm(name);
}

TEST(rejects_file_backend_env) {
    using namespace acct_service;

    ScopedEnvOverride env_override("SHM_USE_FILE", "1");

    const std::string name = unique_shm_name("shm_mgr_file_backend");
    cleanup_shm(name);

    SHMManager manager;
    auto* layout = manager.open_upstream(name, shm_mode::Create, 1);
    assert(layout == nullptr);
    assert(!acct_service::last_error().ok());
    assert(acct_service::last_error().code == acct_service::ErrorCode::ShmOpenFailed);

    cleanup_shm(name);
}

int main() {
    printf("=== Shm Manager Test Suite ===\n\n");

    RUN_TEST(create_and_open);
    RUN_TEST(open_or_create_no_reinit);
    RUN_TEST(open_orders_with_dated_name);
    RUN_TEST(size_mismatch);
    RUN_TEST(create_mode_is_0777_and_ignores_umask);
    RUN_TEST(rejects_file_backend_env);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
