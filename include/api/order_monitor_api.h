#ifndef ACCT_ORDER_MONITOR_API_H
#define ACCT_ORDER_MONITOR_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef ACCT_API_EXPORT
#define ACCT_MON_API __declspec(dllexport)
#else
#define ACCT_MON_API __declspec(dllimport)
#endif
#else
#ifdef ACCT_API_EXPORT
#define ACCT_MON_API __attribute__((visibility("default")))
#else
#define ACCT_MON_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============ 错误码 ============
typedef enum {
    ACCT_MON_OK = 0,                   // 成功
    ACCT_MON_ERR_NOT_INITIALIZED = -1, // 上下文未初始化或已关闭
    ACCT_MON_ERR_INVALID_PARAM = -2,   // 参数非法（空指针/格式错误）
    ACCT_MON_ERR_SHM_FAILED = -3,      // 共享内存打开/映射/校验失败
    ACCT_MON_ERR_NOT_FOUND = -4,       // 索引不可见（如 index >= next_index）
    ACCT_MON_ERR_RETRY = -5,           // 与写进程并发冲突，建议短暂退避后重试
    ACCT_MON_ERR_INTERNAL = -99,       // 内部错误
} acct_mon_error_t;

// 监控上下文句柄（不透明指针）
typedef struct acct_orders_monitor_context* acct_orders_mon_ctx_t;

#define ACCT_MON_TRADING_DAY_LEN 8
#define ACCT_MON_SECURITY_ID_LEN 16
#define ACCT_MON_BROKER_ORDER_ID_LEN 32

// ============ 订单槽位阶段 ============
typedef enum {
    ACCT_MON_STAGE_EMPTY = 0,               // 空槽位
    ACCT_MON_STAGE_RESERVED = 1,            // 已预留
    ACCT_MON_STAGE_UPSTREAM_QUEUED = 2,     // 已入上游队列
    ACCT_MON_STAGE_UPSTREAM_DEQUEUED = 3,   // 已被账户服务上游消费
    ACCT_MON_STAGE_RISK_REJECTED = 4,       // 风控拒绝
    ACCT_MON_STAGE_DOWNSTREAM_QUEUED = 5,   // 已入下游队列
    ACCT_MON_STAGE_DOWNSTREAM_DEQUEUED = 6, // 已被 gateway 消费
    ACCT_MON_STAGE_TERMINAL = 7,            // 终态
    ACCT_MON_STAGE_QUEUE_PUSH_FAILED = 8,   // 队列推送失败
} acct_mon_order_stage_t;

// ============ 订单来源 ============
typedef enum {
    ACCT_MON_SOURCE_UNKNOWN = 0,          // 未知
    ACCT_MON_SOURCE_STRATEGY = 1,         // 策略/API 提交
    ACCT_MON_SOURCE_ACCOUNT_INTERNAL = 2, // 账户服务内部生成（拆单/内部撤单等）
} acct_mon_order_source_t;

// ============ 打开参数 ============
typedef struct acct_orders_mon_options {
    const char* orders_shm_name;  // 订单池 SHM 基础名（默认 "/orders_shm"）
    const char* trading_day;      // 交易日 YYYYMMDD（默认 ACCT_TRADING_DAY，否则 "19700101"）
} acct_orders_mon_options_t;

// ============ 订单池头部信息 ============
typedef struct acct_orders_mon_info {
    uint32_t magic;                           // 订单池 magic
    uint32_t version;                         // 订单池版本
    uint32_t capacity;                        // 槽位容量
    uint32_t next_index;                      // 已发布上界（增量轮询游标）
    uint64_t full_reject_count;               // 池满拒单计数
    uint64_t create_time_ns;                  // 创建时间（Unix Epoch ns）
    uint64_t last_update_ns;                  // 最近更新时间（Unix Epoch ns）
    char trading_day[ACCT_MON_TRADING_DAY_LEN + 1];  // 交易日字符串（以 '\0' 结尾）
} acct_orders_mon_info_t;

// ============ 订单快照 ============
// 说明：这是稳定 C ABI 快照结构，不等价于内部 order_request 内存布局。
typedef struct acct_orders_mon_snapshot {
    uint32_t index;          // 槽位索引
    uint64_t seq;            // seqlock 序号（偶数稳定，奇数写入中）
    uint64_t last_update_ns; // 槽位最后更新时间（Unix Epoch ns）

    uint8_t stage;                // acct_mon_order_stage_t
    uint8_t source;               // acct_mon_order_source_t
    uint8_t order_type;           // 0=NotSet,1=New,2=Cancel,255=Unknown
    uint8_t trade_side;           // 0=NotSet,1=Buy,2=Sell
    uint8_t market;               // 1=SZ,2=SH,3=BJ,4=HK
    uint8_t order_status;         // 内部状态码（见 docs/order_monitor_sdk.md）
    uint16_t internal_security_id; // 内部证券 ID

    uint32_t internal_order_id;      // 系统内部订单 ID
    uint32_t orig_internal_order_id; // 原始订单 ID（撤单对应被撤单 ID）

    uint32_t md_time_driven;          // 触发时间 HHMMSSmmm
    uint32_t md_time_entrust;         // 委托时间 HHMMSSmmm
    uint32_t md_time_cancel_sent;     // 撤单发送时间 HHMMSSmmm
    uint32_t md_time_cancel_done;     // 撤单完成时间 HHMMSSmmm
    uint32_t md_time_broker_response; // 柜台响应时间 HHMMSSmmm
    uint32_t md_time_market_response; // 交易所响应时间 HHMMSSmmm
    uint32_t md_time_traded_first;    // 首次成交时间 HHMMSSmmm
    uint32_t md_time_traded_latest;   // 最近成交时间 HHMMSSmmm

    uint64_t volume_entrust;     // 委托数量
    uint64_t volume_traded;      // 已成交数量
    uint64_t volume_remain;      // 剩余数量
    uint64_t dprice_entrust;     // 委托价格（分）
    uint64_t dprice_traded;      // 成交均价（分）
    uint64_t dvalue_traded;      // 已成交金额（分）
    uint64_t dfee_estimate;      // 预估手续费（分）
    uint64_t dfee_executed;      // 已发生手续费（分）
    uint64_t broker_order_id_u64; // 柜台订单号数字视图

    char security_id[ACCT_MON_SECURITY_ID_LEN];         // 证券代码字符串
    char broker_order_id[ACCT_MON_BROKER_ORDER_ID_LEN]; // 柜台订单号字符串
} acct_orders_mon_snapshot_t;

/**
 * @brief 打开订单池监控上下文（只读）
 * @param options 打开参数，可传 NULL 使用默认值
 * @param out_ctx 输出上下文，成功后非空
 * @return 错误码
 */
ACCT_MON_API acct_mon_error_t acct_orders_mon_open(
    const acct_orders_mon_options_t* options, acct_orders_mon_ctx_t* out_ctx);

/**
 * @brief 关闭监控上下文并释放资源
 * @param ctx 监控上下文
 * @return 错误码
 */
ACCT_MON_API acct_mon_error_t acct_orders_mon_close(acct_orders_mon_ctx_t ctx);

/**
 * @brief 读取订单池头部信息
 * @param ctx 监控上下文
 * @param out_info 输出池头信息
 * @return 错误码
 */
ACCT_MON_API acct_mon_error_t acct_orders_mon_info(acct_orders_mon_ctx_t ctx, acct_orders_mon_info_t* out_info);

/**
 * @brief 按索引读取订单快照
 * @param ctx 监控上下文
 * @param index 订单槽位索引
 * @param out_snapshot 输出订单快照
 * @return 错误码
 * @note 返回 ACCT_MON_ERR_RETRY 表示并发读冲突，调用方应短暂退避后重试
 */
ACCT_MON_API acct_mon_error_t acct_orders_mon_read(
    acct_orders_mon_ctx_t ctx, uint32_t index, acct_orders_mon_snapshot_t* out_snapshot);

/**
 * @brief 获取错误码描述
 * @param err 错误码
 * @return 描述字符串
 */
ACCT_MON_API const char* acct_orders_mon_strerror(acct_mon_error_t err);

#ifdef __cplusplus
}
#endif

#endif  // ACCT_ORDER_MONITOR_API_H
