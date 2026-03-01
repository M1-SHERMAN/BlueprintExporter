# BlueprintExporter 插件 — 第四次迭代需求

## 来源

- 对 AC_EnemyAI 导出结果 v4.0 的逐项测试（第三次迭代 5 项变更中 4 项通过，1 项需修复）
- 新功能需求：支持导出编辑器中选中的蓝图节点

---

## 目标

本次迭代包含两部分：
1. **Bug 修复**：第三次迭代变更 #3（未连接非 exec 输出 Pin 过滤）的漏网问题
2. **新功能**：支持在蓝图编辑器中选中部分节点后导出，而非仅能导出整个蓝图资产

---

## P0：Bug 修复

### 1. 未连接非 exec 输出 Pin 过滤不完整

**来源**：第三次迭代验证

**现状**：v4.0 中仍残留 **63 个**无连接、无消费者的非 exec 输出 Pin，占 v3.0 原始 102 个的 62%。第三次迭代仅成功移除了 39 个。

**残留样例**：

```
[K2Node_CallFunction_16] CALL
  Function: GameplayStatics::BreakHitResult
  ← Hit (HitResult) -> K2Node_CallFunction_0.Out Hit
  → Blocking Hit (bool)           ← 无连接，应移除
  → Initial Overlap (bool)        ← 无连接，应移除
  → Time (float)                  ← 无连接，应移除
  → Distance (float)              ← 无连接，应移除
  → Location (Vector) -> K2Node_CallFunction_5.Target   ← ✅ 有连接，保留
  → Impact Point (Vector)         ← 无连接，应移除
  ...（14 个未用字段中仅 1 个有连接）

[K2Node_CallFunction_17] CALL
  Function: KismetMathLibrary::BreakVector
  ← In Vec (Vector) -> K2Node_CallFunction_27.Return Value
  → X (double)                    ← 无连接，应移除
  → Y (double)                    ← 无连接，应移除
  → Z (double) -> K2Node_CallFunction_11.Target Z   ← ✅ 有连接，保留
```

**残留分布**：

| 来源 | 残留数 |
|------|--------|
| `GameplayStatics::BreakHitResult` | 28（2 实例 × 14 字段） |
| `KismetMathLibrary::BreakVector` | 4 |
| `Actor::K2_SetActorRotation` 返回值 | 3 |
| `KismetArrayLibrary::Array_Add/RemoveItem` 返回值 | 5 |
| `VariableGet` 布尔值输出 | 7 |
| `K2_GetActorLocation` 拆分子分量 | 4 |
| 其他 CALL 返回值 | 12 |

**根因分析**：成功移除的 39 个 Pin 的类型为 `HitResult`、`TimerHandle`、`Actor`、`PhysicalMaterial` 等复合/对象类型；残留的 63 个全部是 `bool`、`int`、`float`、`double`、`Vector`、`name` 等基础类型。

推测 `GetMeaningfulPins` 中的过滤条件：

```cpp
if (Pin.Direction == TEXT("Output")
    && Pin.Category != TEXT("exec")
    && Pin.LinkedTo.Num() == 0
    && Pin.DefaultValue.IsEmpty())  // ← 此处过严
{
    continue;
}
```

基础类型 Pin 即使无连接，也可能携带 trivial 默认值（如 `bool` 的 `"false"`、`int` 的 `"0"`、`float` 的 `"0.0"`、`Vector` 的 `"0, 0, 0"` 等），导致 `IsEmpty()` 为 false，跳过了过滤。

**方案**：将条件中的 `Pin.DefaultValue.IsEmpty()` 放宽为包含 trivial 默认值的情况：

```cpp
if (Pin.Direction == TEXT("Output")
    && Pin.Category != TEXT("exec")
    && Pin.LinkedTo.Num() == 0
    && (Pin.DefaultValue.IsEmpty() || IsTrivialDefault(Pin.DefaultValue)))
{
    continue;
}
```

**文件**：`BlueprintTextFormatter.cpp`（`GetMeaningfulPins` 方法）

**预计减少**：~63 行

---

## P1：新功能 — 导出选中节点

### 2. 支持从蓝图编辑器中导出选中的节点

**来源**：用户需求

**问题**：当前插件仅支持从 Content Browser 右键导出整个蓝图资产的全部 Graph。在实际工作流中，经常只需要查看或转写蓝图中的一小段逻辑（例如某个事件的处理链、某个分支的实现），导出整个蓝图（如 AC_EnemyAI 的 6000+ 行）既浪费 token 又增加 AI 理解负担。

**期望行为**：在蓝图编辑器中框选若干节点 → 右键或工具栏按钮 → 导出选中节点的逻辑文本（包括节点详情、它们之间的 Execution Flow 和 Data Flow）。

**方案概述**：

#### 2a. 获取选中节点

通过蓝图编辑器的 `FBlueprintEditor` 获取当前选中的节点集合：

```cpp
// 从当前活跃的蓝图编辑器获取选中节点
FBlueprintEditor* BlueprintEditor = /* 获取当前蓝图编辑器实例 */;
if (BlueprintEditor)
{
    TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->GetFocusedGraphEditorWidget();  
    if (GraphEditor.IsValid())
    {
        const FGraphPanelSelectionSet& SelectedNodes = GraphEditor->GetSelectedNodes();
        // SelectedNodes 是 TSet<UObject*>，其中每个元素可以 Cast 为 UEdGraphNode*
    }
}
```

获取 `FBlueprintEditor` 实例的方式：遍历 `FAssetEditorManager::Get().GetAllEditorsForAsset(Blueprint)` 或通过 `GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()` 定位当前打开的蓝图编辑器。

#### 2b. 提取逻辑

复用现有 `FBlueprintGraphExtractor` 的节点级提取逻辑（`ExtractNode`、`ExtractPin`），但作用范围限定为选中节点集合：

```cpp
FExportedGraph ExtractSelectedNodes(const TSet<UEdGraphNode*>& SelectedNodes, UEdGraph* OwningGraph);
```

- 遍历 `SelectedNodes`，对每个节点调用 `ExtractNode`
- Pin 的连接关系保留完整信息（包括连向选区外节点的连接），但在格式化时可以标注哪些是选区边界

#### 2c. 格式化输出

复用 `FBlueprintTextFormatter` 的格式化逻辑，但有以下调整：

- **标题行**改为标注选区来源：`=== Selected Nodes from: AC_EnemyAI / EventGraph (X nodes) ===`
- **Execution Flow / Data Flow**：仅生成选中节点之间的连接关系
- **边界连接标注**：连向选区外节点的 Pin 连接可加 `(external)` 标记，帮助 AI 理解上下文边界：

```
[K2Node_IfThenElse_5] BRANCH
  ← Condition (bool) -> K2Node_VariableGet_10.CanReceiveThreatAlert (external)
  → True (exec) -> K2Node_CallArrayFunction_0.execute
  → False (exec) -> K2Node_MacroInstance_3.exec (external)
```

- **变量声明区**：不输出（选区导出关注局部逻辑，变量信息在完整导出中获取）
- **拓扑排序**：仅在选中节点集合内排序

#### 2d. 用户交互入口

在蓝图编辑器的节点右键菜单中添加菜单项。需要扩展 `GraphEditor` 的上下文菜单（通过 `SGraphEditor` 的 `OnCreateNodeOrPinMenu` 委托或 `UToolMenus` 扩展 `GraphEditor.GraphContextMenu`）。

右键菜单中注册两个菜单项，对应两种输出方式：

- **"Copy Selected Nodes as Text"** — 将导出文本直接复制到系统剪贴板。适合选区较小（几个到几十个节点）的日常场景，用户可直接粘贴到 AI 对话窗口。
- **"Export Selected Nodes to File..."** — 弹出文件保存对话框，将导出文本写入 `.txt` 文件。适合选区较大或需要存档的场景。

两个菜单项共享同一条提取 + 格式化管线，仅在最终输出阶段分叉（`FPlatformApplicationMisc::ClipboardCopy` vs 文件写入）。

关于性能：提取和格式化阶段是纯内存字符串操作，即使 100+ 节点也只产生几百 KB 文本，两种输出方式的性能差异可忽略。选区过大时剪贴板方案的瓶颈不在性能，而在使用体验——大段文本粘贴到对话窗口不如附件方便，因此提供文件输出作为备选。

当没有选中任何节点时，两个菜单项应 Disable（灰显），Tooltip 提示 "No nodes selected"。

**输出格式示例**：

```
=== Selected Nodes from: AC_EnemyAI / EventGraph (6 nodes) ===

[K2Node_Event_0] EVENT
  Event: ReceiveBeginPlay
  (Override)
  → then (exec) -> K2Node_DynamicCast_0.execute
[K2Node_CallFunction_11] CALL
  Function: GetOwner
  → ReturnValue (Actor) -> K2Node_DynamicCast_0.Object
[K2Node_DynamicCast_0] CAST
  CastTo: Character
  → then (exec) -> K2Node_VariableSet_5.execute (external)
  → CastFailed (exec)
  ← Object (Object) -> K2Node_CallFunction_11.ReturnValue
  → AsCharacter (Character) -> K2Node_VariableSet_5.As Character (external)

=== Execution Flow ===
  K2Node_Event_0 --> K2Node_DynamicCast_0

=== Data Flow ===
  K2Node_CallFunction_11.ReturnValue --> K2Node_DynamicCast_0.Object
```

**文件**：
- `BlueprintGraphExtractor.h/cpp`（新增 `ExtractSelectedNodes` 方法）
- `BlueprintTextFormatter.h/cpp`（新增选区格式化模式 + `(external)` 标记逻辑）
- `BlueprintExporterCommands.cpp`（注册蓝图编辑器右键菜单）
- `BlueprintExporter.Build.cs`（可能需要添加 `BlueprintEditor` 模块依赖：`Kismet`）

---

## 实施优先级总结

| 优先级 | 需求 | 工作量估计 |
|--------|------|-----------|
| P0 | #1 未连接输出 Pin 过滤修复 | 小（一行条件修改） |
| P1 | #2 导出选中节点 | 中 |

---

## 验证方法

### 需求 #1 验证

1. 对 AC_EnemyAI 运行完整导出，生成 v5.0 输出。
2. 搜索所有 `→ ... (非exec类型)` 行中无 `->` 连接的行，确认数量为 **0**（或仅剩 exec 类型的命名输出）。
3. 确认 `BreakHitResult` 节点仅保留有连接的 `→ Location (Vector) -> ...`，其余 14 个字段均不显示。
4. 确认 `BreakVector` 节点仅保留有连接的 `→ Z (double) -> ...`，X/Y 不显示。
5. 确认有连接的输出 Pin 未受影响。

### 需求 #2 验证

1. 打开 AC_EnemyAI 蓝图的 EventGraph，框选 BeginPlay 事件链的前 5-6 个节点。
2. 右键 → "Copy Selected Nodes as Text"，确认剪贴板中包含正确的节点详情。
3. 右键 → "Export Selected Nodes to File..."，确认弹出保存对话框并正确写入文件，内容与剪贴板方式一致。
4. 确认 Execution Flow 和 Data Flow 仅包含选中节点之间的连接。
5. 确认连向选区外节点的 Pin 带有 `(external)` 标记。
6. 在非选中状态下，确认两个菜单项均为灰显（Disabled），Tooltip 提示 "No nodes selected"。
7. 测试 Collapsed 子图中的节点选中导出是否正常工作。
