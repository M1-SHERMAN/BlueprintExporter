# BlueprintExporter 插件 — 迭代需求

## 来源

- Code Review（代码审查发现的问题）
- Codex 测试反馈（AI 实际消费导出文本后的改进建议）

---

## P0：信息缺失类（影响 AI 理解完整性）

### 1. Composite 节点子图展开

**来源**：Codex 反馈

**问题**：`K2Node_Composite` 节点只显示了入口连接，内部折叠的子图逻辑没有展开，导致关键逻辑（如 Attack Event、Defend Event 的具体实现）完全丢失。

**方案**：在 `ExtractNodeProperties` 中检测 `UK2Node_Composite`，通过 `GetBoundGraph()` 获取其内部子图，递归调用 `ExtractGraph` 将子图内容展开输出。输出时作为该节点的嵌套 section，格式类似：

```
[K2Node_Composite_0] COLLAPSED: AttackLogic
  --- Collapsed Graph: AttackLogic ---
  [K2Node_Tunnel_0] TUNNEL_ENTRY
    ...
  [K2Node_CallFunction_0] CALL
    ...
  === Execution Flow ===
    ...
```

**文件**：`BlueprintGraphExtractor.cpp`（提取）、`BlueprintTextFormatter.cpp`（嵌套格式化）

---

### 2. Interface Graph 遍历缺失

**来源**：Code Review

**问题**：Extractor 遍历了 `UbergraphPages`、`FunctionGraphs`、`MacroGraphs`，但遗漏了 `ImplementedInterfaces` 中的 Graph。如果蓝图实现了 Interface，这些函数的逻辑不会被导出。

**方案**：在 `FBlueprintGraphExtractor::Extract` 中添加 Interface 遍历：

```cpp
for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
{
    for (UEdGraph* Graph : InterfaceDesc.Graphs)
    {
        if (Graph)
        {
            Result.Graphs.Add(ExtractGraph(Graph, TEXT("Interface")));
        }
    }
}
```

**文件**：`BlueprintGraphExtractor.cpp`

---

### 3. 变量声明区

**来源**：Codex 反馈

**问题**：导出文本缺少蓝图变量的整体声明信息。AI 在阅读逻辑流时需要反复推断变量类型和用途，如果开头有一个变量表会显著提升理解效率。

**方案**：在 `FBlueprintGraphExtractor::Extract` 中遍历 `Blueprint->NewVariables`（`TArray<FBPVariableDescription>`），提取每个变量的名称、类型、默认值、属性标记（EditAnywhere / BlueprintReadOnly 等）。在 `FExportedBlueprint` 中新增一个 `TArray<FExportedVariable>` 字段。

输出格式示例（放在 Blueprint 标题之后、第一个 Graph 之前）：

```
=== Blueprint: AC_EnemyAI (Parent: ActorComponent) ===

=== Variables ===
  CurrentState       : E_AI_State     = Idle          [EditAnywhere, BlueprintReadWrite]
  Health             : float          = 100.0         [EditAnywhere, BlueprintReadWrite]
  AttackSlotRef      : Actor          =               [BlueprintReadOnly]
  bIsStrafing        : bool           = false
```

**文件**：`BlueprintExporterTypes.h`（新增 `FExportedVariable`）、`BlueprintGraphExtractor.cpp`（提取变量）、`BlueprintTextFormatter.cpp`（格式化变量表）

---

## P1：输出质量类（影响 AI 理解准确性）

### 4. 枚举默认值显示为成员名

**来源**：Codex 反馈

**问题**：枚举默认值显示为 `NewEnumerator0`、`NewEnumerator2` 等 Raw 内部名称，AI 无法理解其实际含义。

**方案**：在 `FBlueprintGraphExtractor::ExtractPin` 中，当 Pin 的 `PinSubCategoryObject` 指向一个 `UEnum` 时，使用 `UEnum::GetDisplayNameTextByValue` 或 `GetNameByValue` 将 `DefaultValue` 中的 Raw 名称替换为人类可读的枚举成员名。

示例：`NewEnumerator0` → `Idle`，`NewEnumerator2` → `Attack`

**文件**：`BlueprintGraphExtractor.cpp`

---

### 5. FormatPin 默认值显示逻辑修正

**来源**：Code Review

**问题**：当前 `FormatPin` 中，trivial 默认值（`0`, `false`, `None` 等）在无连接时仍会显示，与 Python 清洗脚本的行为不一致，且会产生大量噪音。

**方案**：去掉 `IsTrivialDefault` 为 true 时的 else 分支，只在非 trivial 默认值时显示 `= value`。修改后的逻辑：

```cpp
FString Default;
if (!Pin.DefaultValue.IsEmpty() && !IsTrivialDefault(Pin.DefaultValue))
{
    Default = FString::Printf(TEXT(" = %s"), *Pin.DefaultValue);
}
```

exec 和 delegate 类型的 pin 始终不显示默认值。

**文件**：`BlueprintTextFormatter.cpp`（`FormatPin` 方法）

---

### 6. DataFlow 中 Reroute 穿透后 Pin 名称不准确

**来源**：Code Review

**问题**：`FormatDataFlow` 在 Reroute 穿透后，目标 Pin 名使用的是 `Link.Value`（Reroute 节点的 `InputPin` 名称），而非穿透后最终目标节点上的实际 Pin 名。导致输出类似 `Source.ReturnValue --> FinalTarget.InputPin`。

**方案**：修改 `ResolveRerouteChain` 使其同时返回最终节点名和最终 Pin 名（返回 `TPair<FString, FString>`），在穿透过程中追踪 Output Pin 连接到的下一个节点的 Input Pin 名称。

**文件**：`BlueprintTextFormatter.h`（修改 `ResolveRerouteChain` 返回类型）、`BlueprintTextFormatter.cpp`（实现穿透时 Pin 名追踪）

---

## P2：增强类（锦上添花）

### 7. 变量初始值与字面量标注区分

**来源**：Codex 反馈

**问题**：Pin 上显示的 `= false`、`= 0, 0, 0` 有时是变量声明的默认值，有时是节点 Pin 上的字面量输入，AI 难以区分二者。

**方案**：如果实现了变量声明区（需求 #3），变量的默认值已经在声明区展示，节点 Pin 上的值自然就是"当前 Pin 的输入值"，歧义会大幅降低。可以在变量声明区用 `(default)` 标注，Pin 上的值不加标注，靠上下文自然区分。暂不做额外标注，观察变量声明区实现后是否仍有歧义。

---

## 实施优先级总结

| 优先级 | 需求 | 工作量估计 |
|--------|------|-----------|
| P0 | #1 Composite 子图展开 | 中 |
| P0 | #2 Interface Graph 遍历 | 小 |
| P0 | #3 变量声明区 | 中 |
| P1 | #4 枚举默认值可读化 | 小 |
| P1 | #5 默认值显示逻辑修正 | 小 |
| P1 | #6 Reroute Pin 名修正 | 小 |
| P2 | #7 初始值与字面量区分 | 观察 |
