 # 主动策略覆盖 TWAP 的执行框架

  ## Summary

  - 保留现有“用户订单 -> 被动拆单/直发”的主路径，但把它拆成两类：
      - 非时间片算法继续走现有一次性路径。
      - TWAP 迁移为长期执行会话，不再由当前 order_splitter.hpp 的一次性 split() 实现。
  - 新增一个可选的“主动策略覆盖层”，只对 passive algo = TWAP 的订单生效。
  - 每个 TWAP 时间片按以下固定顺序执行：
      1. TWAP 会话打开当前时间片预算。
      2. 在该时间片窗口内，主动策略可基于行情/预测随时直接给出子单。
      3. 若到该片应发时点仍未给出子单，则回落到被动 TWAP，按该片默认规则发单。
      4. 主动策略给出的子单只允许消耗“当前片额”，不得透支未来片额。
  - 主动策略按服务级配置选择一个内建实现；用户逐单选择的是被动算法名。首版只保证 TWAP 被动算法可由用户逐单选择并接入该覆盖框架。

  ## Key Changes

  - 新增 src/market_data/
      - 提供 MarketDataService 和 snapshot_reader 适配层，统一按 internal_security_id 读取最新行情。
      - 定义项目内稳定视图 MarketSnapshotView 与 PredictionView。
      - 当前 vendored snapshot_reader 还没有 prediction 字段，先在内部类型中预留 prediction 位置，适配层当前返回“无 prediction”；后续三方协议扩展时只改适
        配层。
  - 新增 src/strategy/
      - 只放“主动策略”接口与实现，不混入被动拆单逻辑。
      - 提供 ActiveStrategy、ActiveDecision、StrategyRegistry。
      - ActiveDecision 语义固定为“直接给子单或不动作”，不做“仅放行/仅修参”模式。
      - 首版注册表按名字创建内建策略实现，不做动态插件 ABI。
      - 第一类策略实现定位为“基于 prediction 的 TWAP 覆盖策略”，具体预测阈值和下单规则封装在该实现内，不泄漏到框架层。
  - 新增 src/execution/
      - 提供长期执行会话层，承载时间片型被动算法与主动覆盖逻辑。
      - 核心对象包括：
          - ExecutionSession
          - TwapSession
          - ExecutionEngine
      - TwapSession 负责切片计划、当前片额、时间片状态、父单剩余量、活动子单、完成/失败/撤销状态。
      - ExecutionEngine 在每轮 EventLoop 中推进所有活跃 TwapSession：
          - 若当前片窗口未结束，轮询主动策略是否给出子单。
          - 若主动策略已在本片发单，则本片关闭，不再走被动回落。
          - 若到片末仍无主动子单，则按被动 TWAP 默认规则发该片。
      - 非 TWAP 算法不进入 ExecutionEngine，仍走现有路由/一次性拆单链。
  - 调整 src/order/
      - order_splitter.cpp 只保留一次性算法，如 FixedSize / Iceberg。
      - TWAP 从一次性 split() 里移出，改由 src/execution/ 长期会话实现。
      - order_router 提炼一条“内部子单提交”路径，供 ExecutionEngine 复用，避免主动子单重新走上游 SHM 和用户 API。
      - 主动子单与被动 TWAP 回落子单都走同一条内部子单提交路径，并显式标记为“内部生成子单”，避免再次进入被动拆单。
  - 调整 src/core/
      - AccountService 初始化 MarketDataService、StrategyRegistry、ExecutionEngine。
      - EventLoop 处理新父单时改为分流：
          - 若订单被动算法不是 TWAP，继续走现有主路径。
          - 若订单被动算法是 TWAP，先通过基础校验/风控，再创建 TwapSession，不直接调用当前一次性拆单逻辑。
      - 每轮 EventLoop 增加 execution_engine.tick()，让 TWAP 会话持续推进。
      - 回报处理需要通知 ExecutionEngine，让会话能依据子单成交/撤单状态更新下一片预算与完成判定。
  - 构建接入
      - 顶层 CMake 正式接入 third_party/snapshot_reader，并显式传入 BASECORE_SRC_DIR=${CMAKE_SOURCE_DIR}/BaseCore/src。
      - 新增内部库目标：
          - acct_market_data
          - acct_strategy
          - acct_execution
      - acct_core_service / acct_core_loop 依赖上述新库。

  ## Public APIs / Interfaces

  - 扩展 OrderRequest、orders_shm、监控快照
      - 新增逐单被动算法字段，例如 passive_exec_algo，首版支持值至少包括 None、FixedSize、Iceberg、TWAP、VWAP。
      - 首版 VWAP 可以在协议中保留，但服务侧必须明确返回“不支持”，不能静默按其它算法处理。
      - 预留逐单执行参数扩展位，但首版不消费这些参数。
  - 扩展下单 API
      - 保留现有 acct_new_order / acct_submit_order 兼容旧调用方。
      - 新增 _ex 接口与 acct_order_exec_options_t，允许用户逐单选择被动算法名。
      - 首版单笔订单只传“算法名”，不传单笔 TWAP 参数；TWAP 的 interval_ms、每片默认量等均来自服务配置。
  - 新配置模型
      - 新增 market_data 段：snapshot shm 名称、兼容性检查、future prediction 开关。
      - 新增 active_strategy 段：enabled、name、通用轮询参数、策略私有参数。
      - 新增 passive_execution 段：按算法组织默认参数，至少包含 twap 子段。
      - 保留旧 split 段做兼容读取：
          - 旧配置自动映射到 passive_execution 默认值。
          - 旧客户端未传逐单算法时，仍按旧默认行为运行。
  - 主动策略作用面
      - 主动策略是服务级可选项，不是用户逐单选择项。
      - 首版仅对 passive_exec_algo = TWAP 的父单生效。
      - 对非 TWAP 订单完全不参与，也不改变现有 FixedSize / Iceberg 主路径。

  ## Test Plan

  - 配置测试
      - 新 market_data / active_strategy / passive_execution 配置解析。
      - 旧 split 配置向新模型的兼容映射。
  - API / SHM 测试
      - _ex 接口能把逐单被动算法写入 OrderRequest / orders_shm。
      - 旧 API 无逐单算法时能正确回落。
      - 监控快照能读到逐单被动算法与“内部生成子单”标记。
  - 行情模块测试
  - 被动路径回归
      - TWAP 不再经过旧 order_splitter 的一次性实现。
  - TWAP 会话测试
      - 会话按时间片推进。
      - 当前片预算正确计算与扣减。
      - 子单成交/撤单后会话状态正确推进。
      - 父单撤销能终止会话并清理活动子单。
  - 主动覆盖测试
      - 在某个时间片内，主动策略先于被动回落给出子单，则该片不再执行被动 TWAP。
      - 主动策略未给子单，到该片应发时点触发被动 TWAP 回落。
      - 主动策略不能超过当前片额。
      - 主动策略只对 TWAP 订单生效。
  - 回归与端到端
      - TWAP + active disabled
      - TWAP + active enabled but no decision
      - TWAP + active emits child
      - FixedSize/Iceberg + active enabled
      - 回报驱动下多时间片连续推进

  ## Assumptions / Defaults

  - 当前讨论聚焦框架与目录边界；prediction 的具体业务字段会在未来 snapshot_reader 扩展后接入。
  - 主动策略的业务规则由具体策略实现决定，框架层只规定输入/输出与预算边界，不把预测阈值硬编码进公共接口。
  - 首版用户逐单只选择被动算法名；算法参数继续由服务配置提供默认值。
  - 首版只把 TWAP 升级到长期会话模型；其余算法继续保留当前一次性执行方式。
  - 主动策略不是“整单接管后永不回落”，而是“每个 TWAP 时间片上的前置覆盖层”；该片无主动子单时才回落到被动 TWAP。