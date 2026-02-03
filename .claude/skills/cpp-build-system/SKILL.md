---
name: cpp-build-system
description: 为高性能低延时量化交易账户服务项目生成CMakeLists.txt构建配置文件。自动扫描项目结构，配置优化编译选项，支持模块化构建。
argument-hint: [target-type] [target-name]
disable-model-invocation: false
user-invocable: true
allowed-tools: Read, Write, Glob, Grep, Bash
---

# C++构建系统生成器 - 账户服务专用

为高性能低延时量化交易账户服务项目生成专业的CMakeLists.txt配置文件。专门针对金融交易系统的性能需求进行优化配置。

## 参数说明
- `$0` (target-type): 目标类型 - `executable`(可执行文件), `static_lib`(静态库), `shared_lib`(动态库)
- `$1` (target-name): 目标名称 - 如 `account_service`, `portfolio_lib` 等

## 功能特性
1. **项目结构感知**：自动识别账户服务项目的模块化结构
2. **性能优化**：配置交易系统专用的编译优化选项
3. **内存对齐**：根据缓存行大小(64字节)进行内存对齐优化
4. **依赖检测**：自动配置线程、实时、共享内存等系统依赖
5. **测试集成**：可选集成Google Test框架

## 使用示例
- `/cpp-build-system executable account_service` - 生成账户服务可执行文件配置
- `/cpp-build-system static_lib portfolio` - 生成投资组合模块静态库配置
- `/cpp-build-system shared_lib risk` - 生成风控模块动态库配置

## 项目结构分析

首先分析账户服务项目的特定结构：

```bash
!`find /home/ythe/repositories/account_services -name "*.hpp" -o -name "*.h" -o -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" | sort`
```

```bash
!`ls -la /home/ythe/repositories/account_services/include/*/*.hpp 2>/dev/null | wc -l`
```

## CMakeLists.txt模板

根据账户服务项目特点生成以下配置：

```cmake
# CMake最低版本要求
cmake_minimum_required(VERSION 3.16)

# 项目名称和版本 - 高性能量化交易账户服务
project(account_services VERSION 1.0.0 LANGUAGES CXX DESCRIPTION "高性能低延时量化交易账户服务进程")

# 设置C++标准 - 要求C++17以获得更好的性能特性
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译器和架构检测
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "使用GCC编译器，版本: ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "使用Clang编译器，版本: ${CMAKE_CXX_COMPILER_VERSION}")
else()
    message(WARNING "未知编译器: ${CMAKE_CXX_COMPILER_ID}")
endif()

# 性能优化选项 - 针对低延时交易系统
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # 调试模式：完整的调试信息和基本优化
    add_compile_options(-g -O0 -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    add_compile_options(-Wno-unused-parameter -Wno-missing-field-initializers)
    message(STATUS "构建类型: Debug - 启用调试符号")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    # 发布模式：极致性能优化
    add_compile_options(-O3 -march=native -mtune=native)
    add_compile_options(-flto -fno-fat-lto-objects)
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
    message(STATUS "构建类型: Release - 启用最大性能优化")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # 带调试信息的发布模式
    add_compile_options(-O2 -g -march=native)
    message(STATUS "构建类型: RelWithDebInfo - 平衡性能与调试")
else()
    # 默认模式
    add_compile_options(-O2)
    message(STATUS "构建类型: 默认 - 使用O2优化")
endif()

# 特定于交易系统的优化
add_compile_options(-ftree-vectorize)
add_compile_options(-fno-strict-aliasing)  # 对内存映射I/O更友好
add_compile_options(-funroll-loops)

# 内存对齐优化 - 针对64字节缓存行
add_compile_options(-malign-data=cacheline)

# 包含目录 - 基于账户服务项目结构
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/common
    ${CMAKE_CURRENT_SOURCE_DIR}/include/core
    ${CMAKE_CURRENT_SOURCE_DIR}/include/order
    ${CMAKE_CURRENT_SOURCE_DIR}/include/portfolio
    ${CMAKE_CURRENT_SOURCE_DIR}/include/risk
    ${CMAKE_CURRENT_SOURCE_DIR}/include/shm
)

# 自动收集源文件
file(GLOB_RECURSE COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp)
file(GLOB_RECURSE CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp)
file(GLOB_RECURSE ORDER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/order/*.cpp)
file(GLOB_RECURSE PORTFOLIO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/portfolio/*.cpp)
file(GLOB_RECURSE RISK_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/risk/*.cpp)
file(GLOB_RECURSE SHM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/shm/*.cpp)

# 源文件列表
set(SOURCES
    ${COMMON_SOURCES}
    ${CORE_SOURCES}
    ${ORDER_SOURCES}
    ${PORTFOLIO_SOURCES}
    ${RISK_SOURCES}
    ${SHM_SOURCES}
)

# 打印找到的源文件数量
message(STATUS "找到公共模块源文件: ${#COMMON_SOURCES}个")
message(STATUS "找到核心模块源文件: ${#CORE_SOURCES}个")
message(STATUS "找到订单模块源文件: ${#ORDER_SOURCES}个")
message(STATUS "找到投资组合模块源文件: ${#PORTFOLIO_SOURCES}个")
message(STATUS "找到风控模块源文件: ${#RISK_SOURCES}个")
message(STATUS "找到共享内存模块源文件: ${#SHM_SOURCES}个")

# 根据目标类型创建目标
if(TARGET_TYPE STREQUAL "executable")
    add_executable(${TARGET_NAME} ${SOURCES})
    message(STATUS "创建可执行目标: ${TARGET_NAME}")

    # 可执行文件特定设置
    set_target_properties(${TARGET_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

elseif(TARGET_TYPE STREQUAL "static_lib")
    add_library(${TARGET_NAME} STATIC ${SOURCES})
    message(STATUS "创建静态库目标: ${TARGET_NAME}")

    # 静态库特定设置
    set_target_properties(${TARGET_NAME} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
        POSITION_INDEPENDENT_CODE ON  # 允许链接到可执行文件
    )

elseif(TARGET_TYPE STREQUAL "shared_lib")
    add_library(${TARGET_NAME} SHARED ${SOURCES})
    message(STATUS "创建动态库目标: ${TARGET_NAME}")

    # 动态库特定设置
    set_target_properties(${TARGET_NAME} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
        VERSION 1.0.0
        SOVERSION 1
    )
endif()

# 系统依赖库 - 交易系统必需
target_link_libraries(${TARGET_NAME}
    pthread      # 线程支持
    rt           # 实时扩展
    dl           # 动态加载
    atomic       # 原子操作（某些平台需要）
)

# 可选第三方库
find_package(Threads REQUIRED)
target_link_libraries(${TARGET_NAME} Threads::Threads)

# 检查并链接Boost库（如果可用）
find_package(Boost 1.70.0 COMPONENTS system thread REQUIRED)
if(Boost_FOUND)
    target_link_libraries(${TARGET_NAME} Boost::system Boost::thread)
    message(STATUS "找到并链接Boost库")
else()
    message(WARNING "未找到Boost库，某些功能可能受限")
endif()

# 安装规则
install(TARGETS ${TARGET_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
    ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
)

# 安装头文件
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_PREFIX}/include
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

# 测试配置（可选）
option(BUILD_TESTS "构建测试" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)

    # 创建测试目录
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        add_subdirectory(tests)
    else()
        message(STATUS "未找到tests目录，跳过测试配置")
    endif()
endif()

# 性能分析支持
option(ENABLE_PROFILING "启用性能分析" OFF)
if(ENABLE_PROFILING)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-pg)
        add_link_options(-pg)
        message(STATUS "启用gprof性能分析支持")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-fprofile-instr-generate)
        add_link_options(-fprofile-instr-generate)
        message(STATUS "启用Clang性能分析支持")
    endif()
endif()

# 代码覆盖率（仅调试模式）
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(ENABLE_COVERAGE "启用代码覆盖率" OFF)
    if(ENABLE_COVERAGE)
        add_compile_options(--coverage)
        add_link_options(--coverage)
        message(STATUS "启用代码覆盖率分析")
    endif()
endif()

# 输出总结信息
message(STATUS "=== 构建配置总结 ===")
message(STATUS "项目名称: ${PROJECT_NAME}")
message(STATUS "目标类型: ${TARGET_TYPE}")
message(STATUS "目标名称: ${TARGET_NAME}")
message(STATUS "C++标准: ${CMAKE_CXX_STANDARD}")
message(STATUS "构建类型: ${CMAKE_BUILD_TYPE}")
message(STATUS "编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "源文件总数: ${#SOURCES}")
message(STATUS "安装前缀: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "=========================")
```

## 使用说明

### 基本使用
1. **生成构建配置**：
   ```bash
   /cpp-build-system executable account_service
   ```

2. **创建构建目录**：
   ```bash
   mkdir -p build && cd build
   ```

3. **配置项目**：
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   ```

4. **编译项目**：
   ```bash
   make -j$(nproc)
   ```

### 高级选项

**启用测试**：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
```

**启用性能分析**：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_PROFILING=ON
```

**启用代码覆盖率**：
```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
```

**自定义安装路径**：
```bash
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/account_services
```

## 针对账户服务的特定优化

### 1. 缓存行对齐
- 使用`-malign-data=cacheline`确保数据结构按64字节对齐
- 符合项目中`kCacheLineSize = 64`的定义

### 2. 低延时优化
- `-O3`和`-march=native`最大化性能
- `-ftree-vectorize`启用自动向量化
- `-funroll-loops`循环展开优化

### 3. 共享内存优化
- `-fno-strict-aliasing`对内存映射I/O更友好
- 链接`rt`库支持共享内存操作

### 4. 线程安全
- 链接`pthread`库支持多线程
- 使用原子操作确保线程安全

## 故障排除

### 问题1：找不到源文件
**症状**：CMake警告"未找到源文件"
**解决**：确保源文件已创建或调整`file(GLOB_RECURSE ...)`模式

### 问题2：Boost库缺失
**症状**：CMake错误"找不到Boost"
**解决**：安装Boost开发包或禁用Boost依赖

### 问题3：编译选项不被支持
**症状**：编译器警告"未知选项"
**解决**：根据实际编译器调整优化选项

### 问题4：链接错误
**症状**：链接时找不到符号
**解决**：检查库路径和依赖关系

## 扩展建议

### 1. 模块化构建
对于大型项目，可以考虑：
- 为每个模块创建独立的CMakeLists.txt
- 使用`add_subdirectory()`集成各模块
- 定义清晰的模块间依赖关系

### 2. 包管理器集成
考虑集成：
- Conan包管理器
- vcpkg包管理器
- 系统包管理器(apt/yum/dnf)

### 3. 交叉编译支持
如果需要部署到不同架构：
- 定义工具链文件
- 设置交叉编译选项
- 处理架构特定优化

## 验证生成的配置

生成CMakeLists.txt后，建议验证：
```bash
!`mkdir -p /tmp/test_build && cd /tmp/test_build && cmake /home/ythe/repositories/account_services 2>&1 | tail -20`
```

## 贡献和反馈

这个skill专为高性能量化交易账户服务项目设计。如果发现问题或有改进建议，请：
1. 检查项目结构是否符合预期
2. 验证编译选项是否适用于你的系统
3. 根据实际需求调整优化级别
4. 添加特定于项目的定制选项