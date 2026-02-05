/**
 * 使用 libacct_order.so 的示例程序
 *
 * 演示如何：
 * 1. 初始化 API 上下文（自动创建/打开共享内存）
 * 2. 提交订单
 * 3. 查询队列状态
 * 4. 清理资源
 */

#include <cstdio>

#include "api/order_api.h"

int main() {
    printf("=== libacct_order.so 使用示例 ===\n\n");

    // 打印版本
    printf("API Version: %s\n\n", acct_version());

    // 1. 初始化 API 上下文（自动创建或打开共享内存）
    printf("--- 步骤1: 初始化 API ---\n");
    acct_ctx_t ctx = nullptr;
    acct_error_t err = acct_init(&ctx);
    if (err != ACCT_OK) {
        fprintf(stderr, "初始化失败: %s\n", acct_strerror(err));
        return 1;
    }
    printf("[Client] API 上下文初始化成功\n");

    // 2. 提交订单
    printf("\n--- 步骤2: 提交订单 ---\n");

    uint32_t order_id = 0;
    acct_error_t submit_err = acct_submit_order(ctx,
        "000001",        // 证券代码: 平安银行
        ACCT_SIDE_BUY,   // 买入
        ACCT_MARKET_SZ,  // 深圳市场
        100,             // 数量: 100股
        10.5,            // 价格: 10.5 元
        0,               // valid_sec: 保留参数（暂未使用）
        &order_id);

    if (submit_err == ACCT_OK) {
        printf("[Client] 订单提交成功, order_id=%u\n", order_id);
    } else {
        fprintf(stderr, "[Client] 订单提交失败: %s\n", acct_strerror(submit_err));
    }

    // 提交第二个订单
    submit_err = acct_submit_order(ctx,
        "600519",        // 证券代码: 贵州茅台
        ACCT_SIDE_SELL,  // 卖出
        ACCT_MARKET_SH,  // 上海市场
        10,              // 数量: 10股
        1800.0,          // 价格: 1800.0 元
        0,               // valid_sec: 保留参数（暂未使用）
        &order_id);

    if (submit_err == ACCT_OK) {
        printf("[Client] 订单提交成功, order_id=%u\n", order_id);
    } else {
        fprintf(stderr, "[Client] 订单提交失败: %s\n", acct_strerror(submit_err));
    }

    // 3. 查询队列状态
    printf("\n--- 步骤3: 查询队列状态 ---\n");
    size_t queue_size = 0;
    acct_queue_size(ctx, &queue_size);
    printf("[Client] 当前队列中有 %zu 个订单\n", queue_size);

    // 4. 使用 new_order + send_order 分步操作
    printf("\n--- 步骤4: 分步创建和发送订单 ---\n");

    order_id = 0;
    acct_error_t new_err = acct_new_order(ctx,
        "300750",  // 证券代码: 宁德时代
        ACCT_SIDE_BUY, ACCT_MARKET_SZ, 50,
        250.0,  // 250.0 元
        0,      // valid_sec: 保留参数（暂未使用）
        &order_id);

    if (new_err == ACCT_OK) {
        printf("[Client] 订单已创建, order_id=%u (未发送)\n", order_id);

        // 发送订单
        acct_error_t err = acct_send_order(ctx, order_id);
        if (err == ACCT_OK) {
            printf("[Client] 订单已发送, order_id=%u\n", order_id);
        } else {
            fprintf(stderr, "[Client] 发送失败: %s\n", acct_strerror(err));
        }
    } else {
        fprintf(stderr, "[Client] 订单创建失败: %s\n", acct_strerror(new_err));
    }

    // 最终队列状态
    printf("\n--- 最终状态 ---\n");
    acct_queue_size(ctx, &queue_size);
    printf("[Client] 队列中共有 %zu 个订单\n", queue_size);

    // 5. 清理 API 资源（不删除共享内存）
    printf("\n--- 步骤5: 清理资源 ---\n");
    acct_error_t destroy_err = acct_destroy(ctx);
    if (destroy_err == ACCT_OK) {
        printf("[Client] API 上下文已销毁\n");
    } else {
        fprintf(stderr, "[Client] 销毁失败: %s\n", acct_strerror(destroy_err));
    }

    // 6. 可选：清理共享内存（如果需要重置）
    printf("\n--- 步骤6: 可选 - 清理共享内存 ---\n");
    printf("如果要删除共享内存，调用: acct_cleanup_shm()\n");
    printf("跳过此步骤，共享内存将保留供下次使用\n");
    acct_cleanup_shm();

    printf("\n=== 示例完成 ===\n");
    return 0;
}
