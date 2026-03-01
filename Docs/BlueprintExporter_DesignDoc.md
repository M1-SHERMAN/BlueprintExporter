# UE5 蓝图导出插件 — 技术设计方案

## 1. 目标

开发一个 UE5 Editor Plugin，能够指定任意一个蓝图资产，将其中所有逻辑（EventGraph、自定义函数、宏）导出为 AI 可读的纯文本格式。输出格式与现有 Python 清洗脚本 `ue5_blueprint_cleaner.py` 的输出一致。

### 核心用途
- 将蓝图组件的完整逻辑导出，供 AI 理解和辅助转写为 C++
- 替代手动 Ctrl+C → Python 清洗的工作流，一键导出整个蓝图资产

### 不做的事
- 不做批量/文件夹级别导出（后续可扩展）
- 不做 JSON 格式输出（后续可加命令行参数切换）
- 不做蓝图到 C++ 的自动转换（导出结果供人工或 AI 辅助转写）

---

## 2. 项目环境

- UE5.6，C++ + Blueprint 混合项目
- 插件类型：Editor-only Module（不打包进最终游戏）
- 插件名称建议：`BlueprintExporter`

---

## 3. 插件架构

```
BlueprintExporter/
├── BlueprintExporter.uplugin
├── Source/
│   └── BlueprintExporter/
│       ├── BlueprintExporter.Build.cs
│       ├── Public/
│       │   ├── BlueprintExporterModule.h       // 模块入口
│       │   ├── BlueprintGraphExtractor.h       // 核心：遍历蓝图图表和节点
│       │   └── BlueprintTextFormatter.h        // 格式化输出纯文本
│       └── Private/
│           ├── BlueprintExporterModule.cpp
│           ├── BlueprintGraphExtractor.cpp
│           ├── BlueprintTextFormatter.cpp
│           └── BlueprintExporterCommands.cpp   // 注册菜单/命令
```

### 模块依赖（Build.cs）

```csharp
PublicDependencyModuleNames.AddRange(new string[] {
    "Core",
    "CoreUObject",
    "Engine",
    "UnrealEd",           // 编辑器基础
    "BlueprintGraph",     // UEdGraphNode_*, UK2Node_* 等蓝图节点类
    "KismetCompiler",     // 蓝图编译相关工具类（可能需要）
    "Kismet",             // 蓝图编辑器相关
    "GraphEditor",        // Graph 编辑器工具
    "ToolMenus",          // 注册右键菜单/工具栏按钮
    "ContentBrowser",     // Content Browser 集成
    "AssetTools",         // 资产操作
});
```

### .uplugin 关键配置

```json
{
    "Modules": [
        {
            "Name": "BlueprintExporter",
            "Type": "Editor",
            "LoadingPhase": "Default"
        }
    ]
}
```

---

## 4. 用户交互入口

在 Content Browser 中右键蓝图资产，出现菜单项 **"Export Blueprint Logic"**。

### 注册方式（BlueprintExporterCommands.cpp）

使用 `UToolMenus` 扩展 Content Browser 的资产右键菜单：

```cpp
// 伪代码 — 展示注册思路
void RegisterMenus()
{
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(
        "ContentBrowser.AssetContextMenu.Blueprint"
    );

    FToolMenuSection& Section = Menu->AddSection("BlueprintExporter", 
        LOCTEXT("BlueprintExporter", "Blueprint Exporter"));

    Section.AddMenuEntry(
        "ExportBlueprintLogic",
        LOCTEXT("ExportLabel", "Export Blueprint Logic"),
        LOCTEXT("ExportTooltip", "Export all graphs to AI-readable text"),
        FSlateIcon(),
        FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
        {
            // 1. 获取选中的蓝图资产
            // 2. 调用 BlueprintGraphExtractor 提取
            // 3. 调用 BlueprintTextFormatter 格式化
            // 4. 弹出文件保存对话框，写入 .txt
        })
    );
}
```

### 输出路径

弹出系统文件保存对话框（`DesktopPlatform->SaveFileDialog`），默认文件名为 `{蓝图资产名}_exported.txt`，默认目录为项目根目录。

---

## 5. 核心逻辑：BlueprintGraphExtractor

这是插件最核心的类。负责从 `UBlueprint` 对象中提取所有结构化数据。

### 5.1 数据结构

定义中间数据结构，与现有 Python 脚本的 `NodeInfo` / `PinInfo` 对应：

```cpp
// 单个 Pin 的信息
struct FExportedPin
{
    FString Name;           // 显示名称（优先使用 FriendlyName）
    FString Direction;      // "Input" / "Output"
    FString Category;       // bool, int, float, exec, object, struct, byte, etc.
    FString SubType;        // 具体子类型：Character, Vector, E_AI_State
    FString ContainerType;  // Array, Set, Map, 或空
    FString DefaultValue;   // 默认值
    bool bIsHidden;
    
    // 连接信息：目标节点名 + 目标 Pin 名
    TArray<TPair<FString, FString>> LinkedTo;
};

// 单个节点的信息
struct FExportedNode
{
    FString NodeName;       // e.g. "K2Node_Event_0"
    FString NodeClass;      // e.g. "K2Node_Event"
    FString GraphName;      // 所属 Graph 名称
    
    // 节点属性
    TMap<FString, FString> Properties;  // Event, Function, Variable, CastTo, etc.
    
    TArray<FExportedPin> Pins;
};

// 单个 Graph 的信息
struct FExportedGraph
{
    FString GraphName;      // "EventGraph", "SetCurrentState", etc.
    FString GraphType;      // "EventGraph", "Function", "Macro"
    TArray<FExportedNode> Nodes;
};

// 整个蓝图的导出结果
struct FExportedBlueprint
{
    FString BlueprintName;
    FString ParentClass;    // 父类名称，如 "ActorComponent", "Character"
    TArray<FExportedGraph> Graphs;
};
```

### 5.2 提取流程

```cpp
FExportedBlueprint Extract(UBlueprint* Blueprint);
```

#### Step 1: 获取蓝图基本信息

```cpp
FExportedBlueprint Result;
Result.BlueprintName = Blueprint->GetName();

// 父类
if (Blueprint->ParentClass)
{
    Result.ParentClass = Blueprint->ParentClass->GetName();
}
```

#### Step 2: 遍历所有 Graph

```cpp
TArray<UEdGraph*> AllGraphs;

// EventGraph（可能有多个，通常是1个）
for (UEdGraph* Graph : Blueprint->UbergraphPages)
{
    AllGraphs.Add(Graph);
}

// 自定义函数
for (UEdGraph* Graph : Blueprint->FunctionGraphs)
{
    AllGraphs.Add(Graph);
}

// 宏
for (UEdGraph* Graph : Blueprint->MacroGraphs)
{
    AllGraphs.Add(Graph);
}
```

#### Step 3: 遍历每个 Graph 中的节点

```cpp
for (UEdGraph* Graph : AllGraphs)
{
    FExportedGraph ExportedGraph;
    ExportedGraph.GraphName = Graph->GetName();
    // GraphType 根据来源设置

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        FExportedNode ExportedNode = ExtractNode(Node);
        ExportedGraph.Nodes.Add(ExportedNode);
    }
    
    Result.Graphs.Add(ExportedGraph);
}
```

#### Step 4: 提取单个节点（ExtractNode）

这是最复杂的部分。不同的 K2Node 子类有不同的属性需要提取。

```cpp
FExportedNode ExtractNode(UEdGraphNode* Node)
{
    FExportedNode Result;
    
    // 节点名和类名
    Result.NodeName = Node->GetName();  // "K2Node_Event_0"
    Result.NodeClass = Node->GetClass()->GetName();  // "K2Node_Event"
    
    // --- 按节点类型提取属性 ---
    ExtractNodeProperties(Node, Result);
    
    // --- 提取所有 Pin ---
    for (UEdGraphPin* Pin : Node->Pins)
    {
        FExportedPin ExportedPin = ExtractPin(Pin);
        Result.Pins.Add(ExportedPin);
    }
    
    return Result;
}
```

### 5.3 节点属性提取（关键映射表）

根据节点的 C++ 类型，提取不同的属性。需要 `Cast` 到具体的 K2Node 子类：

| 节点类型 | Cast 目标类 | 需要提取的属性 |
|----------|-------------|---------------|
| Event | `UK2Node_Event` | `EventReference.MemberName` → `Properties["Event"]`；`bOverrideFunction` → `Properties["Override"]` |
| CustomEvent | `UK2Node_CustomEvent` | `CustomFunctionName` → `Properties["Event"]` |
| CallFunction | `UK2Node_CallFunction` | `FunctionReference.MemberName` → `Properties["Function"]`；`FunctionReference.MemberParent` → 可选的前缀 |
| CallArrayFunction | `UK2Node_CallArrayFunction` | 同 CallFunction（它是 CallFunction 的子类） |
| VariableGet / VariableSet | `UK2Node_Variable` | `VariableReference.MemberName` → `Properties["Variable"]` |
| DynamicCast | `UK2Node_DynamicCast` | `TargetType` → `Properties["CastTo"]`（取类名） |
| MacroInstance | `UK2Node_MacroInstance` | `GetMacroGraph()->GetName()` → `Properties["Macro"]` |
| IfThenElse | `UK2Node_IfThenElse` | 无额外属性，类型标记为 BRANCH 即可 |
| SwitchEnum | `UK2Node_SwitchEnum` | 枚举类型名 |
| Timeline | `UK2Node_Timeline` | `TimelineName` → `Properties["Timeline"]` |
| SpawnActorFromClass | `UK2Node_SpawnActorFromClass` | 无额外属性 |
| MakeArray / MakeStruct / BreakStruct | 对应类 | 无额外属性 |
| Knot (Reroute) | `UK2Node_Knot` | 无属性，格式化阶段处理穿透 |

```cpp
void ExtractNodeProperties(UEdGraphNode* Node, FExportedNode& Result)
{
    // Event
    if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
    {
        Result.Properties.Add("Event", 
            EventNode->EventReference.GetMemberName().ToString());
        if (EventNode->bOverrideFunction)
        {
            Result.Properties.Add("Override", "true");
        }
        return;
    }
    
    // CustomEvent
    if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
    {
        Result.Properties.Add("Event", CustomEvent->CustomFunctionName.ToString());
        return;
    }
    
    // CallFunction (包括 CallArrayFunction，因为它是子类)
    if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
    {
        FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
        Result.Properties.Add("Function", FuncName);
        
        // 检查是否为 self 调用
        if (CallNode->FunctionReference.IsSelfContext())
        {
            Result.Properties.Add("SelfContext", "true");
        }
        return;
    }
    
    // VariableGet / VariableSet
    if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
    {
        Result.Properties.Add("Variable",
            VarNode->VariableReference.GetMemberName().ToString());
        return;
    }
    
    // DynamicCast
    if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
    {
        if (CastNode->TargetType)
        {
            Result.Properties.Add("CastTo", CastNode->TargetType->GetName());
        }
        return;
    }
    
    // MacroInstance
    if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
    {
        if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
        {
            Result.Properties.Add("Macro", MacroGraph->GetName());
        }
        return;
    }
    
    // Timeline
    if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
    {
        Result.Properties.Add("Timeline", TimelineNode->TimelineName.ToString());
        return;
    }
    
    // SwitchEnum
    if (UK2Node_SwitchEnum* SwitchNode = Cast<UK2Node_SwitchEnum>(Node))
    {
        if (SwitchNode->Enum)
        {
            Result.Properties.Add("Enum", SwitchNode->Enum->GetName());
        }
        return;
    }
    
    // 其他节点类型不需要额外属性
}
```

### 5.4 Pin 提取

```cpp
FExportedPin ExtractPin(UEdGraphPin* Pin)
{
    FExportedPin Result;
    
    // 名称：优先使用 FriendlyName（显示名），否则用 PinName
    FText DisplayName = Pin->GetDisplayName();
    Result.Name = DisplayName.IsEmpty() 
        ? Pin->PinName.ToString() 
        : DisplayName.ToString();
    
    // 方向
    Result.Direction = (Pin->Direction == EGPD_Output) ? "Output" : "Input";
    
    // 类型
    Result.Category = Pin->PinType.PinCategory.ToString();  // exec, bool, real, object, struct, byte...
    
    // SubType（具体类型名）
    if (Pin->PinType.PinSubCategoryObject.IsValid())
    {
        UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get();
        Result.SubType = SubObj->GetName();
        // 去掉 _C 后缀（蓝图生成类）
        if (Result.SubType.EndsWith("_C"))
        {
            Result.SubType.LeftChopInline(2);
        }
    }
    else if (!Pin->PinType.PinSubCategory.IsNone())
    {
        // float, double 等通过 PinSubCategory 指定
        Result.SubType = Pin->PinType.PinSubCategory.ToString();
    }
    
    // 容器类型
    switch (Pin->PinType.ContainerType)
    {
        case EPinContainerType::Array: Result.ContainerType = "Array"; break;
        case EPinContainerType::Set:   Result.ContainerType = "Set";   break;
        case EPinContainerType::Map:   Result.ContainerType = "Map";   break;
        default: break;
    }
    
    // 默认值
    if (!Pin->DefaultValue.IsEmpty())
    {
        Result.DefaultValue = Pin->DefaultValue;
    }
    else if (Pin->DefaultObject != nullptr)
    {
        Result.DefaultValue = Pin->DefaultObject->GetName();
    }
    
    // 是否隐藏
    Result.bIsHidden = Pin->bHidden;
    
    // 连接关系
    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
    {
        FString TargetNodeName = LinkedPin->GetOwningNode()->GetName();
        FString TargetPinName;
        FText TargetDisplayName = LinkedPin->GetDisplayName();
        TargetPinName = TargetDisplayName.IsEmpty()
            ? LinkedPin->PinName.ToString()
            : TargetDisplayName.ToString();
        
        Result.LinkedTo.Add(TPair<FString, FString>(TargetNodeName, TargetPinName));
    }
    
    return Result;
}
```

---

## 6. 格式化输出：BlueprintTextFormatter

将 `FExportedBlueprint` 格式化为纯文本。输出格式须与现有 Python 清洗脚本 `ue5_blueprint_cleaner.py` 的输出完全一致。

### 6.1 整体文件结构

```
=== Blueprint: AC_EnemyAI (Parent: ActorComponent) ===

--- Graph: EventGraph ---

[K2Node_Event_0] EVENT
  Event: ReceiveBeginPlay
  (Override)
  → then (exec) -> K2Node_VariableSet_5.execute

[K2Node_CallFunction_11] CALL
  Function: GetOwner
  ← Target (ActorComponent)
  → ReturnValue (Actor) -> K2Node_DynamicCast_0.Object

...

=== Execution Flow ===
  K2Node_Event_0 --> K2Node_DynamicCast_0
  ...

=== Data Flow ===
  K2Node_CallFunction_11.ReturnValue --> K2Node_DynamicCast_0.Object
  ...

--- Graph: SetCurrentState (Function) ---

[K2Node_FunctionEntry_0] FUNC_ENTRY
  ...

=== Execution Flow ===
  ...
```

### 6.2 格式化规则（与 Python 脚本一致）

参照 Python 脚本 `BlueprintFormatter` 类的逻辑，逐条实现：

**节点类型名映射**（`_get_readable_type`）：

| C++ 类名 | 输出标记 |
|----------|---------|
| UK2Node_Event | EVENT |
| UK2Node_CustomEvent | CUSTOM_EVENT |
| UK2Node_CallFunction | CALL |
| UK2Node_CallArrayFunction | CALL(Array) |
| UK2Node_VariableGet | GET |
| UK2Node_VariableSet | SET |
| UK2Node_IfThenElse | BRANCH |
| UK2Node_SwitchEnum | SWITCH |
| UK2Node_SwitchInteger | SWITCH(Int) |
| UK2Node_MacroInstance | MACRO |
| UK2Node_DynamicCast | CAST |
| UK2Node_Knot | REROUTE |
| UK2Node_FunctionEntry | FUNC_ENTRY |
| UK2Node_FunctionResult | FUNC_RESULT |
| UK2Node_Timeline | TIMELINE |
| UK2Node_SpawnActorFromClass | SPAWN_ACTOR |
| UK2Node_MakeArray | MAKE_ARRAY |
| UK2Node_MakeStruct | MAKE_STRUCT |
| UK2Node_BreakStruct | BREAK_STRUCT |
| UK2Node_Select | SELECT |
| UK2Node_ForEachLoop | FOR_EACH |
| UK2Node_Delay | DELAY |
| 其他 | 去掉 "K2Node_" 前缀 |

**Pin 过滤规则**（`_get_meaningful_pins`）：
1. 跳过隐藏的 self / Target pin（`bIsHidden && Name in {self, Target}`）
2. 跳过无连接的 Output_Get pin
3. 跳过 Reroute 节点的所有 pin
4. 跳过无连接、无有意义默认值的 exec pin（但保留有名称的 exec 输出如 Branch 的 true/false）
5. 跳过隐藏且无连接无默认值的 pin

**Pin 格式化**（`_format_pin`）：
- 方向标记：Output 用 `→`，Input 用 `←`
- 类型显示：优先用 SubType，否则用 Category；有 ContainerType 时包裹为 `Array<type>`
- 连接目标：`-> TargetNode.TargetPin`
- 默认值：跳过 `0`, `0.0`, `0, 0, 0`, `false`, `None`，其余显示 `= value`

**Reroute 穿透**：
- 在 Execution Flow 和 Data Flow 总览中，递归解析 Reroute 链，显示最终目标节点

**拓扑排序**：
- 按执行流（exec pin 连接）做 Kahn 拓扑排序
- Event 节点优先
- 非连通节点追加到末尾

---

## 7. 实现顺序（建议分4步）

### Phase 1: 插件骨架
- 创建 Editor Module 模板
- 注册 Content Browser 右键菜单项
- 点击后能加载 UBlueprint 并弹出文件保存对话框
- 验收标准：右键蓝图 → 点击菜单项 → 弹出保存对话框 → 写入一个空文件

### Phase 2: 数据提取
- 实现 `BlueprintGraphExtractor`
- 遍历所有 Graph + Node + Pin
- 输出中间数据结构 `FExportedBlueprint`
- 验收标准：对一个测试蓝图，能打印出所有节点名、Pin名、连接关系到 Output Log

### Phase 3: 文本格式化
- 实现 `BlueprintTextFormatter`
- 输出与 Python 清洗脚本一致的纯文本格式
- 验收标准：同一个蓝图，C++ 插件的输出与 Ctrl+C → Python 清洗脚本的输出在结构和信息量上一致

### Phase 4: 打磨
- 处理边界情况（空 Graph、编译错误的蓝图、Widget Blueprint 等非标准蓝图类型）
- 添加 Output Log 的进度提示
- 测试大型蓝图（100+ 节点）的性能和输出完整性

---

## 8. 关键 API 速查

| 需求 | UE5 API |
|------|---------|
| 加载蓝图资产 | `LoadObject<UBlueprint>(nullptr, *AssetPath)` 或从 ContentBrowser 选中资产获取 |
| 获取所有 EventGraph | `UBlueprint::UbergraphPages` |
| 获取所有函数 Graph | `UBlueprint::FunctionGraphs` |
| 获取所有宏 Graph | `UBlueprint::MacroGraphs` |
| Graph 中的节点列表 | `UEdGraph::Nodes` (TArray\<UEdGraphNode*\>) |
| 节点的所有 Pin | `UEdGraphNode::Pins` (TArray\<UEdGraphPin*\>) |
| Pin 的连接 | `UEdGraphPin::LinkedTo` (TArray\<UEdGraphPin*\>) |
| Pin 的类型信息 | `UEdGraphPin::PinType` (FEdGraphPinType) |
| Pin 的显示名 | `UEdGraphPin::GetDisplayName()` |
| Pin 的默认值 | `UEdGraphPin::DefaultValue`, `DefaultObject`, `DefaultTextValue` |
| Pin 是否隐藏 | `UEdGraphPin::bHidden` |
| 判断节点类型 | `Cast<UK2Node_Event>(Node)` 等 |
| 事件名 | `UK2Node_Event::EventReference.GetMemberName()` |
| 函数名 | `UK2Node_CallFunction::FunctionReference.GetMemberName()` |
| 变量名 | `UK2Node_Variable::VariableReference.GetMemberName()` |
| Cast 目标类 | `UK2Node_DynamicCast::TargetType` (UClass*) |
| 宏引用 | `UK2Node_MacroInstance::GetMacroGraph()` |
| Timeline 名称 | `UK2Node_Timeline::TimelineName` |
| 注册右键菜单 | `UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.Blueprint")` |
| 文件保存对话框 | `FDesktopPlatformModule::Get()->SaveFileDialog(...)` |

---

## 9. 注意事项

1. **UK2Node 子类的头文件分散在多个模块中**。常用的在 `BlueprintGraph/Classes/K2Node_*.h`，但部分在 `Engine` 或 `KismetCompiler` 中。编译时遇到找不到头文件的情况，需要检查对应模块是否在 Build.cs 中声明了依赖。

2. **Pin 的 PinCategory 是 FName 而非 enum**。常见值：`exec`, `bool`, `int`, `int64`, `real`, `byte`, `name`, `string`, `text`, `struct`, `object`, `class`, `interface`, `softobject`, `softclass`, `delegate`, `wildcard`。

3. **`real` 类型的 PinSubCategory** 区分 `float` 和 `double`，在格式化时应优先显示 SubCategory。

4. **蓝图编译状态**：有些蓝图可能处于编译错误状态，此时 Graph 和 Node 仍然可以遍历，但部分 Pin 连接可能不完整。插件应容错处理，不要因为个别节点出错就中断整个导出。

5. **UEdGraphNode::GetName()** 返回的是对象名（如 `K2Node_Event_0`），这和 Ctrl+C 导出文本中的 `Name=` 字段一致，可以直接使用。

6. **Comment 节点**（`UEdGraphNode_Comment`）不属于 K2Node，不参与执行流。如果遇到，可以选择忽略或单独输出为注释文本。

7. **Collapsed Graph / Composite Node**：有些节点内部包含子图（如 Collapsed Graph），如果需要展开这些子图，需要递归处理。初期建议先标记为 "COLLAPSED: {名称}" 而不展开。

---

## 10. 参考文件

实现时需参考的现有代码：

- **Python 清洗脚本**：`ue5_blueprint_cleaner.py` — 格式化逻辑的参考实现，C++ 侧的 `BlueprintTextFormatter` 应产出与之结构一致的输出
- **Python 测试用例**：`test_blueprint_cleaner.py` — 包含 48 个测试场景，覆盖各种节点类型和边界情况，可用于验证 C++ 插件输出的正确性
- **清洗后的输出样例**：`blueprint_cleaned_1.txt`（AC_EnemyAI 组件的 BeginPlay 导出结果）— 作为目标输出格式的参照
