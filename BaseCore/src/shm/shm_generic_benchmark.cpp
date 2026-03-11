/**
 * shm_benchmark: ShmGenericWriter / ShmGenericReader 性能测试
 * 用法: shm_benchmark [iterations] [block_size] [cpu_core]
 *   iterations: 默认 1000000
 *   block_size: 每次 write_at/read_at 的字节数，默认 64
 *   cpu_core: 绑定 CPU 核，-1 表示不绑定，默认 0
 * 支持 SHM_USE_FILE=1 时使用文件后端
 */

#include "shm/shm_generic.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

static bool pin_to_cpu(int cpu) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        std::cerr << "shm_benchmark: sched_setaffinity(cpu=" << cpu << ") failed\n";
        return false;
    }
    return true;
#else
    (void)cpu;
    return false;
#endif
}

static std::string make_bench_name() {
    std::ostringstream oss;
    oss << "/tmp/shm_bench_" << getpid();
    const char* env = std::getenv("SHM_USE_FILE");
    if (env && env[0] == '1') {
        return oss.str();
    }
    oss.str("");
    oss << "/shm_bench_" << getpid();
    return oss.str();
}

int main(int argc, char* argv[]) {
    std::size_t iterations = 1000000;
    std::size_t block_size = 64;
    int cpu_core = 0;

    if (argc >= 2) {
        char* end = nullptr;
        const long long n = std::strtoll(argv[1], &end, 10);
        if (end != argv[1] && n > 0 && n <= 100000000) {
            iterations = static_cast<std::size_t>(n);
        }
    }
    if (argc >= 3) {
        char* end = nullptr;
        const long long n = std::strtoll(argv[2], &end, 10);
        if (end != argv[2] && n > 0 && n <= 1024 * 1024) {
            block_size = static_cast<std::size_t>(n);
        }
    }
    if (argc >= 4) {
        char* end = nullptr;
        const long n = std::strtol(argv[3], &end, 10);
        if (end != argv[3] && n >= -1 && n < 256) {
            cpu_core = static_cast<int>(n);
        }
    }

    const std::string name = make_bench_name();
    const std::size_t shm_size = std::max(block_size * 2, std::size_t(65536));

    if (cpu_core >= 0 && pin_to_cpu(cpu_core)) {
        std::cout << "shm_benchmark: pinned to CPU " << cpu_core << "\n";
    }

    const char* env = std::getenv("SHM_USE_FILE");
    std::cout << "shm_benchmark: " << (env && env[0] == '1' ? "file" : "POSIX shm")
              << " backend, iterations=" << iterations << ", block_size=" << block_size
              << ", shm_size=" << shm_size << "\n";

    // 准备数据
    std::vector<char> write_buf(block_size, 'x');
    std::vector<char> read_buf(block_size, 0);

    // ========== write_at 吞吐 ==========
    shm::ShmGenericWriter w;
    if (!shm::create_if_not_exists(name, shm_size)) {
        std::cerr << "shm_benchmark: create_if_not_exists failed\n";
        return 1;
    }
    void* pw = w.open(name, shm_size);
    if (!pw) {
        std::cerr << "shm_benchmark: writer open failed\n";
        return 1;
    }

    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t offset = (i * block_size) % (shm_size - block_size);
        w.write_at(offset, write_buf.data(), block_size);
    }
    auto t1 = std::chrono::steady_clock::now();
    const auto write_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double write_mb = (iterations * block_size) / (1024.0 * 1024.0);
    const double write_sec = write_ns / 1e9;
    std::cout << "write_at: " << (write_mb / write_sec) << " MB/s, "
              << (iterations * 1e9 / write_ns) << " ops/s, "
              << (write_ns / iterations) << " ns/op\n";

    w.close();

    // ========== read_at 吞吐 ==========
    shm::ShmGenericReader r;
    const void* pr = r.open(name, shm_size);
    if (!pr) {
        std::cerr << "shm_benchmark: reader open failed\n";
        shm::ShmGenericWriter::unlink(name);
        return 1;
    }

    t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t offset = (i * block_size) % (shm_size - block_size);
        r.read_at(offset, read_buf.data(), block_size);
    }
    t1 = std::chrono::steady_clock::now();
    const auto read_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const double read_mb = (iterations * block_size) / (1024.0 * 1024.0);
    const double read_sec = read_ns / 1e9;
    std::cout << "read_at:  " << (read_mb / read_sec) << " MB/s, "
              << (iterations * 1e9 / read_ns) << " ops/s, "
              << (read_ns / iterations) << " ns/op\n";

    r.close();
    shm::ShmGenericWriter::unlink(name);

    // ========== 直接 memcpy 对比（无 shm 开销） ==========
    std::vector<char> src(block_size, 'y');
    std::vector<char> dst(block_size, 0);
    t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        std::memcpy(dst.data(), src.data(), block_size);
    }
    t1 = std::chrono::steady_clock::now();
    const auto memcpy_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::cout << "memcpy:   " << (iterations * 1e9 / memcpy_ns) << " ops/s, "
              << (memcpy_ns / iterations) << " ns/op (baseline)\n";

    std::cout << "shm_benchmark: done\n";
    return 0;
}
