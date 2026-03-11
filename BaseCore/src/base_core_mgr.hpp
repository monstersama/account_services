#pragma once

#include <string>

/**
 * BaseCore 统一管理器：单例，目前仅作为占位/扩展点，不再直接负责写日志。
 */
class BaseCoreMgr {
public:
    static BaseCoreMgr* instance() noexcept;

    BaseCoreMgr(const BaseCoreMgr&) = delete;
    BaseCoreMgr& operator=(const BaseCoreMgr&) = delete;

    // 初始化基础设施（目前仅占位，返回 true 表示成功）
    bool init();

    // 关闭并释放资源
    void shutdown() noexcept;

private:
    BaseCoreMgr() = default;
    ~BaseCoreMgr() noexcept = default;
};
