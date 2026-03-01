# BlueprintExporter 插件 — 第三次迭代需求

## 来源

- 对 AC_EnemyAI 导出结果 v3.0 的逐项测试（第二次迭代全部 6 项 Change 已验证通过）
- 输出体积优化分析（当前 7256 行 / 33 万字符）

---

## 目标

本次迭代聚焦**输出体积压缩**。导出文档的消费者是 AI，不需要考虑人类可读性的排版需求。在不丢失任何已接线逻辑信息的前提下，去除冗余行，预计减少约 **900+ 行（~12-13%）**。

---

## P1：冗余信息清理

### 1. 移除 `← exec` 输入 Pin 行

**现状**：每个节点的 exec 输入 Pin（如 `← execute (exec) -> SourceNode.then`）与源节点的 exec 输出 Pin（如 `→ then (exec) -> ThisNode.execute`）表达的是**同一条边的两端**，信息完全重复。当前共 **415 行**。

**具体示例**：

```
[K2Node_CallFunction_15] CALL
  Function: SetCurrentState
  → then (exec) -> K2Node_IfThenElse_5.execute    ← 源端已声明连接目标

[K2Node_IfThenElse_5] BRANCH
  ← execute (exec) -> K2Node_CallFunction_15.then  ← 冗余：反向重复上面的连接
  ← Condition (bool) = true -> K2Node_VariableGet_10.Can Receive Threat Alert
  → True (exec) -> K2Node_CallArrayFunction_0.execute
```

删除后变为：

```
[K2Node_CallFunction_15] CALL
  Function: SetCurrentState
  → then (exec) -> K2Node_IfThenElse_5.execute

[K2Node_IfThenElse_5] BRANCH
  ← Condition (bool) = true -> K2Node_VariableGet_10.Can Receive Threat Alert
  → True (exec) -> K2Node_CallArrayFunction_0.execute
```

**方案**：在 `GetMeaningfulPins` 中，跳过 `Category == "exec" && Direction == "Input"` 的 Pin。

**注意**：仅移除 exec 类型的输入 Pin。非 exec 的 `←` 输入 Pin 携带参数名、默认值、数据来源等信息，必须保留。

**文件**：`BlueprintTextFormatter.cpp`（`GetMeaningfulPins` 方法）

**预计减少**：~415 行

---

### 2. 移除 REROUTE 节点条目

**现状**：REROUTE（`K2Node_Knot`）节点在节点列表中仅显示一行 `[K2Node_Knot_X] REROUTE`，没有任何 Pin 信息（Pin 过滤规则已将 Reroute 的 Pin 全部跳过）。Execution Flow / Data Flow 总览中的穿透逻辑已经把 Reroute 跳过并直接显示最终目标。这些条目是纯占位符。当前共 **101 行**。

**具体示例**：

```
  ← Amount (double) = 25.000000
[K2Node_Knot_0] REROUTE          ← 无任何信息
[K2Node_Knot_1] REROUTE          ← 无任何信息
[K2Node_CallFunction_0] CALL
  Function: CheckDodgeSide
```

**方案**：在 `FormatGraph`（或节点循环）中，跳过 `NodeClass == "K2Node_Knot"` 的节点，不输出其条目。

**文件**：`BlueprintTextFormatter.cpp`（节点格式化循环）

**预计减少**：~101 行

---

### 3. 移除未连接的非 exec 输出 Pin

**现状**：大量函数返回值、结构体字段等输出 Pin 在蓝图中未被任何节点消费，但仍被输出。这些只表示"此函数/结构有此输出可用"，对理解实际逻辑流无帮助。当前共 **103 行**。

**具体示例**：

```
[K2Node_CallArrayFunction_0] CALL(Array)
  Function: KismetArrayLibrary::Array_Add
  ← Target Array (Array<n>) -> K2Node_VariableGet_14.Tags
  ← New Item (name) -> K2Node_VariableGet_6.Tag Name
  → Return Value (int)                ← 没有节点消费此返回值

[K2Node_AIMoveTo_3] AIMoveTo
  → On Success (exec) -> K2Node_CallFunction_5.execute
  → On Fail (exec)
  → Movement Result (EPathFollowingResult)   ← 没有节点读取此结果

[K2Node_BreakStruct_4] BREAK_STRUCT
  → Target (Actor) -> K2Node_CallFunction_8.Target       ← ✅ 有连接，保留
  → Last Sensed Stimuli (Array<AIStimulus>) -> ...        ← ✅ 有连接，保留
  → Is Hostile (bool)                                     ← 没有节点消费
  → Is Friendly (bool)                                    ← 没有节点消费
```

**方案**：在 `GetMeaningfulPins` 中，对非 exec 类型的 Output Pin，如果 `LinkedTo` 为空且 `DefaultValue` 为空，则跳过。

**注意**：未连接的**命名 exec 输出**（如 Branch 的 `→ False (exec)`、SwitchEnum 的 `→ Attack (exec)` 等，共 99 行）**不在此项移除范围内**。这些标明了存在但未处理的逻辑分支，对 AI 理解分支完整性有价值。

**文件**：`BlueprintTextFormatter.cpp`（`GetMeaningfulPins` 方法）

**预计减少**：~103 行

---

### 4. 精简空行：仅保留 Graph 分隔处

**现状**：当前节点之间、Section 之间有大量空行用于视觉分隔，总计 **674 行**。导出文档的消费者是 AI，不需要空行辅助阅读。

**方案**：

- **保留空行的位置**：`=== Blueprint ===`、`=== Variables ===`、`--- Graph: xxx ---`、`=== Execution Flow ===`、`=== Data Flow ===` 这些 Section 分隔符的前后各保留 1 行空行
- **移除空行的位置**：节点与节点之间的空行全部移除

**文件**：`BlueprintTextFormatter.cpp`（`FormatGraph`、`FormatBlueprint` 等方法中控制换行的逻辑）

**预计减少**：~300+ 行（674 总空行 - Section 分隔保留约 100×2 行）

---

## P2：变量区 trivial 默认值清理

### 5. 变量声明区省略 trivial 默认值

**现状**：变量声明区有 12 行显示了无意义的默认值，包括零向量 `(X=0.000000,Y=0.000000,Z=0.000000)`、空结构体 `()`、空数组 `(())`。

**具体示例**：

```
  Last Seen Location           : Vector                        = (X=0.000000,Y=0.000000,Z=0.000000)
  BlockDispatcher              : mcdelegate                    = ()
  SpecialAttacks               : Array<F_SpecialAttack_Struct> = (())
```

**方案**：在变量默认值格式化时，将以下值视为 trivial 并省略显示：

- `(X=0.000000,Y=0.000000,Z=0.000000)` — 零向量
- `()` — 空结构体 / 空 delegate
- `(())` — 空数组

可复用或扩展现有 Pin 层面的 `IsTrivialDefault` 逻辑。

**文件**：`BlueprintTextFormatter.cpp`（`FormatVariables` 方法）

**预计减少**：12 行变短（不删除行，仅去掉 `= ...` 部分），体积影响很小

---

## 实施优先级总结

| 优先级 | 需求 | 预计减少行数 | 工作量 |
|--------|------|-------------|--------|
| P1 | #1 移除 `← exec` 输入 Pin | ~415 | 小 |
| P1 | #2 移除 REROUTE 节点条目 | ~101 | 小 |
| P1 | #3 移除未连接非 exec 输出 Pin | ~103 | 小 |
| P1 | #4 精简空行 | ~300+ | 小 |
| P2 | #5 变量区 trivial 默认值 | ~0（缩短 12 行） | 小 |

**合计预计**：从 7256 行降至约 **6340 行**，减少约 **12-13%**。

---

## 验证方法

1. 对 AC_EnemyAI 蓝图运行导出，对比 v3.0 与 v4.0 输出。
2. **逻辑完整性检查**：
   - 在 v4.0 中任取一个 CALL 节点，确认其 `→ exec` 输出连接与 v3.0 一致（exec 流未丢失）。
   - 确认无 `[K2Node_Knot_X] REROUTE` 条目残留。
   - 确认 BreakStruct / CallFunction 等节点上未连接的输出 Pin 不再显示，但有连接的输出 Pin 完好。
   - 确认 Branch / SwitchEnum 的未连接命名 exec 输出（如 `→ False (exec)`）仍然保留。
3. **空行检查**：节点之间无空行，Graph / Section 分隔处有空行。
4. **行数对比**：确认总行数减少在预期范围内（约 900+ 行）。
