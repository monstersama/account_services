---
name: cpp-performance-audit
description: 检查C++代码是否符合高性能低延时量化交易系统的最佳实践。分析缓存行对齐、内存访问模式、原子操作、无锁数据结构等关键性能指标。
argument-hint: [file-or-directory] [check-type]
disable-model-invocation: false
user-invocable: true
allowed-tools: Read, Write, Glob, Grep, Bash
---

# C++高性能低延时代码审计

专门为量化交易系统设计的代码性能审计工具。检查代码是否符合高性能低延时的最佳实践，并提供具体的优化建议。

## 参数说明
- `$0` (file-or-directory): 要检查的文件或目录路径，默认为当前目录
- `$1` (check-type): 检查类型 - `all`(全面检查), `cache`(缓存相关), `memory`(内存管理), `atomic`(原子操作), `branch`(分支预测)

## 功能特性
1. **缓存行对齐检查**：验证数据结构是否按64字节对齐
2. **内存访问模式分析**：检查连续内存访问和预取友好性
3. **原子操作审计**：确保正确的内存序和原子操作使用
4. **无锁数据结构检查**：识别可能的锁竞争和替代方案
5. **动态内存分配检测**：查找不必要的堆分配
6. **分支预测优化**：分析分支友好性和likely/unlikely提示
7. **系统调用分析**：识别性能敏感区域的系统调用
8. **虚函数开销检查**：查找可能的多态性能开销

## 使用示例
- `/cpp-performance-audit . all` - 全面检查当前目录
- `/cpp-performance-audit include/order cache` - 检查订单模块的缓存优化
- `/cpp-performance-audit src/core/ memory` - 检查核心模块的内存管理

## 性能检查规则

### 1. 缓存行对齐规则
```cpp
// 良好：显式指定对齐
struct alignas(64) OrderRequest {
    // 192字节，正好3个缓存行
};

// 不良：未指定对齐
struct BadOrderRequest {
    // 可能导致缓存行伪共享
};
```

**检查方法**：
```bash
!`grep -n "struct\|class" $FILE | grep -v "alignas\|__attribute__((aligned" | head -10`
```

### 2. 内存访问模式规则
```cpp
// 良好：连续内存访问
for (size_t i = 0; i < N; ++i) {
    data[i].process();  // 顺序访问
}

// 不良：随机内存访问
for (auto& ptr : pointers) {
    ptr->process();  // 指针追逐，缓存不友好
}
```

**检查方法**：
```bash
!`grep -n "->\|\.\*" $FILE | grep -v "//\|/\*" | head -10`
```

### 3. 原子操作规则
```cpp
// 良好：正确的内存序
std::atomic<int> counter{0};
counter.fetch_add(1, std::memory_order_relaxed);  // 计数器使用relaxed

// 不良：过度同步
std::atomic<int> flag{0};
flag.store(1, std::memory_order_seq_cst);  // 不必要的顺序一致性
```

**检查方法**：
```bash
!`grep -n "std::atomic\|__atomic" $FILE | grep -v "//\|/\*" | head -10`
```

### 4. 动态内存分配规则
```cpp
// 良好：栈分配或内存池
OrderRequest request;  // 栈分配
request_pool.allocate();  // 内存池分配

// 不良：性能敏感区的堆分配
void process_order() {
    auto* data = new OrderData();  // 避免在热路径中new
    // ...
    delete data;
}
```

**检查方法**：
```bash
!`grep -n "new \|malloc\|std::make_shared\|std::make_unique" $FILE | grep -v "//\|/\*" | head -10`
```

### 5. 分支预测规则
```cpp
// 良好：分支提示
if (__builtin_expect(is_hot_path, 1)) {
    fast_path();
}

// 良好：无分支代码
result = (a > b) ? a : b;  // 条件移动，非分支

// 不良：深层嵌套分支
if (condition1) {
    if (condition2) {
        if (condition3) {  // 分支预测困难
            // ...
        }
    }
}
```

**检查方法**：
```bash
!`grep -n "if \|switch \|case " $FILE | grep -v "//\|/\*" | wc -l`
```

## 审计脚本实现

### 全面性能审计
```bash
#!/bin/bash
# performance_audit.sh

TARGET=${1:-.}
CHECK_TYPE=${2:-all}

echo "=== 高性能低延时代码审计 ==="
echo "目标: $TARGET"
echo "检查类型: $CHECK_TYPE"
echo

if [[ "$CHECK_TYPE" == "all" || "$CHECK_TYPE" == "cache" ]]; then
    echo "1. 缓存行对齐检查"
    echo "------------------"

    # 查找结构体和类定义
    find "$TARGET" -name "*.hpp" -o -name "*.h" -o -name "*.cpp" | while read file; do
        echo "检查文件: $file"

        # 检查alignas使用
        alignas_count=$(grep -c "alignas\|__attribute__((aligned" "$file")
        if [[ $alignas_count -eq 0 ]]; then
            echo "  警告: 未找到显式对齐指定"

            # 检查可能的大数据结构
            grep -n "struct\|class" "$file" | while read line; do
                struct_name=$(echo "$line" | awk '{print $2}')
                echo "    潜在问题: $struct_name 未指定对齐"
            done
        else
            echo "  良好: 找到 $alignas_count 处对齐指定"
        fi
    done
    echo
fi

if [[ "$CHECK_TYPE" == "all" || "$CHECK_TYPE" == "memory" ]]; then
    echo "2. 内存管理检查"
    echo "---------------"

    # 检查动态内存分配
    find "$TARGET" -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" | while read file; do
        echo "检查文件: $file"

        new_count=$(grep -c "new " "$file")
        malloc_count=$(grep -c "malloc\|calloc\|realloc" "$file")

        if [[ $new_count -gt 0 || $malloc_count -gt 0 ]]; then
            echo "  警告: 找到动态内存分配"
            echo "    new操作: $new_count"
            echo "    malloc操作: $malloc_count"

            # 显示具体位置
            grep -n "new \|malloc\|calloc\|realloc" "$file" | head -5 | while read alloc_line; do
                echo "    位置: $alloc_line"
            done
        else
            echo "  良好: 未找到动态内存分配"
        fi
    done
    echo
fi

if [[ "$CHECK_TYPE" == "all" || "$CHECK_TYPE" == "atomic" ]]; then
    echo "3. 原子操作检查"
    echo "---------------"

    find "$TARGET" -name "*.hpp" -o -name "*.h" -o -name "*.cpp" | while read file; do
        echo "检查文件: $file"

        atomic_count=$(grep -c "std::atomic\|__atomic" "$file")
        if [[ $atomic_count -gt 0 ]]; then
            echo "  找到 $atomic_count 处原子操作"

            # 检查内存序
            memory_order_count=$(grep -c "memory_order" "$file")
            seq_cst_count=$(grep -c "memory_order_seq_cst" "$file")
            relaxed_count=$(grep -c "memory_order_relaxed" "$file")

            echo "    内存序使用统计:"
            echo "      总计: $memory_order_count"
            echo "      顺序一致性: $seq_cst_count"
            echo "      宽松序: $relaxed_count"

            if [[ $seq_cst_count -gt 0 && $relaxed_count -eq 0 ]]; then
                echo "  警告: 可能过度使用顺序一致性"
            fi
        else
            echo "  注意: 未找到原子操作，检查是否需要线程同步"
        fi
    done
    echo
fi

if [[ "$CHECK_TYPE" == "all" || "$CHECK_TYPE" == "branch" ]]; then
    echo "4. 分支预测检查"
    echo "---------------"

    find "$TARGET" -name "*.cpp" -o -name "*.cc" -o -name "*.cxx" | while read file; do
        echo "检查文件: $file"

        # 统计分支数量
        if_count=$(grep -c "if (" "$file")
        switch_count=$(grep -c "switch (" "$file")
        nested_depth=0

        echo "  分支统计:"
        echo "    if语句: $if_count"
        echo "    switch语句: $switch_count"

        # 检查深层嵌套
        max_depth=0
        current_depth=0

        while IFS= read -r line; do
            if [[ "$line" =~ "if (" || "$line" =~ "for (" || "$line" =~ "while (" || "$line" =~ "switch (" ]]; then
                ((current_depth++))
                if [[ $current_depth -gt $max_depth ]]; then
                    max_depth=$current_depth
                fi
            fi

            if [[ "$line" =~ "}" && ! "$line" =~ "} else" ]]; then
                if [[ $current_depth -gt 0 ]]; then
                    ((current_depth--))
                fi
            fi
        done < "$file"

        echo "    最大嵌套深度: $max_depth"

        if [[ $max_depth -gt 3 ]]; then
            echo "  警告: 发现深层嵌套分支，可能影响分支预测"
        fi

        # 检查分支提示
        builtin_expect_count=$(grep -c "__builtin_expect\|likely\|unlikely" "$file")
        if [[ $builtin_expect_count -eq 0 && $if_count -gt 10 ]]; then
            echo "  建议: 考虑在热点路径添加分支提示"
        fi
    done
    echo
fi

echo "=== 审计完成 ==="
echo
echo "优化建议摘要:"
echo "1. 确保关键数据结构按64字节缓存行对齐"
echo "2. 在性能敏感区域避免动态内存分配"
echo "3. 根据需要使用适当的内存序（优先使用relaxed）"
echo "4. 减少深层分支嵌套，添加分支提示"
echo "5. 优化内存访问模式，提高缓存命中率"
```

### 专用检查函数

#### 缓存对齐检查
```bash
check_cache_alignment() {
    local file=$1
    echo "检查缓存对齐: $file"

    # 查找大于64字节的结构体
    awk '
    BEGIN { in_struct=0; struct_name=""; byte_size=0; }
    /^struct |^class / {
        if (in_struct && byte_size > 64) {
            print "警告: " struct_name " 大小 " byte_size " 字节，应考虑64字节对齐"
        }
        in_struct=1;
        struct_name=$2;
        byte_size=0;
    }
    /^};/ {
        if (in_struct && byte_size > 64) {
            print "警告: " struct_name " 大小约 " byte_size " 字节，应考虑64字节对齐"
        }
        in_struct=0;
    }
    in_struct && !/^\s*\/\// {
        # 粗略估计大小
        if (/\w+\s+\w+;/) byte_size += 8;
        if (/\[\d+\]/) {
            match($0, /\[([0-9]+)\]/);
            byte_size += substr($0, RSTART+1, RLENGTH-2) * 8;
        }
    }
    ' "$file"
}
```

#### 虚函数开销检查
```bash
check_virtual_overhead() {
    local file=$1
    echo "检查虚函数开销: $file"

    virtual_count=$(grep -c "virtual " "$file")
    if [[ $virtual_count -gt 0 ]]; then
        echo "  找到 $virtual_count 个虚函数"

        # 检查是否在性能关键类中
        grep -B5 -A5 "virtual " "$file" | grep -n "class\|struct" | while read line; do
            echo "    可能影响性能: $line"
        done

        if [[ $virtual_count -gt 5 ]]; then
            echo "  警告: 虚函数数量较多，考虑使用CRTP或其他静态多态技术"
        fi
    else
        echo "  良好: 未找到虚函数"
    fi
}
```

#### 系统调用检查
```bash
check_syscalls() {
    local file=$1
    echo "检查系统调用: $file"

    # 常见的性能敏感系统调用
    sensitive_calls=("gettimeofday\|clock_gettime\|time"
                    "malloc\|free\|new\|delete"
                    "pthread_mutex\|std::mutex"
                    "printf\|cout\|fprintf"
                    "dynamic_cast\|typeid")

    for pattern in "${sensitive_calls[@]}"; do
        count=$(grep -c "$pattern" "$file")
        if [[ $count -gt 0 ]]; then
            echo "  找到 $count 处 $pattern 调用"

            # 显示具体位置
            grep -n "$pattern" "$file" | head -3 | while read call_line; do
                echo "    位置: $call_line"
            done
        fi
    done
}
```

## 针对交易系统的特定检查

### 订单处理性能检查
```bash
check_order_performance() {
    local dir=$1
    echo "=== 订单处理模块性能检查 ==="

    # 检查订单数据结构
    order_files=$(find "$dir" -name "*order*.hpp" -o -name "*order*.h")

    for file in $order_files; do
        echo "检查文件: $file"

        # 检查订单结构大小
        grep -A20 "struct.*[Oo]rder\|class.*[Oo]rder" "$file" | head -30 | grep -E "sizeof|alignas|//.*字节"

        # 检查原子操作
        atomic_in_order=$(grep -c "std::atomic" "$file")
        if [[ $atomic_in_order -gt 0 ]]; then
            echo "  订单结构中使用原子操作: $atomic_in_order 处"
        fi

        # 检查内存池使用
        has_memory_pool=$(grep -c "pool\|allocator\|arena" "$file")
        if [[ $has_memory_pool -eq 0 ]]; then
            echo "  建议: 考虑为订单对象使用内存池"
        fi
    done
}
```

### 共享内存性能检查
```bash
check_shm_performance() {
    local dir=$1
    echo "=== 共享内存模块性能检查 ==="

    shm_files=$(find "$dir" -name "*shm*.hpp" -o -name "*shm*.h")

    for file in $shm_files; do
        echo "检查文件: $file"

        # 检查无锁队列
        has_lockfree=$(grep -c "lock-free\|spsc\|mpmc\|atomic.*queue" "$file")
        if [[ $has_lockfree -eq 0 ]]; then
            echo "  建议: 共享内存通信考虑使用无锁队列"
        fi

        # 检查内存屏障
        has_barrier=$(grep -c "std::atomic_thread_fence\|__sync_synchronize\|asm volatile" "$file")
        if [[ $has_barrier -gt 0 ]]; then
            echo "  良好: 找到内存屏障使用"
        fi

        # 检查缓存行填充
        has_padding=$(grep -c "char padding\|__attribute__((aligned" "$file")
        if [[ $has_padding -eq 0 ]]; then
            echo "  建议: 考虑添加缓存行填充防止伪共享"
        fi
    done
}
```

## 审计报告生成

### 生成HTML报告
```bash
generate_html_report() {
    local target=$1
    local report_file="performance_audit_$(date +%Y%m%d_%H%M%S).html"

    cat > "$report_file" << EOF
<!DOCTYPE html>
<html>
<head>
    <title>高性能代码审计报告</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        h1 { color: #333; }
        .section { margin: 20px 0; padding: 15px; border-left: 4px solid #007acc; }
        .warning { background-color: #fff3cd; border-color: #ffc107; }
        .good { background-color: #d4edda; border-color: #28a745; }
        .metric { display: inline-block; margin: 5px 15px; padding: 5px 10px; background: #f8f9fa; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }
        th { background-color: #f2f2f2; }
    </style>
</head>
<body>
    <h1>高性能低延时代码审计报告</h1>
    <p>目标: $target</p>
    <p>生成时间: $(date)</p>

    <div class="section">
        <h2>检查摘要</h2>
        <div class="metric">文件数: $(find "$target" -name "*.cpp" -o -name "*.hpp" | wc -l)</div>
        <div class="metric">结构体/类数: $(grep -r "struct\|class" "$target" --include="*.hpp" --include="*.h" | wc -l)</div>
    </div>

    <div class="section">
        <h2>缓存优化</h2>
        <table>
            <tr><th>检查项</th><th>状态</th><th>发现</th></tr>
            <tr><td>缓存行对齐</td><td>待检查</td><td>-</td></tr>
            <tr><td>数据结构大小</td><td>待检查</td><td>-</td></tr>
        </table>
    </div>

    <div class="section">
        <h2>内存管理</h2>
        <table>
            <tr><th>检查项</th><th>状态</th><th>发现</th></tr>
            <tr><td>动态内存分配</td><td>待检查</td><td>-</td></tr>
            <tr><td>内存池使用</td><td>待检查</td><td>-</td></tr>
        </table>
    </div>

    <div class="section">
        <h2>并发优化</h2>
        <table>
            <tr><th>检查项</th><th>状态</th><th>发现</th></tr>
            <tr><td>原子操作</td><td>待检查</td><td>-</td></tr>
            <tr><td>锁竞争</td><td>待检查</td><td>-</td></tr>
        </table>
    </div>

    <div class="section warning">
        <h2>优化建议</h2>
        <ul>
            <li>检查关键数据结构是否按64字节对齐</li>
            <li>在性能敏感路径避免动态内存分配</li>
            <li>使用适当的内存序减少同步开销</li>
            <li>优化分支预测和内存访问模式</li>
        </ul>
    </div>
</body>
</html>
EOF

    echo "生成报告: $report_file"
}
```

## 使用指南

### 快速开始
```bash
# 全面检查项目
/cpp-performance-audit . all

# 只检查缓存相关优化
/cpp-performance-audit include/ cache

# 生成详细报告
/cpp-performance-audit src/ memory | tee performance_report.txt
```

### 集成到开发流程
1. **预提交检查**：在git pre-commit钩子中运行基础检查
2. **代码审查**：作为代码审查的一部分运行全面检查
3. **持续集成**：在CI流水线中集成性能审计
4. **定期扫描**：每周或每月运行完整审计

### 性能基准
提供参考性能指标：
- L1缓存命中率：>95%
- 分支预测成功率：>90%
- 原子操作延迟：<50ns
- 内存分配延迟：<100ns（池化）

## 常见问题

### Q1: 如何确定结构体是否需要缓存行对齐？
A: 如果结构体满足以下任一条件，应考虑64字节对齐：
- 大小超过32字节
- 被多个线程频繁访问
- 包含原子变量
- 在共享内存中使用

### Q2: 何时使用memory_order_relaxed？
A: 在以下场景使用relaxed序：
- 统计计数器
- 标志位（非同步点）
- 进度跟踪
- 无依赖的数据更新

### Q3: 如何减少分支预测失误？
A:
1. 使用`__builtin_expect()`或C++20的`[[likely]]`/`[[unlikely]]`
2. 将常见条件放在前面
3. 使用查表法代替switch
4. 使用无分支算法

### Q4: 交易系统的关键性能指标是什么？
A:
- 订单处理延迟：<1微秒
- 99.9%尾延迟：<10微秒
- 吞吐量：>100k订单/秒
- CPU缓存命中率：>98%

## 扩展和定制

### 添加自定义规则
在skill目录中创建`custom_rules.yaml`：
```yaml
rules:
  - name: "cache_line_alignment"
    pattern: "struct.*{"
    check: "是否包含alignas(64)"
    severity: "high"

  - name: "hot_path_allocation"
    pattern: "new.*|malloc.*"
    context: "循环内|高频函数"
    severity: "critical"
```

### 集成外部工具
可以集成：
- **perf**：性能分析
- **valgrind**：内存检查
- **cachegrind**：缓存模拟
- **Intel VTune**：深度性能分析

## 反馈和改进

这个skill专注于量化交易系统的高性能需求。根据实际使用情况，可以：
1. 添加项目特定的性能规则
2. 调整检查阈值
3. 集成实际性能测试
4. 添加架构特定的优化建议

使用`/cpp-performance-audit`命令开始审计你的代码性能！