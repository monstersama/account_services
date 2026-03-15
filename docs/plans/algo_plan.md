  # 统一执行会话模型与权威 cancelled_volume 回报改造

  ## Summary

  - 以 ExecutionEngine 作为所有已落地拆单算法的统一运行时入口，覆盖 FixedSize、Iceberg、TWAP；VWAP 继续显式 unsupported，但在新框架里预留会话接口。
  - 增加权威 cancelled_volume，并沿 broker_api::broker_event -> gateway::response_mapper -> TradeResponse 全链路透传；相关 ABI/协议版本同步提升。
  - 舍弃旧的“一次性 order_splitter + order_router::handle_split_order()”主路径。order_router 收敛为下游发送、内部子单提交、撤单路由组件；算法推进和父单状
    态收敛全部迁入 ExecutionEngine。
  - 父单在 OrderBook 中改为“执行任务镜像”，与执行侧统一；外部 order_monitor ABI允许变化，因此新增最小必要执行态字段，而不是把语义硬塞进旧字段里。

  ## Key Changes

  - 回报协议与网关链路
      - 给内部 TradeResponse 增加 cancelled_volume，语义固定为“终态剩余撤销量”，只在撤单成功/完成类终态回报上填写；自然完成或纯成交类事件填 0。
      - 给 broker_api::broker_event 同步增加 cancelled_volume，并把 kBrokerApiAbiVersion 提升一版；更新 docs/broker_api_contract.md。
      - 更新 gateway 的 response_mapper.cpp 和模拟适配器，使取消完成事件能带权威 cancelled_volume。模拟适配器需要维护原始在途订单的 entrust / cum_traded /
        remaining，以便在 cancel finish 时给出准确剩余撤销量。
      - 执行侧以后不再通过 grace window 释放预算；收到权威终态 cancelled_volume 后立即结算该子单。
  - 统一执行会话框架
      - 将当前 ExecutionEngine 从“只管理 TWAP 的单一 TwapSession”升级为“统一执行会话容器”：
          - ExecutionSession 抽象基类
          - FixedSizeSession
          - IcebergSession
          - TwapSession
          - ExecutionSessionFactory 按 PassiveExecutionAlgo 创建具体会话
      - 所有算法共享一套子单执行账本，不再只维护 active_child_ids。每条子单记录至少包含：
          - child_order_id
          - entrust_volume
          - confirmed_traded_volume
          - confirmed_traded_value
          - confirmed_fee
          - final_cancelled_volume
          - finalized
          - cancel_requested
          - order_state
      - 父单预算统一用派生公式，不做“把撤单量直接回填某个旧字段”的事件式维护：
          - remaining_target = target_volume - confirmed_traded_volume
          - working_volume = sum(unfinalized child unresolved qty)
          - schedulable_volume = max(0, remaining_target - working_volume)
      - 收到带权威 cancelled_volume 的终态回报后，相关子单立即 finalized，其未成交剩余不再计入 working_volume，父单 schedulable_volume 立即释放。
  - 算法推进语义
      - 主动策略前置覆盖层扩展到所有执行算法，但“决策时点”由算法自行定义：
          - TWAP：每个时间片到达时先问主动策略，若无决策再执行被动片额。
          - FixedSize：每次具备释放下一笔 clip 条件时先问主动策略，若无决策再按固定 clip 发单。
          - Iceberg：每次展示量窗口可更新时先问主动策略，若无决策再按 Iceberg 默认 display-size 推进；首版实现语义允许与 FixedSize 共用推进器，只要会话类
            型和状态命名独立。
      - VWAP 本次不实现，但统一走新工厂入口并返回确定性的“不支持”拒绝，不再通过旧 order_splitter 路径兜底。
      - 父单取消语义统一：
          - 一旦父单收到取消请求，会话停止生成未来子单。
          - 对活动子单发撤单。
          - 子单终态结算后，不再继续推进后续算法步骤。
      - 若在权威终态后又收到会破坏 entrust = traded + cancelled 的晚到回报，执行侧按协议异常处理：记录错误、拒绝再次释放预算，不让父单目标量膨胀。
  - OrderBook / Monitor 语义重构
      - 对“受管父单”禁用当前 refresh_parent_from_children_nolock() 的旧聚合规则，不再用子单 volume_remain 简单求和刷新父单。
      - 给 OrderBook 增加显式同步接口，例如 sync_managed_parent_view(...)，由 ExecutionEngine 在每轮 tick() 和关键回报结算后写回父单镜像。
      - 受管父单的旧字段语义固定为：
          - volume_entrust = target_volume
          - volume_traded = confirmed_traded_volume
          - volume_remain = remaining_target
          - dvalue_traded / dprice_traded / dfee_executed = 已确认成交累计
      - 扩展 order_monitor ABI，新增最小必要执行态字段：
          - execution_algo
          - execution_state
          - target_volume
          - working_volume
          - schedulable_volume
            普通订单这些字段填默认值；受管父单填真实执行态。
      - 保留现有下单 ABI；passive_execution_algo 和 _ex 下单接口继续作为选择执行算法的入口，不在本次重构里再改一次外部下单协议。
  - 旧路径移除
      - order_splitter 退出运行时主路径；不再由 order_router 调 split() 生成子单。
      - order_router::handle_split_order() 与相关一次性拆单状态推进删除。
      - order_router 只保留：
          - 普通非执行会话订单直发
          - 内部子单提交
          - 撤单路由
          - 下游发送
      - 旧的拆单单测迁移为执行会话测试；order_splitter 文件可在本次重构末尾删除，或者仅保留为兼容壳但不再编进运行时目标。推荐直接删除，避免双模型并存。

  ## Public APIs / Interfaces

  - include/broker_api/broker_api.hpp
      - broker_event 增加 cancelled_volume
      - ABI 版本升级
  - src/shm/shm_layout.hpp
      - TradeResponse 增加 cancelled_volume
  - include/api/order_monitor_api.h
      - 新增 execution_algo
      - 新增 execution_state
      - 新增 target_volume
      - 新增 working_volume
      - 新增 schedulable_volume
  - 外部下单 API include/api/order_api.h
      - 本次不再新增字段；继续复用现有逐单 passive_execution_algo
  - 配置
      - 继续沿用现有 split 配置作为已落地算法的默认参数源：
          - strategy 作为 Default 订单的默认算法
          - max_child_volume / min_child_volume / max_child_count / interval_ms 供新会话实现消费
      - 不在本次重构中额外引入新的配置 schema，避免同时做两次迁移

  - 回报链路
  - 执行引擎
      - FixedSize 会话：按 clip 推进，子单终态后释放预算并发下一笔
      - Iceberg 会话：展示量滚动推进，子单终态后刷新下一笔
      - TWAP 会话：时间片到达后先主动、后被动
      - 父单取消：停止未来推进，只处理活动子单撤单与最终结算
      - 权威 cancelled_volume 到达后立即释放预算，不再依赖延迟排空窗口
      - 权威终态后收到冲突性晚到成交：记录异常且不重复释放预算
  - OrderBook / 监控
      - 受管父单不再被旧的子单 volume_remain 聚合逻辑覆盖
      - order_monitor 新字段对受管父单输出正确，对普通订单保持零值/默认值
      - 旧字段在受管父单上的新语义与执行引擎内部量一致
  - 回归
      - 普通非会话订单直发链路不回归
      - 现有 test_event_loop、test_gateway_mapper、test_gateway_loop、test_account_service 更新后通过
      - 原 order_splitter 相关测试替换为执行会话测试，不再保留旧一次性拆单断言

  ## Assumptions

  - cancelled_volume 的权威语义固定为“终态剩余撤销量”，不是增量值，也不是适配器自定义口径。
  - 本次重构覆盖当前已落地算法：FixedSize、Iceberg、TWAP；VWAP 先保留工厂入口并显式拒绝。
  - 主动策略前置覆盖层对所有执行算法开放，但各算法自行定义“何时评估一次”。
  - 允许修改 order_monitor ABI；监控文档与 SDK 文档需要同步更新。
  - 目标是删除旧一次性拆单运行时逻辑，而不是继续保留双路径兼容。