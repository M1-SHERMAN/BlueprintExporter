# BlueprintExporter 插件 — 第二次迭代需求

## 来源

- 对 AC_EnemyAI 导出结果 v2.0 的逐项测试
- 与 v1.0 输出及迭代需求文档的对照审查

---

## P0：上轮未完成项

### 1. 变量声明区缺少默认值

**来源**：迭代 1 需求 #3 的遗留

**现状**：变量声明区已实现名称、类型、属性标记的输出，但所有变量均**没有默认值列**。

当前输出：
```
  CurrentAIState               : E_AI_State                     [EditDefaultsOnly, BlueprintReadWrite]
  Health                       : float                          [EditDefaultsOnly, BlueprintReadWrite]
```

期望输出：
```
  CurrentAIState               : E_AI_State     = Idle          [EditDefaultsOnly, BlueprintReadWrite]
  Health                       : float          = 100.0         [EditDefaultsOnly, BlueprintReadWrite]
```

**方案**：在遍历 `Blueprint->NewVariables` 时，通过 `FBPVariableDescription::DefaultValue` 获取默认值字符串。枚举类型的默认值需要复用需求 #4 的枚举可读化逻辑（避免再次出现 `NewEnumerator0`）。

**文件**：`BlueprintGraphExtractor.cpp`（提取默认值）、`BlueprintTextFormatter.cpp`（格式化对齐）

---

### 2. 变量属性标记全部相同

**来源**：v2.0 输出审查

**现状**：77 个变量的属性标记全部是 `[EditDefaultsOnly, BlueprintReadWrite]`，没有任何区分。实际蓝图中不太可能所有变量都具有完全一致的属性组合。

**需要排查**：
- `FBPVariableDescription::PropertyFlags` 的读取是否正确
- 是否遗漏了 `EditAnywhere` / `EditInstanceOnly` / `BlueprintReadOnly` / `ExposeOnSpawn` 等标记的判断分支
- 如果变量确实没有手动设置过属性（蓝图编辑器中未勾选），UE 默认赋予的 flag 组合是什么——如果默认组合恰好就是 `EditDefaultsOnly | BlueprintReadWrite`，那说明这个蓝图的变量都没有被定制过属性，输出是正确的，但建议在这种情况下省略属性标记以减少噪音

**文件**：`BlueprintGraphExtractor.cpp`

---

## P1：输出噪音清理

### 3. SwitchEnum 隐藏 Pin 噪音：`Default__KismetMathLibrary`

**现状**：每个 `SwitchEnum` 节点都输出了一行：
```
  ← Not Equal Byte Byte (KismetMathLibrary) = Default__KismetMathLibrary
```

这是 UE 内部用于枚举比较的隐藏 Pin，对 AI 理解逻辑毫无帮助。当前共出现 **17 次**。

**方案**：在 Pin 过滤规则中增加一条：跳过 `DefaultObject` 指向 `Default__` 前缀对象的 Pin。或者更精准地，识别 `SwitchEnum` / `SwitchString` 节点上名为 `"b"` 或函数名包含 `NotEqual` 的隐藏比较 Pin 并过滤。

**文件**：`BlueprintTextFormatter.cpp`（Pin 过滤规则）

---

### 4. LatentActionInfo Pin 噪音

**现状**：所有 Delay / AI MoveTo 等 Latent 节点都显示了完整的 LatentActionInfo 默认值：
```
  ← Latent Info (LatentActionInfo) = (Linkage=-1,UUID=-1,ExecutionFunction="",CallbackTarget=None)
```

这是引擎内部的 Latent 回调元数据，对理解蓝图逻辑没有任何意义。当前共出现 **12 次**。

**方案**：在 Pin 过滤或默认值显示逻辑中，将 `LatentActionInfo` 类型的 Pin 的默认值视为 trivial，不显示。或者直接跳过该 Pin 的输出。

**文件**：`BlueprintTextFormatter.cpp`

---

### 5. OutputDelegate Pin 无意义输出

**现状**：每个 CustomEvent 节点都输出了一行无连接的 delegate Pin：
```
  → Output Delegate (delegate)
```

这个 Pin 是 UE 事件节点自动生成的内部 delegate 输出，在绝大多数蓝图中不会被连接。当前共 **20 次** 出现，且无一有连接。

**方案**：在 Pin 过滤规则中，对 `delegate` 类型的 Pin，如果无连接则跳过输出。（如果有连接则保留——极少数蓝图会绑定事件的 delegate。）

**文件**：`BlueprintTextFormatter.cpp`（Pin 过滤规则）

---

### 6. 默认值中的 `_C` 后缀未清理

**现状**：`GetComponentByClass` 等节点的 ComponentClass 默认值仍显示蓝图生成类后缀：
```
  ← Component Class (ActorComponent) = AC_HitReaction_C
```

期望：
```
  ← Component Class (ActorComponent) = AC_HitReaction
```

Pin 的 SubType 已经在提取时做了 `_C` 后缀清理，但 `DefaultValue` / `DefaultObject` 中的类名没有做同样的处理。

**方案**：在 `ExtractPin` 中，对 `DefaultValue` 和 `DefaultObject` 的值也执行 `_C` 后缀清理（与 SubType 的逻辑一致）。

**文件**：`BlueprintGraphExtractor.cpp`（`ExtractPin` 方法）

---

## P2：可读性增强

### 7. Collapsed 子图内节点名冲突

**现状**：不同的 Collapsed 子图内部都有 `K2Node_Tunnel_0`、`K2Node_CallFunction_0` 等同名节点（当前有 **27 个** `K2Node_Tunnel_0`）。由于子图内容紧跟在 Composite 节点下方并有缩进，AI 可以靠上下文区分，但在跨子图搜索或引用时可能产生歧义。

**方案（暂不实施，观察）**：可以考虑在子图节点名前加作用域前缀，如 `UpdateInputs::K2Node_CallFunction_0`。但这会增加输出长度且破坏与原始节点名的一致性。当前缩进 + `--- Graph: xxx (Collapsed) ---` 分隔符已经提供了足够的上下文，**建议先观察 AI 实际消费时是否出现混淆再决定**。

---

### 8. Composite 子图内的 Execution/Data Flow 中 Reroute 穿透

**现状**：Collapsed 子图内部的 Data Flow 中，Reroute 穿透逻辑似乎已经生效（未观察到 `InputPin` 噪音）。但子图内节点 Pin 中仍然有对 Reroute 节点的直接引用：
```
  ← Target (AC_HitReaction) -> K2Node_Knot_6.OutputPin
```

这不是错误（Pin 详情中显示直接连接关系是合理的），但可以考虑在 Pin 详情中也做穿透，使其显示最终来源节点和 Pin 名。

**方案（低优先级）**：如果实施，需要在 `FormatPin` 的连接目标解析中也调用 `ResolveRerouteChain`。当前不影响理解，可后续考虑。

---

## 实施优先级总结

| 优先级 | 需求 | 工作量估计 |
|--------|------|-----------|
| P0 | #1 变量默认值补全 | 小 |
| P0 | #2 变量属性标记排查 | 小（可能只是验证） |
| P1 | #3 SwitchEnum 隐藏 Pin 过滤 | 小 |
| P1 | #4 LatentActionInfo 噪音过滤 | 小 |
| P1 | #5 OutputDelegate 无连接过滤 | 小 |
| P1 | #6 默认值 `_C` 后缀清理 | 小 |
| P2 | #7 子图节点名冲突 | 观察 |
| P2 | #8 Pin 详情中 Reroute 穿透 | 低优先级 |
