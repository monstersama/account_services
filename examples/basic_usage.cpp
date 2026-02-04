/**
 * 使用 libacct_order.so C++ API 的示例程序
 *
 * 演示如何：
 * 1. 创建共享内存（模拟账户服务）
 * 2. 使用 C++ API 初始化
 * 3. 提交订单
 * 4. 查询队列状态
 * 5. 资源自动清理（RAII）
 */

#include "api/order_api.hpp"

// 内部头文件（仅用于创建共享内存，实际使用时由账户服务创建）
#include "common/constants.hpp"
#include "shm/shm_layout.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>

// 前向声明，避免 <unistd.h> 中 acct() 与 namespace acct 冲突
extern "C" {
    int close(int __fd) noexcept;
    int ftruncate(int __fd, long __length) noexcept;
    int shm_unlink(const char* __name) noexcept;
}

// 创建共享内存（模拟账户服务）
static void* create_shm() {
    // 先尝试删除已存在的
    shm_unlink(acct::kStrategyOrderShmName);

    int fd = shm_open(acct::kStrategyOrderShmName, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return nullptr;
    }

    size_t size = sizeof(acct::upstream_shm_layout);
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ptr == MAP_FAILED) {
        perror("mmap");
        return nullptr;
    }

    // 初始化共享内存布局
    auto* layout = new (ptr) acct::upstream_shm_layout();
    layout->header.magic = acct::shm_header::kMagic;
    layout->header.version = acct::shm_header::kVersion;
    layout->header.create_time = 0;
    layout->header.last_update = 0;

    printf("[Server] 共享内存已创建: %s (size=%zu bytes)\n",
           acct::kStrategyOrderShmName, size);

    return ptr;
}

// 销毁共享内存
static void destroy_shm(void* ptr) {
    if (ptr) {
        munmap(ptr, sizeof(acct::upstream_shm_layout));
    }
    shm_unlink(acct::kStrategyOrderShmName);
    printf("[Server] 共享内存已销毁\n");
}

int main() {
    using namespace acct;

    printf("=== libacct_order.so C++ API 使用示例 ===\n\n");

    // 打印版本
    printf("API Version: %s\n\n", OrderApi::version());

    // 1. 创建共享内存（实际场景由账户服务创建）
    printf("--- 步骤1: 创建共享内存 ---\n");
    void* shm_ptr = create_shm();
    if (!shm_ptr) {
        fprintf(stderr, "创建共享内存失败\n");
        return 1;
    }

    // 2. 初始化 API（使用 C++ API）
    printf("\n--- 步骤2: 初始化 API ---\n");
    Error err;
    auto api = OrderApi::create(&err);
    if (!api) {
        fprintf(stderr, "初始化失败: %s (错误码: %d)\n",
                OrderApi::errorString(err), static_cast<int>(err));
        destroy_shm(shm_ptr);
        return 1;
    }
    printf("[Client] API 初始化成功\n");

    // 3. 提交订单
    printf("\n--- 步骤3: 提交订单 ---\n");

    uint32_t order_id = api->submitOrder(
        "000001",           // 证券代码: 平安银行
        Side::Buy,          // 买入
        Market::SZ,         // 深圳市场
        100,                // 数量: 100股
        10.5,               // 价格: 10.5 元
        0                   // valid_sec: 保留参数（暂未使用）
    );

    if (order_id != 0) {
        printf("[Client] 订单提交成功, order_id=%u\n", order_id);
    } else {
        fprintf(stderr, "[Client] 订单提交失败\n");
    }

    // 提交第二个订单
    order_id = api->submitOrder(
        "600519",           // 证券代码: 贵州茅台
        Side::Sell,         // 卖出
        Market::SH,         // 上海市场
        10,                 // 数量: 10股
        1800.0,             // 价格: 1800.0 元
        0                   // valid_sec: 保留参数（暂未使用）
    );

    if (order_id != 0) {
        printf("[Client] 订单提交成功, order_id=%u\n", order_id);
    } else {
        fprintf(stderr, "[Client] 订单提交失败\n");
    }

    // 4. 查询队列状态
    printf("\n--- 步骤4: 查询队列状态 ---\n");
    size_t queue_size = api->queueSize();
    printf("[Client] 当前队列中有 %zu 个订单\n", queue_size);

    // 5. 使用 newOrder + sendOrder 分步操作
    printf("\n--- 步骤5: 分步创建和发送订单 ---\n");

    order_id = api->newOrder(
        "300750",           // 证券代码: 宁德时代
        Side::Buy,
        Market::SZ,
        50,
        250.0,              // 250.0 元
        0                   // valid_sec: 保留参数（暂未使用）
    );

    if (order_id != 0) {
        printf("[Client] 订单已创建, order_id=%u (未发送)\n", order_id);

        // 发送订单
        Error send_err = api->sendOrder(order_id);
        if (send_err == Error::Ok) {
            printf("[Client] 订单已发送, order_id=%u\n", order_id);
        } else {
            fprintf(stderr, "[Client] 发送失败: %s\n", OrderApi::errorString(send_err));
        }
    }

    // 最终队列状态
    printf("\n--- 最终状态 ---\n");
    printf("[Client] 队列中共有 %zu 个订单\n", api->queueSize());

    // 6. 清理（C++ API 使用 RAII，析构时自动清理）
    printf("\n--- 步骤6: 资源自动清理 ---\n");
    printf("[Client] API 资源将在作用域结束时自动清理\n");

    destroy_shm(shm_ptr);

    printf("\n=== 示例完成 ===\n");
    return 0;
}
