#ifndef ACCT_ORDER_API_H
#define ACCT_ORDER_API_H

#include <stddef.h>
#include <stdint.h>

// ===== export.h 内容内联 =====
#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef ACCT_API_EXPORT
        #define ACCT_API __declspec(dllexport)
    #else
        #define ACCT_API __declspec(dllimport)
    #endif
#else
    #ifdef ACCT_API_EXPORT
        #define ACCT_API __attribute__((visibility("default")))
    #else
        #define ACCT_API
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============ 错误码 ============
typedef enum {
    ACCT_OK = 0,
    ACCT_ERR_NOT_INITIALIZED = -1,
    ACCT_ERR_INVALID_PARAM = -2,
    ACCT_ERR_QUEUE_FULL = -3,
    ACCT_ERR_SHM_FAILED = -4,
    ACCT_ERR_ORDER_NOT_FOUND = -5,
    ACCT_ERR_INTERNAL = -99,
} acct_error_t;

// ============ 市场枚举 ============
typedef enum {
    ACCT_MARKET_SZ = 1,
    ACCT_MARKET_SH = 2,
    ACCT_MARKET_BJ = 3,
    ACCT_MARKET_HK = 4,
} acct_market_t;

// ============ 买卖方向 ============
typedef enum {
    ACCT_SIDE_BUY = 1,
    ACCT_SIDE_SELL = 2,
} acct_side_t;

// ============ 上下文句柄 ============
typedef struct acct_context* acct_ctx_t;

// ============ 初始化/销毁 ============

/**
 * @brief 初始化订单 API 上下文
 * @return 上下文句柄，失败返回 NULL
 */
ACCT_API acct_ctx_t acct_init(void);

/**
 * @brief 销毁上下文并释放资源
 * @param ctx 上下文句柄
 */
ACCT_API void acct_destroy(acct_ctx_t ctx);

// ============ 核心订单接口 ============

/**
 * @brief 创建新订单（不发送，仅缓存在本地）
 * @param ctx 上下文
 * @param security_id 证券代码 (如 "000001")
 * @param side 买卖方向 (ACCT_SIDE_BUY / ACCT_SIDE_SELL)
 * @param market 市场 (ACCT_MARKET_SZ / SH / BJ / HK)
 * @param volume 委托数量
 * @param price 委托价格 (单位: 元, 如 10.5 表示 10.5元)
 * @param md_time 订单有效时间(单位: 秒, 0 表示无时间限制)
 * @return 成功返回订单ID，失败返回0
 */
ACCT_API uint32_t acct_new_order(
    acct_ctx_t ctx,
    const char* security_id,
    uint8_t side,
    uint8_t market,
    uint64_t volume,
    double price,
    uint32_t md_time
);

/**
 * @brief 发送已创建的订单到共享内存队列
 * @param ctx 上下文
 * @param order_id 订单ID (由 acct_new_order 返回)
 * @return 错误码
 */
ACCT_API acct_error_t acct_send_order(acct_ctx_t ctx, uint32_t order_id);

/**
 * @brief 创建并发送订单（便捷接口，合并 new_order + send_order）
 * @param ctx 上下文
 * @param security_id 证券代码
 * @param side 买卖方向
 * @param market 市场
 * @param volume 委托数量
 * @param price 委托价格
 * @param md_time 订单有效时间(单位: 秒, 0 表示无时间限制)
 * @return 成功返回订单ID，失败返回0
 */
ACCT_API uint32_t acct_submit_order(
    acct_ctx_t ctx,
    const char* security_id,
    uint8_t side,
    uint8_t market,
    uint64_t volume,
    double price,
    uint32_t md_time
);

// ============ 撤单接口 ============

/**
 * @brief 发送撤单请求
 * @param ctx 上下文
 * @param orig_order_id 要撤销的原订单ID
 * @param md_time 订单有效时间(单位: 秒, 0 表示无时间限制)
 * @return 成功返回撤单请求ID，失败返回0
 */
ACCT_API uint32_t acct_cancel_order(
    acct_ctx_t ctx,
    uint32_t orig_order_id,
    uint32_t md_time
);

// ============ 辅助接口 ============

/**
 * @brief 获取队列当前元素数量
 * @param ctx 上下文
 * @return 队列元素数量
 */
ACCT_API size_t acct_queue_size(acct_ctx_t ctx);

/**
 * @brief 获取错误描述字符串
 * @param err 错误码
 * @return 错误描述
 */
ACCT_API const char* acct_strerror(acct_error_t err);

/**
 * @brief 获取库版本号
 * @return 版本字符串
 */
ACCT_API const char* acct_version(void);

#ifdef __cplusplus
}
#endif

#endif // ACCT_ORDER_API_H
