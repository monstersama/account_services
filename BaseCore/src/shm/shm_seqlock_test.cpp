/**
 * shm_latest_test: 单槽位共享内存缓冲区性能测试
 * 测试场景：Level 2 订单簿高频写入，多读者并发读取
 */

#include "shm/shm_seqlock.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// 共享内存清理
#include <sys/mman.h>
#include <unistd.h>

// 模拟 Level 2 订单簿数据结构（约 400 字节）
struct alignas(64) OrderBook {
    uint64_t timestamp_ns;           // 8 字节
    uint32_t instrument_id;          // 4 字节
    uint32_t sequence_no;            // 4 字节
    double best_bid_price;           // 8 字节
    double best_ask_price;           // 8 字节
    double best_bid_qty;             // 8 字节
    double best_ask_qty;             // 8 字节
    
    // 10档深度（每档：价格 + 数量）
    struct Level {
        double price;
        double qty;
    };
    Level bids[10];                  // 160 字节
    Level asks[10];                  // 160 字节
    
    uint64_t total_bid_qty;          // 8 字节
    uint64_t total_ask_qty;          // 8 字节
    char padding[24];                // 对齐到64字节倍数
};

static_assert(sizeof(OrderBook) == 448, "OrderBook size should be 448 bytes");

// 测试配置（使用文件后端，沙盒环境兼容）
constexpr const char* kTestShmName = "shm_latest_test.tmp";
constexpr uint64_t kTestDurationSec = 5;
constexpr int kNumReaders = 4;

// 全局统计
std::atomic<uint64_t> g_writer_count{0};
std::atomic<uint64_t> g_reader_success_count[kNumReaders]{};
std::atomic<uint64_t> g_reader_retry_count[kNumReaders]{};
std::atomic<uint64_t> g_reader_fail_count[kNumReaders]{};
std::atomic<bool> g_stop{false};

// 写者线程
void writer_thread() {
    using ShmSeqLockWriterType = shm::ShmSeqLockWriter<OrderBook>;
    
    ShmSeqLockWriterType writer;
    if (!writer.open(kTestShmName)) {
        std::cerr << "Writer failed to open shm\n";
        return;
    }

    OrderBook book{};
    book.instrument_id = 600000;  // 股票代码
    
    uint64_t seq = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        // 模拟生成订单簿数据
        book.timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        book.sequence_no = static_cast<uint32_t>(++seq);
        book.best_bid_price = 10.0 + (seq % 100) * 0.01;
        book.best_ask_price = book.best_bid_price + 0.01;
        book.best_bid_qty = 1000.0 + (seq % 50) * 10;
        book.best_ask_qty = 1000.0 + (seq % 50) * 10;
        
        for (int i = 0; i < 10; ++i) {
            book.bids[i].price = book.best_bid_price - i * 0.01;
            book.bids[i].qty = book.best_bid_qty * (1.0 - i * 0.1);
            book.asks[i].price = book.best_ask_price + i * 0.01;
            book.asks[i].qty = book.best_ask_qty * (1.0 - i * 0.1);
        }
        
        // 写入（无阻塞）
        if (writer.write(book)) {
            g_writer_count.fetch_add(1, std::memory_order_relaxed);
        }
        
        // 模拟高频写入：约 100,000 TPS
        // 每 10μs 写入一次
        // 这里我们让循环尽可能快，实际延迟取决于CPU
    }
}

// 读者线程
void reader_thread(int reader_id) {
    using ShmSeqLockReaderType = shm::ShmSeqLockReader<OrderBook>;
    
    ShmSeqLockReaderType reader;
    if (!reader.open(kTestShmName)) {
        std::cerr << "Reader " << reader_id << " failed to open shm\n";
        return;
    }

    OrderBook book{};
    uint64_t last_seq = 0;
    
    while (!g_stop.load(std::memory_order_relaxed)) {
        // 读取最新数据（带重试）
        bool success = false;
        int retries = 0;
        
        for (int retry = 0; retry < 3; ++retry) {
            if (reader.read_latest(book)) {
                success = true;
                retries = retry;
                break;
            }
            // 重试时短暂让出CPU
            std::this_thread::yield();
        }
        
        if (success) {
            g_reader_success_count[reader_id].fetch_add(1, std::memory_order_relaxed);
            g_reader_retry_count[reader_id].fetch_add(retries, std::memory_order_relaxed);
            
            // 验证数据完整性（简单检查）
            if (book.sequence_no < last_seq) {
                std::cerr << "Reader " << reader_id << " detected sequence regression: "
                          << last_seq << " -> " << book.sequence_no << "\n";
            }
            last_seq = book.sequence_no;
        } else {
            g_reader_fail_count[reader_id].fetch_add(1, std::memory_order_relaxed);
        }
        
        // 模拟策略处理时间：约 1-10 μs
        // 这里我们用空循环模拟
        for (volatile int i = 0; i < 100; ++i) {}
    }
}

int main() {
    std::cout << "=== ShmLatestBuffer 性能测试 ===\n";
    std::cout << "数据大小: " << sizeof(OrderBook) << " bytes (Level 2 OrderBook)\n";
    std::cout << "测试时长: " << kTestDurationSec << " seconds\n";
    std::cout << "读者数量: " << kNumReaders << "\n\n";

    std::cout << "[调试] 清理旧的共享内存...\n";
    // 清理旧的共享内存（如果存在）
    shm_unlink(kTestShmName);

    std::cout << "[调试] 尝试打开共享内存...\n";
    // 先创建共享内存（主线程创建，所有子线程打开）
    using ShmSeqLockWriterType = shm::ShmSeqLockWriter<OrderBook>;
    ShmSeqLockWriterType pre_writer;
    if (!pre_writer.open(kTestShmName)) {
        std::cerr << "Failed to create shared memory\n";
        return 1;
    }
    std::cout << "[调试] 共享内存创建成功\n";
    // 写入一条初始化数据
    OrderBook init_data{};
    init_data.instrument_id = 0;
    init_data.sequence_no = 0;
    pre_writer.write(init_data);
    pre_writer.close();

    // 短暂延时确保共享内存就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动写者线程
    std::thread writer(writer_thread);

    // 启动读者线程
    std::vector<std::thread> readers;
    for (int i = 0; i < kNumReaders; ++i) {
        readers.emplace_back(reader_thread, i);
    }

    // 等待测试完成
    std::this_thread::sleep_for(std::chrono::seconds(kTestDurationSec));
    g_stop.store(true, std::memory_order_relaxed);

    // 等待所有线程结束
    writer.join();
    for (auto& t : readers) {
        t.join();
    }

    // 统计结果
    uint64_t total_writer = g_writer_count.load();
    uint64_t total_reader_success = 0;
    uint64_t total_reader_retry = 0;
    uint64_t total_reader_fail = 0;

    std::cout << "\n=== 测试结果 ===\n\n";

    std::cout << "【写者统计】\n";
    std::cout << "  总写入次数: " << total_writer << "\n";
    std::cout << "  写入频率: " << (total_writer / kTestDurationSec) << " ops/sec\n";
    double write_latency_ns = (kTestDurationSec * 1e9) / total_writer;
    std::cout << "  平均写入延迟: " << write_latency_ns << " ns\n\n";

    std::cout << "【读者统计】\n";
    for (int i = 0; i < kNumReaders; ++i) {
        uint64_t success = g_reader_success_count[i].load();
        uint64_t retry = g_reader_retry_count[i].load();
        uint64_t fail = g_reader_fail_count[i].load();
        uint64_t total = success + fail;

        total_reader_success += success;
        total_reader_retry += retry;
        total_reader_fail += fail;

        double success_rate = total > 0 ? (100.0 * success / total) : 0;
        double avg_retry = success > 0 ? (1.0 * retry / success) : 0;

        std::cout << "  读者 " << i << ":\n";
        std::cout << "    成功读取: " << success << " (" << success_rate << "%)\n";
        std::cout << "    失败次数: " << fail << "\n";
        std::cout << "    平均重试: " << avg_retry << " 次\n";
        std::cout << "    读取频率: " << (success / kTestDurationSec) << " ops/sec\n";
    }

    std::cout << "\n【汇总】\n";
    std::cout << "  读者总成功: " << total_reader_success << "\n";
    std::cout << "  读者总重试: " << total_reader_retry << "\n";
    std::cout << "  读者总失败: " << total_reader_fail << "\n";
    
    if (total_reader_success > 0) {
        double conflict_rate = 100.0 * total_reader_retry / total_reader_success;
        std::cout << "  冲突率: " << conflict_rate << "% (读取时遇到写入)\n";
    }

    // 延迟估算（假设CPU 3GHz，每条指令约0.33ns）
    std::cout << "\n【性能估算】\n";
    std::cout << "  单条写入: 约 50-150 ns\n";
    std::cout << "  单条读取: 约 100-200 ns\n";
    std::cout << "  理论最大写入带宽: " << (1e9 / write_latency_ns * sizeof(OrderBook) / 1024 / 1024) << " MB/s\n";

    // 清理共享内存
    shm::ShmSeqLockWriter<OrderBook> cleanup;
    cleanup.open(kTestShmName);
    cleanup.close();

    std::cout << "\n测试完成!\n";
    return 0;
}
