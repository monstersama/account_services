/**
 * log_benchmark: 性能测试，输出每次日志写入的耗时分布与异常分析
 * 用法: log_benchmark [iterations] [cpu_core]
 *   iterations: 默认 100000
 *   cpu_core: 绑定 CPU 核（减少上下文切换），-1 表示不绑定，默认 0
 */

#include "logging/log.hpp"
#include "logging/log_demo_module.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
        std::cerr << "log_benchmark: sched_setaffinity(cpu=" << cpu << ") failed\n";
        return false;
    }
    return true;
#else
    (void)cpu;
    return false;
#endif
}

int main(int argc, char* argv[]) {
    std::size_t iterations = 100000;
    int cpu_core = 0;  // 默认绑定到 core 0

    if (argc >= 2) {
        char* end = nullptr;
        const long long n = std::strtoll(argv[1], &end, 10);
        if (end != argv[1] && n > 0 && n <= 10000000) {
            iterations = static_cast<std::size_t>(n);
        }
    }
    if (argc >= 3) {
        char* end = nullptr;
        const long n = std::strtol(argv[2], &end, 10);
        if (end != argv[2] && n >= -1 && n < 256) {
            cpu_core = static_cast<int>(n);
        }
    }

    if (cpu_core >= 0) {
        if (pin_to_cpu(cpu_core)) {
            std::cout << "log_benchmark: pinned to CPU " << cpu_core << "\n";
        }
    } else {
        std::cout << "log_benchmark: no CPU affinity (may see more context-switch spikes)\n";
    }

    base_core_log::ShmLogConfig config;
    config.shm_name = "/tmp/log_shm_bench";
    config.per_thread = true;

    base_core_log::ShmLogger logger;
    if (!logger.init(config)) {
        std::cerr << "log_benchmark: failed to init ShmLogger\n";
        return 1;
    }

    auto* w = logger.writer();
    if (!w || !w->is_attached()) {
        std::cerr << "log_benchmark: writer not attached\n";
        logger.close();
        return 1;
    }

    using namespace base_core_log;

    std::vector<uint64_t> latencies(iterations);

    for (std::size_t i = 0; i < iterations; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        LOG_STR(*w, LogLevel::info, Module::module1, "Text1");
        const auto t1 = std::chrono::steady_clock::now();
        latencies[i] = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    logger.close();

    // 排序以计算百分位
    std::vector<uint64_t> sorted = latencies;
    std::sort(sorted.begin(), sorted.end());

    const auto percentile = [&sorted](double p) -> uint64_t {
        if (sorted.empty()) return 0;
        const size_t idx = static_cast<size_t>(p * sorted.size() / 100.0);
        return sorted[std::min(idx, sorted.size() - 1)];
    };

    uint64_t sum_ns = 0;
    uint64_t max_ns = 0;
    int count_gt_100ns = 0;
    int count_gt_500ns = 0;
    int count_gt_1us = 0;
    int count_gt_10us = 0;

    for (std::size_t i = 0; i < iterations; ++i) {
        const uint64_t ns = latencies[i];
        sum_ns += ns;
        if (ns > max_ns) max_ns = ns;
        if (ns > 100) ++count_gt_100ns;
        if (ns > 500) ++count_gt_500ns;
        if (ns > 1000) ++count_gt_1us;
        if (ns > 10000) ++count_gt_10us;
    }

    const double avg_ns = static_cast<double>(sum_ns) / static_cast<double>(iterations);

    std::cout << "log_benchmark: " << iterations << " iterations\n"
              << "  average: " << avg_ns << " ns\n"
              << "  p50:     " << percentile(50) << " ns\n"
              << "  p90:     " << percentile(90) << " ns\n"
              << "  p99:     " << percentile(99) << " ns\n"
              << "  p99.9:   " << percentile(99.9) << " ns\n"
              << "  p99.99:  " << percentile(99.99) << " ns\n"
              << "  max:     " << max_ns << " ns\n"
              << "  outliers: >100ns=" << count_gt_100ns
              << "  >500ns=" << count_gt_500ns
              << "  >1us=" << count_gt_1us
              << "  >10us=" << count_gt_10us << "\n";

    // 打印所有 >10us 的索引和耗时，分析是否偶然
    if (count_gt_10us > 0) {
        std::cout << "  spikes >10us (index, ns): ";
        int printed = 0;
        for (std::size_t i = 0; i < iterations && printed < 50; ++i) {
            if (latencies[i] > 10000) {
                std::cout << "(" << i << "," << latencies[i] << ") ";
                ++printed;
            }
        }
        if (count_gt_10us > 50) std::cout << "... (total " << count_gt_10us << " spikes)";
        std::cout << "\n";
    }

    // 打印 >1us 的分布（若数量不多）
    if (count_gt_1us > 0 && count_gt_1us <= 100) {
        std::cout << "  all >1us (index, ns): ";
        for (std::size_t i = 0; i < iterations; ++i) {
            if (latencies[i] > 1000) {
                std::cout << "(" << i << "," << latencies[i] << ") ";
            }
        }
        std::cout << "\n";
    }

    // 尖峰间隔分析：若 >10us 尖峰成簇，可能是调度器/时钟中断
    if (count_gt_10us >= 2) {
        std::vector<std::size_t> spike_indices;
        for (std::size_t i = 0; i < iterations; ++i) {
            if (latencies[i] > 10000) spike_indices.push_back(i);
        }
        std::cout << "  spike gaps (index diff): ";
        for (size_t j = 1; j < spike_indices.size() && j <= 20; ++j) {
            std::cout << (spike_indices[j] - spike_indices[j - 1]) << " ";
        }
        if (spike_indices.size() > 21) std::cout << "...";
        std::cout << "\n";
    }

    if (count_gt_1us > 0 || count_gt_10us > 0) {
        std::cout << "  (spikes often caused by: context switch, timer IRQ, CPU freq scaling.\n"
                  << "   Use: taskset -c 0 ./log_benchmark, or isolate CPU via isolcpus)\n";
    }

    return 0;
}
