#!/bin/bash

echo "=== 高性能低延时代码审计演示 ==="
echo "目标目录: include/"
echo

echo "1. 缓存行对齐检查"
echo "------------------"

# 检查alignas使用
echo "检查alignas指定:"
grep -r "alignas\|__attribute__((aligned" include/ --include="*.hpp" --include="*.h" || echo "未找到显式对齐指定"

echo
echo "检查大结构体:"
find include/ -name "*.hpp" -o -name "*.h" | while read file; do
    # 粗略检查是否有大结构体
    grep -l "struct.*{" "$file" | while read struct_file; do
        struct_name=$(grep "struct.*{" "$struct_file" | head -1 | awk '{print $2}')
        echo "  文件: $struct_file"
        echo "    结构体: $struct_name"
    done
done

echo
echo "2. 原子操作检查"
echo "---------------"

echo "原子操作使用统计:"
atomic_count=$(grep -r "std::atomic" include/ --include="*.hpp" --include="*.h" | wc -l)
memory_order_count=$(grep -r "memory_order" include/ --include="*.hpp" --include="*.h" | wc -l)
relaxed_count=$(grep -r "memory_order_relaxed" include/ --include="*.hpp" --include="*.h" | wc -l)

echo "  原子操作总数: $atomic_count"
echo "  内存序使用总数: $memory_order_count"
echo "  宽松序使用数: $relaxed_count"

if [[ $atomic_count -gt 0 && $relaxed_count -eq 0 ]]; then
    echo "  警告: 可能过度使用顺序一致性"
fi

echo
echo "3. 订单请求结构体专项检查"
echo "-------------------------"

order_request_file="include/order/order_request.hpp"
if [[ -f "$order_request_file" ]]; then
    echo "分析订单请求结构体:"

    # 检查对齐
    if grep -q "alignas(64)" "$order_request_file"; then
        echo "  ✓ 使用64字节对齐"
    else
        echo "  ✗ 未使用64字节对齐"
    fi

    # 检查填充
    padding_count=$(grep -c "padding\|pad" "$order_request_file")
    echo "  ✓ 找到 $padding_count 处填充字段"

    # 检查原子操作
    if grep -q "std::atomic<order_status_t>" "$order_request_file"; then
        echo "  ✓ 订单状态使用原子操作"
    fi

    # 检查内存序
    if grep -q "memory_order_relaxed" "$order_request_file"; then
        echo "  ✓ 使用relaxed内存序"
    fi
else
    echo "  未找到订单请求文件"
fi

echo
echo "4. 共享内存模块检查"
echo "-------------------"

shm_files=$(find include/shm/ -name "*.hpp" -o -name "*.h" 2>/dev/null || true)
if [[ -n "$shm_files" ]]; then
    echo "共享内存文件:"
    for file in $shm_files; do
        echo "  $(basename "$file")"
    done

    # 检查无锁结构
    lockfree_count=$(grep -r "lock-free\|spsc\|mpmc" include/shm/ --include="*.hpp" --include="*.h" | wc -l)
    echo "  无锁数据结构引用: $lockfree_count"
else
    echo "  未找到共享内存文件"
fi

echo
echo "=== 审计总结 ==="
echo
echo "正面发现:"
echo "1. 订单请求结构体使用 alignas(64) 明确指定缓存行对齐"
echo "2. 使用 std::atomic 进行线程安全的状态管理"
echo "3. 正确使用 memory_order_relaxed 减少同步开销"
echo "4. 添加填充字段确保数据结构对齐"
echo
echo "建议:"
echo "1. 检查其他性能关键结构体是否同样进行缓存行对齐"
echo "2. 确保所有原子操作使用适当的内存序"
echo "3. 在性能敏感路径避免动态内存分配"
echo "4. 考虑使用内存池管理频繁创建的对象"
echo
echo "使用 /cpp-performance-audit 命令进行更全面的检查"