#ifndef ACCT_POSITION_MONITOR_API_H
#define ACCT_POSITION_MONITOR_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef ACCT_API_EXPORT
#define ACCT_POS_MON_API __declspec(dllexport)
#else
#define ACCT_POS_MON_API __declspec(dllimport)
#endif
#else
#ifdef ACCT_API_EXPORT
#define ACCT_POS_MON_API __attribute__((visibility("default")))
#else
#define ACCT_POS_MON_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============ 错误码 ============
typedef enum {
    ACCT_POS_MON_OK = 0,                   // 成功
    ACCT_POS_MON_ERR_NOT_INITIALIZED = -1, // 上下文未初始化或已关闭
    ACCT_POS_MON_ERR_INVALID_PARAM = -2,   // 参数非法（空指针/格式错误）
    ACCT_POS_MON_ERR_SHM_FAILED = -3,      // 共享内存打开/映射/校验失败
    ACCT_POS_MON_ERR_NOT_FOUND = -4,       // 目标索引不可见
    ACCT_POS_MON_ERR_RETRY = -5,           // 与写进程并发冲突，建议短暂退避后重试
    ACCT_POS_MON_ERR_INTERNAL = -99,       // 内部错误
} acct_pos_mon_error_t;

// 监控上下文句柄（不透明指针）
typedef struct acct_positions_monitor_context* acct_positions_mon_ctx_t;

#define ACCT_POS_MON_POSITION_ID_LEN 16
#define ACCT_POS_MON_POSITION_NAME_LEN 16

// ============ 打开参数 ============
typedef struct acct_positions_mon_options {
    const char* positions_shm_name;  // 持仓 SHM 名称（默认 "/positions_shm"）
} acct_positions_mon_options_t;

// ============ 持仓池头部信息 ============
typedef struct acct_positions_mon_info {
    uint32_t magic;             // 共享内存 magic
    uint32_t version;           // 共享内存版本
    uint32_t capacity;          // positions[] 容量
    uint32_t init_state;        // 0=未完成初始化，1=可读
    uint32_t position_count;    // 可见证券行数（不含资金行）
    uint32_t next_security_id;  // 下一个证券编号（由服务侧维护）
    uint64_t create_time_ns;    // 创建时间（Unix Epoch ns）
    uint64_t last_update_ns;    // 最近更新时间（Unix Epoch ns）
} acct_positions_mon_info_t;

// ============ 资金行快照 ============
typedef struct acct_positions_mon_fund_snapshot {
    uint64_t last_update_ns; // 对应持仓池头部最近更新时间

    char id[ACCT_POS_MON_POSITION_ID_LEN];      // 资金行标识（默认 "FUND"）
    char name[ACCT_POS_MON_POSITION_NAME_LEN];  // 资金行名称

    uint64_t total_asset;   // 总资产（分）
    uint64_t available;     // 可用资金（分）
    uint64_t frozen;        // 冻结资金（分）
    uint64_t market_value;  // 持仓市值（分）
    uint64_t count_order;   // 订单计数
} acct_positions_mon_fund_snapshot_t;

// ============ 证券行快照 ============
typedef struct acct_positions_mon_position_snapshot {
    uint32_t index;      // 证券逻辑索引 [0, position_count)
    uint32_t row_index;  // 共享内存物理行索引（index + 1）
    uint64_t last_update_ns;

    char id[ACCT_POS_MON_POSITION_ID_LEN];      // 内部证券 ID（market.security_id）
    char name[ACCT_POS_MON_POSITION_NAME_LEN];  // 证券名称

    uint64_t available;
    uint64_t volume_available_t0;
    uint64_t volume_available_t1;
    uint64_t volume_buy;
    uint64_t dvalue_buy;
    uint64_t volume_buy_traded;
    uint64_t dvalue_buy_traded;
    uint64_t volume_sell;
    uint64_t dvalue_sell;
    uint64_t volume_sell_traded;
    uint64_t dvalue_sell_traded;
    uint64_t count_order;
} acct_positions_mon_position_snapshot_t;

/**
 * @brief 打开持仓监控上下文（只读）
 * @param options 打开参数，可传 NULL 使用默认值
 * @param out_ctx 输出上下文，成功后非空
 * @return 错误码
 */
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_open(
    const acct_positions_mon_options_t* options, acct_positions_mon_ctx_t* out_ctx);

/**
 * @brief 关闭监控上下文并释放资源
 * @param ctx 监控上下文
 * @return 错误码
 */
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_close(acct_positions_mon_ctx_t ctx);

/**
 * @brief 读取持仓池头部信息
 * @param ctx 监控上下文
 * @param out_info 输出池头信息
 * @return 错误码
 */
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_info(
    acct_positions_mon_ctx_t ctx, acct_positions_mon_info_t* out_info);

/**
 * @brief 读取资金行快照
 * @param ctx 监控上下文
 * @param out_snapshot 输出资金行快照
 * @return 错误码
 * @note 返回 ACCT_POS_MON_ERR_RETRY 表示并发读冲突，调用方应短暂退避后重试
 */
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_read_fund(
    acct_positions_mon_ctx_t ctx, acct_positions_mon_fund_snapshot_t* out_snapshot);

/**
 * @brief 按证券逻辑索引读取证券行快照
 * @param ctx 监控上下文
 * @param index 证券逻辑索引 [0, position_count)
 * @param out_snapshot 输出证券行快照
 * @return 错误码
 * @note 返回 ACCT_POS_MON_ERR_RETRY 表示并发读冲突，调用方应短暂退避后重试
 */
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_read_position(
    acct_positions_mon_ctx_t ctx, uint32_t index, acct_positions_mon_position_snapshot_t* out_snapshot);

/**
 * @brief 获取错误码描述
 * @param err 错误码
 * @return 描述字符串
 */
ACCT_POS_MON_API const char* acct_positions_mon_strerror(acct_pos_mon_error_t err);

#ifdef __cplusplus
}
#endif

#endif  // ACCT_POSITION_MONITOR_API_H
