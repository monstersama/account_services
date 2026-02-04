/**
 * 使用 libacct_order.so 的示例程序
 *
 * 演示如何：
 * 1. 创建共享内存（模拟账户服务）
 * 2. 初始化 API 上下文
 * 3. 提交订单
 * 4. 查询队列状态
 * 5. 清理资源
 */

#include "api/order_api.h"

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
    printf("=== libacct_order.so 使用示例 ===\n\n");

    // 打印版本
    printf("API Version: %s\n\n", acct_version());

    // 1. 创建共享内存（实际场景由账户服务创建）
    printf("--- 步骤1: 创建共享内存 ---\n");
    void* shm_ptr = create_shm();
    if (!shm_ptr) {
        fprintf(stderr, "创建共享内存失败\n");
        return 1;
    }

    // 2. 初始化 API 上下文
    printf("\n--- 步骤2: 初始化 API ---\n");
    acct_ctx_t ctx = acct_init();
    if (!ctx) {
        fprintf(stderr, "初始化失败: %s\n", acct_strerror(ACCT_ERR_SHM_FAILED));
        destroy_shm(shm_ptr);
        return 1;
    }
    printf("[Client] API 上下文初始化成功\n");

    // 3. 提交订单
    printf("\n--- 步骤3: 提交订单 ---\n");

    uint32_t order_id = acct_submit_order(
        ctx,
        "000001",           // 证券代码: 平安银行
        ACCT_SIDE_BUY,      // 买入
        ACCT_MARKET_SZ,     // 深圳市场
        100,                // 数量: 100股
        10.5,               // 价格: 10.5 元
        93000000            // 行情时间: 09:30:00.000
    );

    if (order_id != 0) {
        printf("[Client] 订单提交成功, order_id=%u\n", order_id);
    } else {
        fprintf(stderr, "[Client] 订单提交失败\n");
    }

    // 提交第二个订单
    order_id = acct_submit_order(
        ctx,
        "600519",           // 证券代码: 贵州茅台
        ACCT_SIDE_SELL,     // 卖出
        ACCT_MARKET_SH,     // 上海市场
        10,                 // 数量: 10股
        1800.0,             // 价格: 1800.0 元
        93001000            // 行情时间: 09:30:01.000
    );

    if (order_id != 0) {
        printf("[Client] 订单提交成功, order_id=%u\n", order_id);
    } else {
        fprintf(stderr, "[Client] 订单提交失败\n");
    }

    // 4. 查询队列状态
    printf("\n--- 步骤4: 查询队列状态 ---\n");
    size_t queue_size = acct_queue_size(ctx);
    printf("[Client] 当前队列中有 %zu 个订单\n", queue_size);

    // 5. 使用 new_order + send_order 分步操作
    printf("\n--- 步骤5: 分步创建和发送订单 ---\n");

    order_id = acct_new_order(
        ctx,
        "300750",           // 证券代码: 宁德时代
        ACCT_SIDE_BUY,
        ACCT_MARKET_SZ,
        50,
        250.0,              // 250.0 元
        93002000
    );

    if (order_id != 0) {
        printf("[Client] 订单已创建, order_id=%u (未发送)\n", order_id);

        // 发送订单
        acct_error_t err = acct_send_order(ctx, order_id);
        if (err == ACCT_OK) {
            printf("[Client] 订单已发送, order_id=%u\n", order_id);
        } else {
            fprintf(stderr, "[Client] 发送失败: %s\n", acct_strerror(err));
        }
    }

    // 最终队列状态
    printf("\n--- 最终状态 ---\n");
    printf("[Client] 队列中共有 %zu 个订单\n", acct_queue_size(ctx));

    // 6. 清理
    printf("\n--- 步骤6: 清理资源 ---\n");
    acct_destroy(ctx);
    printf("[Client] API 上下文已销毁\n");

    destroy_shm(shm_ptr);

    printf("\n=== 示例完成 ===\n");
    return 0;
}
