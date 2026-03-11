#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace shm {

/// 从配置文件读取共享内存 name/size 列表
/// 格式：每行 name = xxx, size = 111
/// size 支持单位：纯数字为字节，M/MB 为兆字节，G/GB 为吉字节
class ShmConfigReader {
public:
    /// 从文件加载配置
    /// @param filename 配置文件路径
    /// @return map: name -> size，失败返回空 map
    std::map<std::string, std::size_t> load(const std::string& filename) const;
};

}  // namespace shm
