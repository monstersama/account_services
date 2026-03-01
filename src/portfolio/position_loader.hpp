#pragma once

#include <string>

#include "common/types.hpp"

namespace acct_service {

class position_manager;

// 从外部快照（db/file）加载初始持仓，仅在 fresh SHM 初始化时生效。
class position_loader {
public:
    struct file_source {
        std::string path;
    };

    struct db_source {
        std::string path;
    };

    // 使用配置文件路径初始化文件模式 loader。
    explicit position_loader(file_source source);
    // 使用 sqlite 数据库路径初始化 DB 模式 loader。
    explicit position_loader(db_source source);
    ~position_loader() = default;

    // 按构造时选择的模式加载持仓快照。
    bool load(account_id_t account_id, position_manager& manager);

private:
    enum class source_type { File, Db };

    bool load_from_file(position_manager& manager) const;
    bool load_from_db(account_id_t account_id, position_manager& manager) const;

    source_type source_type_{source_type::File};
    std::string source_path_;
};

}  // namespace acct_service
