#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>

#include "shm/shm_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

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

    shm_manager creator;
    auto* layout_create = creator.open_upstream(name, shm_mode::Create, 1);
    assert(layout_create != nullptr);
    layout_create->header.next_order_id.store(123, std::memory_order_relaxed);

    shm_manager opener;
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

    shm_manager first;
    auto* layout_first = first.open_upstream(name, shm_mode::OpenOrCreate, 1);
    assert(layout_first != nullptr);
    layout_first->header.next_order_id.store(77, std::memory_order_relaxed);

    shm_manager second;
    auto* layout_second = second.open_upstream(name, shm_mode::OpenOrCreate, 1);
    assert(layout_second != nullptr);
    const uint32_t val = layout_second->header.next_order_id.load(std::memory_order_relaxed);
    assert(val == 77);

    first.close();
    second.close();
    cleanup_shm(name);
}

TEST(size_mismatch) {
    using namespace acct_service;

    const std::string name = unique_shm_name("shm_mgr_size_mismatch");
    cleanup_shm(name);

    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    assert(fd >= 0);

    const std::size_t small_size = sizeof(shm_header);
    const int trunc_rc = ::ftruncate(fd, static_cast<off_t>(small_size));
    assert(trunc_rc == 0);
    ::close(fd);

    shm_manager mgr;
    auto* layout = mgr.open_upstream(name, shm_mode::Open, 1);
    assert(layout == nullptr);

    mgr.close();
    cleanup_shm(name);
}

int main() {
    printf("=== Shm Manager Test Suite ===\n\n");

    RUN_TEST(create_and_open);
    RUN_TEST(open_or_create_no_reinit);
    RUN_TEST(size_mismatch);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
