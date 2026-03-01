# BlueprintExporter 插件参考文档

> 适用版本：v7（双层增量检测）
> 引擎：Unreal Engine 5.x（Editor-only Plugin）
> 作者：Capybara、Claude

---

## 目录

1. [功能概述](#1-功能概述)
2. [使用入口](#2-使用入口)
3. [Editor Preference Settings 配置](#3-Editor-Preference-Settings-配置)
4. [输出目录结构](#4-输出目录结构)
5. [输出格式规范](#5-输出格式规范)
6. [增量检测机制](#6-增量检测机制)
7. [技术架构](#7-技术架构)
8. [数据结构参考](#8-数据结构参考)
9. [注意事项](#9-注意事项)

---

## 1\. 功能概述

BlueprintExporter 是一个 UE5 Editor-only 插件，将蓝图的节点图（EventGraph、函数、宏、接口实现）导出为 AI 可读的纯文本格式。

**核心用途**：将蓝图逻辑完整地描述给 AI，辅助理解与转写为 C++。

**五项核心功能**：

|功能|触发方式|
|-|-|
|按需导出单个/多个蓝图|Content Browser 右键 → Export Blueprint Logic|
|批量全量导出|Content Browser 右键 → Export All Blueprints to Cache|
|自动导出（保存时）|Ctrl+S 时自动触发，受 `bAutoExportOnSave` 开关控制|
|关闭编辑器时全量导出|关闭编辑器前自动触发，受 `bExportOnEditorClose` 开关控制|
|选中节点导出|蓝图编辑器内右键节点 → Copy / Export Selected Nodes|

---

## 2\. 使用入口

### 2.1 Content Browser 右键菜单

在 Content Browser 中选中一个或多个蓝图资产，右键，可看到 **Blueprint Exporter** 分节：

```
Blueprint Exporter
  ├── Export Blueprint Logic           ← 导出选中蓝图到文件（弹出保存对话框）
  └── Export All Blueprints to Cache  ← 全量扫描并导出所有蓝图到缓存目录
```

**Export Blueprint Logic**

* 弹出系统"另存为"对话框
* 默认文件名：`{蓝图名}\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_exported.txt`
* 默认目录：项目根目录
* 输出：完整的单文件格式（包含所有图表）

**Export All Blueprints to Cache**

* 无对话框，直接输出到 `{ProjectDir}/Saved/BlueprintExports/`
* 使用双层增量检测，跳过未修改的蓝图
* 完成后在 Output Log 打印统计：`ExportAll complete: X exported, Y skipped, Z filtered out, N total`

### 2.2 蓝图编辑器内右键菜单

在蓝图编辑器中选中节点，右键任意节点，菜单底部出现 **Blueprint Exporter** 分节：

```
Blueprint Exporter
  ├── Copy Selected Nodes as Text     ← 复制到剪贴板（无选中时置灰）
  └── Export Selected Nodes to File...← 导出到文件（无选中时置灰）
```

**Copy Selected Nodes as Text**

* 将当前选中节点格式化为文本，写入系统剪贴板
* 可直接粘贴给 AI

**Export Selected Nodes to File...**

* 弹出"另存为"对话框
* 默认文件名：`{蓝图名}\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_selection.txt`

选中模式的特殊行为：外部连接（连接到未选中节点的 Pin）在 Pin 连接后加注 `(external)` 标记；执行流/数据流摘要只输出选集内部的连接。

### 2.3 自动导出

在 Project Settings 中启用 `Auto Export On Save` 后：

* 每次 Ctrl+S 保存蓝图时，插件检测到 `PackageSavedWithContextEvent` 事件
* 若蓝图通过 `ShouldExport()` 过滤，自动导出到缓存目录
* 保存完成后更新 `\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_index.txt` 和 `README.md`

---

## 3\. Editor Preference Settings 配置

位置：`Editor Preference Settings → Plugins → Blueprint Exporter`

配置文件：`Config/DefaultEditorPerProjectUserSettings.ini`（per-project，不提交版本控制）

### Auto Export

|属性|类型|默认值|说明|
|-|-|-|-|
|`bAutoExportOnSave`|bool|`false`|每次保存蓝图时自动导出到缓存|
|`bExportOnEditorClose`|bool|`false`|关闭编辑器前执行一次全量导出|

### Export Filter — Blueprint Type

|属性|默认值|说明|
|-|-|-|
|`bExportNormalBlueprint`|`true`|导出普通蓝图（Actor、Component 等）|
|`bExportFunctionLibrary`|`true`|导出函数库蓝图|
|`bExportMacroLibrary`|`false`|导出宏库蓝图|
|`bExportInterface`|`false`|导出接口蓝图|
|`bExportLevelScript`|`false`|导出关卡脚本蓝图|

### Export Filter — Parent Class

|属性|类型|说明|
|-|-|-|
|`ParentClassFilter`|`TArray<FSoftClassPath>`|白名单：只导出继承自列表中类的蓝图；**留空 = 不限制**|
|`ExcludedParentClasses`|`TArray<FSoftClassPath>`|黑名单：排除继承自列表中类的蓝图；**优先级高于白名单**|

### Export Filter — Content

|属性|类型|默认值|说明|
|-|-|-|-|
|`MinNodeCount`|`int32`|`2`|所有图表中节点总数低于此值时跳过导出（过滤空蓝图）|

---

## 4\. 输出目录结构

全量导出（含自动导出）输出到：

```
{ProjectDir}/Saved/BlueprintExports/
├── README.md                       ← 所有蓝图的 Markdown 汇总表
├── \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_index.txt                      ← 纯文本索引（蓝图名/父类/图表数/变量数）
│
├── AC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyAI/
│   ├── \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_summary.txt                ← 蓝图头部 + 变量 + 图表列表（无节点内容）
│   ├── EventGraph.txt              ← EventGraph 完整内容
│   └── SetCurrentState.txt         ← 函数图 SetCurrentState 的完整内容
│
├── BP\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyBase/
│   ├── \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_summary.txt
│   └── ...
│
└── ...
```

**文件名规则**：图表名经 `SanitizeFileName()` 处理——只保留字母、数字、`\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_`、`-`，其余字符替换为 `\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_`。

### \_summary.txt 内容示例

```
=== Blueprint: AC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyAI (Parent: ActorComponent) ===

=== Variables ===
  CurrentState : E\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_AI\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_State  = Idle
  OwnerEnemy   : ACA\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyBase

=== Graphs ===
  EventGraph       (EventGraph)  14 nodes
  SetCurrentState  (Function)    6 nodes
```

### README.md 内容示例

```markdown
# Blueprint Exports

Generated: 2026.03.01-10.30.00

| Blueprint | Parent | Graphs | Variables |
|-----------|--------|--------|-----------|
| AC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyAI | ActorComponent | 2 | 3 |
| BP\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyBase | ACA\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyBase | 1 | 0 |
```

---

## 5\. 输出格式规范

### 5.1 完整单文件格式（Export Blueprint Logic）

```
=== Blueprint: {Name} (Parent: {Parent}) ===

=== Variables ===
  {VarName}  : {Type}  \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[= {Default}]  \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[Flags]]

--- Graph: {GraphName} ---          ← EventGraph 省略类型标注
--- Graph: {GraphName} ({Type}) --- ← Function/Macro/Interface 带类型

\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[{NodeName}] {NodeType}
  {Property}: {Value}
  → {PinName} ({Type})\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[= {Default}] \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[-> {TargetNode}.{TargetPin}]
  ← {PinName} ({Type})\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[= {Default}] \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[-> {TargetNode}.{TargetPin}]

=== Execution Flow ===
  {SourceNode} --> {TargetNode}
  {SourceNode} \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[{PinLabel}] --> {TargetNode}

=== Data Flow ===
  {SourceNode}.{SourcePin} --> {TargetNode}.{TargetPin}
```

### 5.2 变量格式

```
  {Name} : {Type}                       ← 基础类型，无默认值
  {Name} : {Type} = {Default}           ← 有非平凡默认值
  {Name} : Array<{Type}>                ← 容器类型
  {Name} : {Type} = {Default}  \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[Flags]  ← 有非默认 Flag 组合
```

**Flag 输出规则**：`EditDefaultsOnly + BlueprintReadWrite` 是默认组合，**不输出**。其余组合如 `EditAnywhere`、`BlueprintReadOnly`、`Replicated` 等用方括号标注。

**平凡默认值**（不输出）：`0`、`0.0`、`0.000000`、`0, 0, 0`、`false`、`None`、`()`、`(())`、`(X=0.000000,Y=0.000000,Z=0.000000)`

### 5.3 节点类型标记映射

|C++ 类名|输出标记|
|-|-|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Event`|`EVENT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_CustomEvent`|`CUSTOM\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EVENT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_ComponentBoundEvent`|`COMPONENT\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EVENT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_CallFunction`|`CALL`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_CallArrayFunction`|`CALL(Array)`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_VariableGet`|`GET`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_VariableSet`|`SET`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_IfThenElse`|`BRANCH`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SwitchEnum`|`SWITCH`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SwitchInteger`|`SWITCH(Int)`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SwitchString`|`SWITCH(String)`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_MacroInstance`|`MACRO`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_DynamicCast`|`CAST`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Knot`|`REROUTE`（不输出到节点列表，仅用于链路穿透）|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_FunctionEntry`|`FUNC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_ENTRY`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_FunctionResult`|`FUNC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_RESULT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_MakeArray`|`MAKE\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_ARRAY`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_MakeStruct`|`MAKE\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_STRUCT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_BreakStruct`|`BREAK\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_STRUCT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Select`|`SELECT`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SpawnActorFromClass`|`SPAWN\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_ACTOR`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Timeline`|`TIMELINE`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Delay`|`DELAY`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_ForEachLoop`|`FOR\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EACH`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_CommutativeAssociativeBinaryOperator`|`OPERATOR`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_PromotableOperator`|`OPERATOR`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Composite`|`COLLAPSED`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Tunnel`|`TUNNEL`|
|其他|去掉 `K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_` 前缀|

### 5.4 节点属性输出规则

|节点类型|属性键|内容|
|-|-|-|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Event`|`Event`|事件函数名；覆写时额外一行 `(Override)`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_CustomEvent`|`Event`|自定义事件名|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_ComponentBoundEvent`|`ComponentProperty` / `DelegateProperty`|组件属性名和委托名|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_CallFunction`|`Function`|函数名；非 Self 上下文时前缀 `ClassName::FuncName`|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_VariableGet/Set`|`Variable`|变量名|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_DynamicCast`|`CastTo`|目标类名（去掉 `\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_C` 后缀）|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_MacroInstance`|`Macro`|宏图名称|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Timeline`|`Timeline`|Timeline 名称|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SwitchEnum`|`Enum`|枚举类名|
|`K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Composite`|`Collapsed`|内嵌子图名称（子图内容以缩进形式展开输出）|

### 5.5 Pin 过滤规则

以下 Pin 不输出：

1. `bIsHidden == true` 且名称为 `Target` 或 `self`
2. 名称为 `Output\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Get` 且无连接
3. 所属节点为 `K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Knot`（Reroute 节点的所有 Pin）
4. `exec` 类型的 **Input** Pin（连接关系已由 Output 端描述）
5. 无连接的 `exec` Output Pin，且名称为 `execute`、`then`、`OutputDelegate` 之一
6. `DefaultValue` 以 `Default\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_` 开头且无连接（引擎内部 Pin）
7. `SubType == LatentActionInfo` 且无连接
8. `category == delegate` 且无连接
9. `bIsHidden == true` 且无连接且无默认值
10. Output Pin 且非 exec 且无连接且默认值为空或平凡值

### 5.6 Execution Flow 和 Data Flow

**Execution Flow** 格式：

```
  {SourceNode} --> {TargetNode}            ← 走 "then" 或 "OutputPin"
  {SourceNode} \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\[TrueBranch] --> {TargetNode}  ← 其他 exec 输出 Pin 带标签
```

**Data Flow** 格式：

```
  {SourceNode}.{SourcePin} --> {TargetNode}.{TargetPin}
```

两者都会自动穿透 `K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Knot`（Reroute）链，显示最终目标节点。

### 5.7 节点排序

每个图表内节点按**拓扑顺序**排列（Kahn 算法，依据 exec 连接构建有向图）：

* 入度为 0 的节点（事件节点）优先
* 同层节点中，包含 `Event` 的节点在前，其余按节点名字母序
* 无法进入拓扑序的节点（无 exec 连接，如纯数据节点）追加到末尾

---

## 6\. 增量检测机制

`Export All Blueprints to Cache` 和自动导出均使用**双层增量检测**，大幅减少无效 IO：

### 第一层：时间戳比对（快速路径）

```
if \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_summary.txt 存在:
    ExportTimestamp = \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_summary.txt 的修改时间
    UassetTimestamp = .uasset 文件的磁盘修改时间
    if ExportTimestamp >= UassetTimestamp:
        → 跳过，无需加载蓝图资产
```

此层不加载 `UBlueprint`，只读取文件系统时间戳，非常低成本。

### 第二层：内容字符串比对（精确去重）

在 `ExportBlueprintToCache()` 内，每个文件写入前调用 `WriteFileIfChanged()`：

```cpp
bool WriteFileIfChanged(FilePath, NewContent):
    if 文件存在:
        LoadFileToString(ExistingContent)
        if ExistingContent == NewContent:
            return false  // 跳过写入，不更新文件时间戳
    SaveStringToFile(NewContent)
    return true
```

**关键语义**：`\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_summary.txt` 只有在内容实际变化时才写入。这保持了其时间戳的语义准确性，使第一层时间戳检测在下次全量导出时仍然有效。

### 日志示例

```
LogBlueprintExporter: ExportAll complete: 3 exported, 47 skipped (unchanged), 5 filtered out, 55 total assets
LogBlueprintExporter: Exported AC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyAI: 3 file(s) updated
LogBlueprintExporter: Verbose: Exported BP\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SomeActor: no changes detected, all files up-to-date
```

### 过期目录清理

全量导出完成后，`CleanupStaleExports()` 删除缓存目录中已不存在对应蓝图资产的子目录，保持缓存整洁。

---

## 7\. 技术架构

### 7.1 模块结构

```
BlueprintExporter/
├── BlueprintExporter.uplugin
└── Source/BlueprintExporter/
    ├── BlueprintExporter.Build.cs
    ├── Public/
    │   ├── BlueprintExporterModule.h     ← 模块入口，菜单注册，事件绑定
    │   ├── BlueprintExporterSettings.h   ← UDeveloperSettings 子类，Project Settings 配置
    │   ├── BlueprintExporterTypes.h      ← 中间数据结构定义（POD structs，无 UObject）
    │   ├── BlueprintGraphExtractor.h     ← UBlueprint → FExportedBlueprint 提取层
    │   └── BlueprintTextFormatter.h      ← FExportedBlueprint → FString 格式化层
    └── Private/
        ├── BlueprintExporterModule.cpp
        ├── BlueprintExporterSettings.cpp
        ├── BlueprintGraphExtractor.cpp
        └── BlueprintTextFormatter.cpp
```

### 7.2 数据流

```
UBlueprint (UE 内存)
    │
    ▼ FBlueprintGraphExtractor::Extract()
FExportedBlueprint (中间 POD 结构)
    │
    ├─▶ FBlueprintTextFormatter::Format()          → 完整单文件文本
    ├─▶ FBlueprintTextFormatter::FormatSummary()   → \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_summary.txt 内容
    ├─▶ FBlueprintTextFormatter::FormatGraphOnly() → {GraphName}.txt 内容
    └─▶ FBlueprintTextFormatter::FormatSelectedNodes() → 选中节点文本
```

### 7.3 模块依赖（Build.cs）

```csharp
PublicDependencyModuleNames:
  Core, CoreUObject, Engine, UnrealEd,
  BlueprintGraph, Kismet, KismetCompiler, GraphEditor,
  ToolMenus, ContentBrowser, DesktopPlatform, AssetTools

PrivateDependencyModuleNames:
  ApplicationCore, AssetRegistry, DeveloperSettings
```

### 7.4 菜单注册方式

* **Content Browser 菜单**：`UToolMenus::ExtendMenu("ContentBrowser.AssetContextMenu.Blueprint")`
* **蓝图编辑器节点右键菜单**：`FGraphEditorModule::GetAllGraphEditorContextMenuExtender()` 数组注册，在 `"EdGraphSchemaNodeActions"` 之后插入扩展段

> 注意：节点右键菜单\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\*\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\*不能\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\*\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\*用 `UToolMenus` 动态段注册——这是 v5 踩过的坑。正确方式是向 `FGraphEditorModule` 的 Extender 数组注册 `FGraphEditorMenuExtender\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_SelectedNode` Lambda。

* **自动加载**：通过 `FModuleManager::OnModulesChanged()` 委托监听 `GraphEditor` 模块加载，确保 GraphEditor 晚于 BlueprintExporter 加载时也能正确注册。

### 7.5 自动导出委托绑定

```cpp
// 保存时触发
PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(...)

// 关闭编辑器时触发（此时资产已全部保存，但编辑器即将退出）
PreExitHandle = FEditorDelegates::OnShutdownPostPackagesSaved.AddRaw(...)
```

---

## 8\. 数据结构参考

```cpp
// BlueprintExporterTypes.h

struct FExportedPin
{
    FString Name;           // 显示名（优先 GetDisplayName()，否则 PinName）
    FString Direction;      // "Input" | "Output"
    FString Category;       // exec | bool | int | real | object | struct | byte | ...
    FString SubType;        // 具体子类型：Character, Vector, E\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_AI\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_State, float, double
    FString ContainerType;  // "Array" | "Set" | "Map" | ""
    FString DefaultValue;
    bool bIsHidden = false;
    TArray<TPair<FString, FString>> LinkedTo;  // (TargetNodeName, TargetPinDisplayName)
};

struct FExportedVariable
{
    FString Name;
    FString Type;
    FString ContainerType;
    FString DefaultValue;
    TArray<FString> Flags;  // "EditAnywhere", "BlueprintReadWrite", "Replicated", ...
};

struct FExportedNode
{
    FString NodeName;       // 对象名，如 "K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Event\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_0"
    FString NodeClass;      // 类名，如 "K2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Event"
    FString GraphName;
    TArray<TPair<FString, FString>> Properties;  // 有序键值对（保留插入顺序）
    TArray<FExportedPin> Pins;
    TSharedPtr<FExportedGraph> SubGraph;  // Composite（折叠）节点的展开子图
};

struct FExportedGraph
{
    FString GraphName;      // "EventGraph", "SetCurrentState", ...
    FString GraphType;      // "EventGraph" | "Function" | "Macro" | "Interface" |
                            // "Collapsed" | "Selection"
    TArray<FExportedNode> Nodes;
};

struct FExportedBlueprint
{
    FString BlueprintName;
    FString ParentClass;    // 去掉 \\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_C 后缀
    TArray<FExportedVariable> Variables;
    TArray<FExportedGraph> Graphs;
};
```

---

## 9\. 注意事项

### 9.1 \_C 后缀处理

蓝图生成的类名（`UBlueprintGeneratedClass`）以 `\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_C` 结尾（如 `AC\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_EnemyAI\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_C`）。提取器在以下位置统一去掉该后缀：

* 蓝图 `ParentClass` 名称
* Pin 的 `SubType`（`PinSubCategoryObject`）
* 动态转型目标类名（`UK2Node\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_DynamicCast::TargetType`）
* Pin 的 `DefaultValue`（class/object 类型）
* `DefaultObject` 名称

### 9.2 枚举显示名

枚举变量默认值和枚举 Pin 默认值会经过 `UEnum::GetDisplayNameTextByValue()` 转换为人类可读的显示名（如 `NewEnumerator0` → `Idle`）。如果转换失败则保留原始枚举项名。

CDO 导出的枚举字符串（格式 `E\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_AI\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_State::NewEnumerator0`）在读取 CDO 属性时额外做了双冒号后半段提取再转换。

### 9.3 Comment 节点

`UEdGraphNode\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\_Comment` 节点在提取阶段直接跳过，不参与输出。

### 9.4 Reroute 节点（K2Node\_Knot）

* 节点列表中不渲染 Reroute 节点
* Execution Flow 和 Data Flow 会自动穿透 Reroute 链，显示链条末端的真实目标节点
* `ResolveRerouteChain()` 带循环检测（`TSet<FString> Visited`），防止环形 Reroute 导致无限递归

### 9.5 编译错误蓝图

蓝图处于编译错误状态时，图表和节点仍可遍历，部分连接可能不完整。提取器使用空指针守卫处理，不会因单个节点异常中断整个导出。

### 9.6 全量导出性能

`SearchAllAssets(bSynchronousSearch=true)` 在首次调用时会阻塞。后续调用（资产注册表已建好）很快。双层增量检测在大项目中可节省 90%+ 的 `GetAsset()` 调用开销。

### 9.7 缓存目录不纳入版本控制

`Saved/BlueprintExports/` 目录通常应加入 `.gitignore`，属于本地临时缓存，不需要提交到版本库。

### 9.8 Claude Code接入

建议在执行完`/Init`命令后，在项目的Claude.md中插入如下说明：



## 蓝图参考

本项目接入了蓝图导出器（BlueprintExporter）插件，默认蓝图逻辑的文本导出位于 Saved/BlueprintExports/。
如果用户设置了AutoExport，则可以认为该文件夹下的蓝图逻辑与项目最新的一致。

* 先读 \_index.txt 了解有哪些蓝图
* 需要某个蓝图的细节时，先读其 \_summary.txt（变量和 Graph 列表）
* 只在需要具体逻辑时才读单个 Graph 文件
* 不要一次性读取整个蓝图的完整导出

## 蓝图导出分析规范

分析 `Saved/BlueprintExports/` 中的蓝图导出时，遵循以下规则：

### 阅读顺序
  1. 先读 `_index.txt` 了解有哪些蓝图
  2. 需要某个蓝图的细节时，先读其 `_summary.txt`（变量和 Graph 列表）
  3. 只在需要具体逻辑时才读单个 Graph 文件
  4. 不要一次性读取整个蓝图的完整导出

  ### 分析折叠节点（COLLAPSED）时的注意事项
  读懂折叠节点的**内部内容**还不够，必须同时确认它在父图中的**执行位置**，
  否则容易对触发时机做出错误的逻辑推断。

  操作步骤：
  1. 读取折叠节点内容后，grep 父图文件中的 `=== Execution Flow ===` 部分
  2. 找到 `ExecutionSequence` 中该节点的 `Then N` 位置，确认前后顺序
  3. 再对节点的行为做结论
