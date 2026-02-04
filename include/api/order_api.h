#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

// 导出宏定义
#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef ACCT_API_EXPORT
        #define ACCT_API_CPP __declspec(dllexport)
    #else
        #define ACCT_API_CPP __declspec(dllimport)
    #endif
#else
    #ifdef ACCT_API_EXPORT
        #define ACCT_API_CPP __attribute__((visibility("default")))
    #else
        #define ACCT_API_CPP
    #endif
#endif

namespace acct {

// 前向声明内部结构
struct acct_context;

// ============ 错误码枚举 ============
enum class Error {
    Ok = 0,
    NotInitialized = -1,
    InvalidParam = -2,
    QueueFull = -3,
    ShmFailed = -4,
    OrderNotFound = -5,
    // 初始化专用错误码
    InitNoMemory = -6,          // 内存分配失败
    InitShmOpenFailed = -7,     // shm_open() 失败
    InitMmapFailed = -8,        // mmap() 失败
    InitInvalidMagic = -9,      // 魔数验证失败
    Internal = -99,
};

// ============ 市场枚举 ============
enum class Market : uint8_t {
    SZ = 1,  // 深圳
    SH = 2,  // 上海
    BJ = 3,  // 北京
    HK = 4,  // 香港
};

// ============ 买卖方向 ============
enum class Side : uint8_t {
    Buy = 1,
    Sell = 2,
};

// ============ C++ Order API 类 ============
class ACCT_API_CPP OrderApi {
public:
    /**
     * @brief 工厂函数：创建 OrderApi 实例
     * @param out_error 用于存储错误码的指针（可为 nullptr，表示不需要错误信息）
     * @return 成功返回实例，失败返回 nullptr
     * @note 如果提供了 out_error 且初始化失败，*out_error 将被设置为具体错误码
     */
    static std::unique_ptr<OrderApi> create(Error* out_error = nullptr);

    // 析构函数 - 在实现文件中定义
    ~OrderApi();

    // 禁止拷贝
    OrderApi(const OrderApi&) = delete;
    OrderApi& operator=(const OrderApi&) = delete;

    // 允许移动 - 在实现文件中定义
    OrderApi(OrderApi&& other) noexcept;
    OrderApi& operator=(OrderApi&& other) noexcept;

    // ============ 核心订单接口 ============

    /**
     * @brief 创建新订单（不发送，仅缓存在本地）
     * @param security_id 证券代码 (如 "000001")
     * @param side 买卖方向 (Side::Buy / Side::Sell)
     * @param market 市场 (Market::SZ / SH / BJ / HK)
     * @param volume 委托数量
     * @param price 委托价格 (单位: 元, 如 10.5 表示 10.5元)
     * @param valid_sec 保留参数（暂未使用，默认为 0）
     * @return 成功返回订单ID，失败返回 0
     */
    uint32_t newOrder(
        std::string_view security_id,
        Side side,
        Market market,
        uint64_t volume,
        double price,
        uint32_t valid_sec = 0
    );

    /**
     * @brief 发送已创建的订单到共享内存队列
     * @param order_id 订单ID (由 newOrder 返回)
     * @return 错误码 (Error::Ok 表示成功)
     */
    Error sendOrder(uint32_t order_id);

    /**
     * @brief 创建并发送订单（便捷接口，合并 new_order + send_order）
     * @param security_id 证券代码
     * @param side 买卖方向
     * @param market 市场
     * @param volume 委托数量
     * @param price 委托价格
     * @param valid_sec 保留参数（暂未使用，默认为 0）
     * @return 成功返回订单ID，失败返回 0
     */
    uint32_t submitOrder(
        std::string_view security_id,
        Side side,
        Market market,
        uint64_t volume,
        double price,
        uint32_t valid_sec = 0
    );

    /**
     * @brief 发送撤单请求
     * @param orig_order_id 要撤销的原订单ID
     * @param valid_sec 保留参数（暂未使用，默认为 0）
     * @return 成功返回撤单请求ID，失败返回 0
     */
    uint32_t cancelOrder(uint32_t orig_order_id, uint32_t valid_sec = 0);

    // ============ 辅助接口 ============

    /**
     * @brief 获取队列当前元素数量
     * @return 队列元素数量
     */
    size_t queueSize() const;

    /**
     * @brief 检查是否已初始化
     * @return 已初始化返回 true
     */
    bool isInitialized() const noexcept;

    /**
     * @brief 获取错误描述字符串
     * @param err 错误码
     * @return 错误描述
     */
    static const char* errorString(Error err);

    /**
     * @brief 获取库版本号
     * @return 版本字符串
     */
    static const char* version();

private:
    // 私有构造函数，只能通过 create() 创建
    OrderApi();

    // 内部上下文指针
    acct_context* ctx_ = nullptr;
};

} // namespace acct
