/**
 * shm_latest_simple_test: 简化版测试，不依赖日志系统
 */

#include "shm/shm_seqlock.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

// 简单数据结构
struct alignas(64) MarketData {
    uint64_t timestamp_ns;
    uint32_t instrument_id;
    uint32_t seq;
    double price;
    double volume;
    char padding[40];  // 对齐到64字节
};

static_assert(sizeof(MarketData) == 128, "MarketData size should be 128 bytes");

constexpr const char* kTestShmName = "shm_latest_simple.tmp";
constexpr int kTestDurationSec = 3;
constexpr int kNumReaders = 2;

std::atomic<uint64_t> g_writer_count{0};
std::atomic<uint64_t> g_reader_count[kNumReaders]{};
std::atomic<bool> g_stop{false};

void writer_thread() {
    shm::ShmSeqLockWriter<MarketData> writer;
    
    // 重试几次打开
    for (int retry = 0; retry < 10; ++retry) {
        if (writer.open(kTestShmName)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    if (!writer.is_open()) {
        std::cerr << "Writer failed to open\n";
        return;
    }

    MarketData data{};
    data.instrument_id = 600000;
    uint64_t seq = 0;
    
    while (!g_stop.load()) {
        data.timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
        data.seq = static_cast<uint32_t>(++seq);
        data.price = 10.0 + (seq % 100) * 0.01;
        data.volume = 1000.0 + (seq % 50) * 10;
        
        if (writer.write(data)) {
            g_writer_count++;
        }
    }
}

void reader_thread(int reader_id) {
    shm::ShmSeqLockReader<MarketData> reader;
    
    // 重试几次打开
    for (int retry = 0; retry < 10; ++retry) {
        if (reader.open(kTestShmName)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!reader.is_open()) {
        std::cerr << "Reader " << reader_id << " failed to open\n";
        return;
    }

    MarketData data{};
    uint32_t last_seq = 0;
    
    while (!g_stop.load()) {
        if (reader.read_latest(data)) {
            g_reader_count[reader_id]++;
            
            // 简单验证
            if (data.seq < last_seq) {
                std::cerr << "Reader " << reader_id << " seq regression: " 
                          << last_seq << " -> " << data.seq << "\n";
            }
            last_seq = data.seq;
        }
        
        // 模拟处理时间
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

int main() {
    std::cout << "=== ShmLatest Simple Test ===\n";
    std::cout << "Data size: " << sizeof(MarketData) << " bytes\n";
    std::cout << "Duration: " << kTestDurationSec << " seconds\n";
    std::cout << "Readers: " << kNumReaders << "\n\n";

    // 清理旧共享内存
    shm_unlink(kTestShmName);
    
    // 创建共享内存
    std::cout << "Creating shared memory...\n";
    shm::ShmSeqLockWriter<MarketData> init_writer;
    if (!init_writer.open(kTestShmName)) {
        std::cerr << "Failed to create shared memory\n";
        return 1;
    }
    
    // 写入初始化数据
    MarketData init_data{};
    init_data.seq = 0;
    init_writer.write(init_data);
    std::cout << "Shared memory created successfully\n\n";
    
    init_writer.close();

    // 启动线程
    std::cout << "Starting threads...\n";
    std::thread writer(writer_thread);
    
    std::vector<std::thread> readers;
    for (int i = 0; i < kNumReaders; ++i) {
        readers.emplace_back(reader_thread, i);
    }

    // 等待
    std::this_thread::sleep_for(std::chrono::seconds(kTestDurationSec));
    g_stop.store(true);

    // 等待结束
    writer.join();
    for (auto& t : readers) {
        t.join();
    }

    // 结果
    std::cout << "\n=== Results ===\n";
    uint64_t writes = g_writer_count.load();
    std::cout << "Writes: " << writes << " (" << (writes / kTestDurationSec) << " ops/sec)\n";
    
    uint64_t total_reads = 0;
    for (int i = 0; i < kNumReaders; ++i) {
        uint64_t reads = g_reader_count[i].load();
        total_reads += reads;
        std::cout << "Reader " << i << ": " << reads << " reads\n";
    }
    
    std::cout << "\nTotal reads: " << total_reads << "\n";
    
    // 清理
    shm_unlink(kTestShmName);
    
    std::cout << "\nTest completed!\n";
    return 0;
}
