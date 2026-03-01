# BlueprintExporter 插件 — 第六次迭代需求

## 来源

- ClaudeCode 协作工作流需求：编辑器关闭状态下，AI 需要直接读取蓝图逻辑
- 前五次迭代完成了手动导出功能的开发，本次迭代将导出流程自动化

---

## 目标

实现**蓝图自动导出 + 类型过滤 + 分层索引**机制。使蓝图逻辑文本始终与编辑器中的蓝图保持同步，存放在项目内固定目录中。ClaudeCode 在编辑器关闭的情况下可以直接读取这些文件，无需人工介入。

### 核心工作流

```
编辑器运行中：
  蓝图 Ctrl+S → 自动增量导出该蓝图 → 写入 Saved/BlueprintExports/

编辑器关闭后：
  ClaudeCode → cat Saved/BlueprintExports/_index.txt → 了解项目蓝图概况
            → cat Saved/BlueprintExports/AC_EnemyAI/_summary.txt → 了解该蓝图结构
            → cat Saved/BlueprintExports/AC_EnemyAI/Investigate.txt → 读取特定函数逻辑
```

### 不做的事

- 不做运行时实时查询（Commandlet / TCP 通信等方案，编辑器关闭时无法使用）
- 不做 MCP Server 集成（后续可扩展）
- 不做 C++ 源文件的自动生成（导出结果供 AI 辅助转写，不做自动转换）

---

## P0：自动导出机制

### 1. 蓝图保存时自动增量导出

**问题**：当前导出必须由用户手动触发（Content Browser 右键或蓝图编辑器右键）。开发过程中频繁修改蓝图后容易忘记导出，导致 ClaudeCode 读到的是过时内容。

**方案**：注册 `UPackage::PackageSavedWithContextEvent` 回调，每当包含蓝图资产的 Package 被保存时，自动对该蓝图执行导出。

```cpp
void FBlueprintExporterModule::StartupModule()
{
    // ... 现有初始化 ...
    
    PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(
        this, &FBlueprintExporterModule::OnPackageSaved);
}

void FBlueprintExporterModule::OnPackageSaved(
    const FString& PackageFilename,
    UPackage* Package,
    FObjectPostSaveContext Context)
{
    UBlueprint* Blueprint = nullptr;
    ForEachObjectWithPackage(Package, [&](UObject* Obj)
    {
        if (UBlueprint* BP = Cast<UBlueprint>(Obj))
        {
            Blueprint = BP;
            return false;
        }
        return true;
    });
    
    if (!Blueprint || !ShouldExport(Blueprint))
        return;
    
    const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
    if (!Settings->bAutoExportOnSave)
        return;
    
    ExportBlueprintToCache(Blueprint);
}
```

导出结果写入 `{ProjectDir}/Saved/BlueprintExports/` 下的固定目录结构（详见需求 #4）。

**回调清理**：在 `ShutdownModule()` 中移除回调，防止悬挂引用：

```cpp
void FBlueprintExporterModule::ShutdownModule()
{
    UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
    // ... 现有清理 ...
}
```

**文件**：
- `BlueprintExporterModule.h`（新增 `FDelegateHandle PackageSavedHandle` 成员、`OnPackageSaved` 和 `ExportBlueprintToCache` 方法声明）
- `BlueprintExporterModule.cpp`（实现回调注册、`OnPackageSaved`、`ExportBlueprintToCache`）

---

### 2. 编辑器关闭前全量刷新

**问题**：仅靠保存时触发，无法覆盖以下场景：
- 蓝图被其他方式修改（如 Blueprint Merge、版本控制还原）
- 插件首次启用时，已有蓝图从未被导出过

**方案**：注册编辑器关闭回调（`FCoreDelegates::OnPreExit` 或 `FEditorDelegates::OnShutdownPostPackagesSaved`），在编辑器退出前对所有符合过滤条件的蓝图执行一次全量扫描导出。

```cpp
// 在 StartupModule() 中注册
PreExitHandle = FCoreDelegates::OnPreExit.AddRaw(
    this, &FBlueprintExporterModule::OnEditorPreExit);
```

全量扫描逻辑：使用 `AssetRegistry` 遍历项目中所有 `UBlueprint` 类型资产，对每个通过过滤条件的蓝图调用 `ExportBlueprintToCache`。

```cpp
void FBlueprintExporterModule::ExportAllBlueprints()
{
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    
    TArray<FAssetData> BlueprintAssets;
    AssetRegistry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), BlueprintAssets, true);
    
    const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
    int32 ExportedCount = 0;
    
    for (const FAssetData& AssetData : BlueprintAssets)
    {
        UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
        if (BP && ShouldExport(BP, Settings))
        {
            ExportBlueprintToCache(BP);
            ExportedCount++;
        }
    }
    
    // 导出完成后刷新索引文件
    GenerateIndexFile();
    
    UE_LOG(LogBlueprintExporter, Log,
        TEXT("Auto-export complete: %d blueprints exported to Saved/BlueprintExports/"),
        ExportedCount);
}
```

**注意**：全量扫描可能涉及加载未在内存中的蓝图资产。对于大型项目，这个过程可能需要数秒到数十秒。由于是编辑器关闭前执行，用户感知不明显，可接受。`OnEditorPreExit` 内部需先检查 `Settings->bExportOnEditorClose`，若为 false 则直接返回。

**补充入口**：同时提供一个手动触发全量导出的菜单项（Content Browser 或 Tools 菜单），用于首次安装插件时或用户希望手动刷新的场景。

**文件**：
- `BlueprintExporterModule.h`（新增 `OnEditorPreExit`、`ExportAllBlueprints`、`GenerateIndexFile` 声明）
- `BlueprintExporterModule.cpp`（实现）

---

## P1：蓝图类型过滤

### 3. 可配置的导出过滤器

**问题**：项目中不是所有蓝图都包含需要导出的逻辑。DataTable、Material、纯数据蓝图、Widget 蓝图、动画蓝图等可能不需要导出。全量导出会浪费磁盘空间和 AI 阅读时间。

**方案**：新增 `UBlueprintExporterSettings` 配置类（继承 `UDeveloperSettings`），在 Project Settings 面板中暴露过滤选项。

#### 3a. 配置类定义

```cpp
// BlueprintExporterSettings.h
UCLASS(config=EditorPerProjectUserSettings, defaultconfig, meta=(DisplayName="Blueprint Exporter"))
class UBlueprintExporterSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UBlueprintExporterSettings();
    
    // --- 自动导出开关 ---
    
    /** 是否启用保存时自动导出（默认关闭，需在 Project Settings 中手动启用） */
    UPROPERTY(Config, EditAnywhere, Category="Auto Export")
    bool bAutoExportOnSave = false;
    
    /** 是否在编辑器关闭前执行全量刷新（默认关闭，需在 Project Settings 中手动启用） */
    UPROPERTY(Config, EditAnywhere, Category="Auto Export")
    bool bExportOnEditorClose = false;
    
    // --- BlueprintType 过滤 ---
    
    /** 导出标准蓝图类（Actor, Component, Character 等） */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
    bool bExportNormalBlueprint = true;
    
    /** 导出函数库蓝图 */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
    bool bExportFunctionLibrary = true;
    
    /** 导出宏库蓝图 */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
    bool bExportMacroLibrary = false;
    
    /** 导出接口蓝图 */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
    bool bExportInterface = false;
    
    /** 导出关卡蓝图 */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
    bool bExportLevelScript = false;
    
    // --- 父类过滤 ---
    
    /** 父类白名单（空 = 不限制，接受所有父类）
     *  填入如 Actor, ActorComponent, Character 等的类路径
     *  蓝图的父类必须是列表中某一项的子类才会被导出 */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Parent Class", meta=(AllowAbstract="true"))
    TArray<FSoftClassPath> ParentClassFilter;
    
    /** 父类黑名单（优先于白名单）
     *  填入如 UserWidget, AnimInstance 等不需要导出的父类 */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Parent Class", meta=(AllowAbstract="true"))
    TArray<FSoftClassPath> ExcludedParentClasses;
    
    // --- 内容过滤 ---
    
    /** 最少逻辑节点数阈值。低于此数的蓝图视为无实质逻辑，跳过导出。
     *  默认 2（空 EventGraph 通常只有 0-1 个默认节点） */
    UPROPERTY(Config, EditAnywhere, Category="Export Filter|Content", meta=(ClampMin=1, ClampMax=100))
    int32 MinNodeCount = 2;
};
```

使用 `UDeveloperSettings` 的好处：自动在 **Project Settings → Plugins → Blueprint Exporter** 中生成 UI 面板，用户可以直接在编辑器内勾选配置，无需编辑配置文件。设置保存在 `Saved/Config/` 下（`EditorPerProjectUserSettings`），不进版本控制。

#### 3b. 过滤判断逻辑

```cpp
bool FBlueprintExporterModule::ShouldExport(UBlueprint* BP, const UBlueprintExporterSettings* Settings)
{
    if (!BP) return false;
    
    // 1. BlueprintType 过滤
    switch (BP->BlueprintType)
    {
        case BPTYPE_Normal:          if (!Settings->bExportNormalBlueprint) return false; break;
        case BPTYPE_FunctionLibrary: if (!Settings->bExportFunctionLibrary) return false; break;
        case BPTYPE_MacroLibrary:    if (!Settings->bExportMacroLibrary) return false; break;
        case BPTYPE_Interface:       if (!Settings->bExportInterface) return false; break;
        case BPTYPE_LevelScript:     if (!Settings->bExportLevelScript) return false; break;
        default: return false;
    }
    
    // 2. 父类黑名单（优先）
    if (BP->ParentClass && Settings->ExcludedParentClasses.Num() > 0)
    {
        for (const FSoftClassPath& Path : Settings->ExcludedParentClasses)
        {
            UClass* ExcludedClass = Path.TryLoadClass<UObject>();
            if (ExcludedClass && BP->ParentClass->IsChildOf(ExcludedClass))
                return false;
        }
    }
    
    // 3. 父类白名单（空 = 不限制）
    if (BP->ParentClass && Settings->ParentClassFilter.Num() > 0)
    {
        bool bMatchesAny = false;
        for (const FSoftClassPath& Path : Settings->ParentClassFilter)
        {
            UClass* FilterClass = Path.TryLoadClass<UObject>();
            if (FilterClass && BP->ParentClass->IsChildOf(FilterClass))
            {
                bMatchesAny = true;
                break;
            }
        }
        if (!bMatchesAny) return false;
    }
    
    // 4. 最低节点数阈值
    int32 TotalNodes = 0;
    for (UEdGraph* G : BP->UbergraphPages)  TotalNodes += G ? G->Nodes.Num() : 0;
    for (UEdGraph* G : BP->FunctionGraphs)  TotalNodes += G ? G->Nodes.Num() : 0;
    for (UEdGraph* G : BP->MacroGraphs)     TotalNodes += G ? G->Nodes.Num() : 0;
    
    return TotalNodes >= Settings->MinNodeCount;
}
```

**文件**：
- `BlueprintExporterSettings.h`（新建，配置类定义）
- `BlueprintExporterSettings.cpp`（新建，构造函数 + `GetSectionName` 等）
- `BlueprintExporterModule.cpp`（`ShouldExport` 实现 + 各处调用）
- `BlueprintExporter.Build.cs`（可能需要新增 `DeveloperSettings` 相关模块依赖）

---

## P1：分层索引输出

### 4. 分层目录结构 + 索引文件

**问题**：单蓝图的完整导出可能有数千行（如 AC_EnemyAI 约 6000 行）。如果 ClaudeCode 为了了解一个函数的逻辑就读取整个文件，会造成巨大的 token 浪费。

**方案**：将导出结果拆分为分层目录结构，支持 ClaudeCode 按需逐级深入。

#### 4a. 目录结构

```
{ProjectDir}/Saved/BlueprintExports/
├── _index.txt                          # 全局索引：所有蓝图的概况一览
├── AC_EnemyAI/
│   ├── _summary.txt                    # 蓝图摘要：变量表 + Graph 列表
│   ├── EventGraph.txt                  # 单 Graph 的完整导出
│   ├── SetCurrentState.txt
│   ├── Investigate.txt
│   ├── AttackLogic.txt                 # Collapsed 子图也作为独立文件
│   └── ...
├── BP_EnemyCharacter/
│   ├── _summary.txt
│   ├── EventGraph.txt
│   └── ...
└── AC_CombatManager/
    ├── _summary.txt
    └── ...
```

#### 4b. 全局索引文件（`_index.txt`）

由 `GenerateIndexFile()` 在全量导出后自动生成，增量导出时也更新对应条目。包含每个蓝图的名称、父类、Graph 列表、变量数量、文件行数。

格式示例：

```
=== Blueprint Export Index ===
Generated: 2026-03-01 15:30:00
Total: 12 blueprints

AC_EnemyAI (Parent: ActorComponent)
  Graphs: EventGraph, SetCurrentState, Investigate, AttackLogic, DefendLogic, ...
  Variables: 77
  Path: AC_EnemyAI/

BP_EnemyCharacter (Parent: Character)
  Graphs: EventGraph, OnDamaged, HandleDeath
  Variables: 23
  Path: BP_EnemyCharacter/

AC_CombatManager (Parent: ActorComponent)
  Graphs: EventGraph, RequestAttackSlot, ReleaseSlot
  Variables: 12
  Path: AC_CombatManager/
```

ClaudeCode 读取此文件即可了解项目中有哪些蓝图及其大致内容，成本极低（通常 < 100 行）。

#### 4c. 蓝图摘要文件（`_summary.txt`）

每个蓝图目录下的摘要文件包含：变量声明区 + 各 Graph 的名称/类型/节点数。

格式示例：

```
=== Blueprint: AC_EnemyAI (Parent: ActorComponent) ===

=== Variables ===
  CurrentAIState               : E_AI_State     = Idle
  Health                       : float          = 100.0
  AttackSlotRef                : Actor
  bIsStrafing                  : bool
  ...

=== Graphs ===
  EventGraph                   (EventGraph)      87 nodes
  SetCurrentState              (Function)         12 nodes
  Investigate                  (Function)         34 nodes
  AttackLogic                  (Collapsed)        28 nodes
  DefendLogic                  (Collapsed)        19 nodes
  ...
```

#### 4d. 单 Graph 文件

每个 Graph 导出为独立 `.txt` 文件，内容与现有 `FormatGraph` 的输出一致（节点详情 + Execution Flow + Data Flow），但不包含变量声明区和蓝图级 header。

文件名基于 Graph 名称，特殊字符替换为下划线。Collapsed 子图也作为独立文件导出。

#### 4e. 对现有导出流程的修改

当前 `ExportBlueprintToCache` 需要调整为：
1. 调用 `FBlueprintGraphExtractor::Extract` 获取 `FExportedBlueprint`（与现有逻辑一致）
2. 使用 `FBlueprintTextFormatter` 分别格式化各 Graph，每个 Graph 写入独立文件
3. 生成 `_summary.txt`（变量区 + Graph 列表）
4. 更新 `_index.txt` 中该蓝图的条目

**文件**：
- `BlueprintTextFormatter.h/cpp`（新增 `FormatGraphOnly`、`FormatSummary` 方法）
- `BlueprintExporterModule.cpp`（`ExportBlueprintToCache` 和 `GenerateIndexFile` 实现）

---

### 5. 过期文件清理

**问题**：蓝图被删除或重命名后，`Saved/BlueprintExports/` 中对应的导出文件不会自动清理，可能导致 ClaudeCode 读到已不存在的蓝图的过期内容。

**方案**：在全量导出（编辑器关闭前或手动触发）时，扫描 `BlueprintExports/` 目录下的所有子文件夹，对比当前项目中实际存在的蓝图列表，删除不再存在的蓝图对应的导出文件夹。

```cpp
void FBlueprintExporterModule::CleanupStaleExports(const TSet<FString>& ExportedBlueprintNames)
{
    FString ExportDir = FPaths::ProjectSavedDir() / TEXT("BlueprintExports");
    
    TArray<FString> ExistingDirs;
    IFileManager::Get().FindFiles(ExistingDirs, *(ExportDir / TEXT("*")), false, true);
    
    for (const FString& DirName : ExistingDirs)
    {
        if (DirName.StartsWith(TEXT("_")))  // 跳过 _index.txt 等元文件
            continue;
            
        if (!ExportedBlueprintNames.Contains(DirName))
        {
            FString FullPath = ExportDir / DirName;
            IFileManager::Get().DeleteDirectory(*FullPath, false, true);
            UE_LOG(LogBlueprintExporter, Log,
                TEXT("Cleaned up stale export: %s"), *DirName);
        }
    }
}
```

在 `ExportAllBlueprints()` 末尾调用，传入本次实际导出的蓝图名称集合。

**文件**：`BlueprintExporterModule.cpp`

---

## P2：ClaudeCode 集成指引

### 6. 自动生成 CLAUDE.md 提示

**问题**：ClaudeCode 需要知道蓝图导出文件的位置和使用方式。每个新项目或新团队成员都需要手动配置 ClaudeCode 的项目指令。

**方案**：在首次全量导出完成后，自动在 `Saved/BlueprintExports/` 目录下生成一个 `README.md` 文件，描述目录结构和使用方式。用户可以将其内容添加到项目的 `CLAUDE.md` 中。

内容示例：

```markdown
# Blueprint Exports

蓝图逻辑的文本导出，由 BlueprintExporter 插件自动生成。

## 使用方式

1. 先读 `_index.txt` 了解项目中有哪些蓝图
2. 需要某个蓝图的细节时，先读其 `_summary.txt`（变量声明 + Graph 列表）
3. 只在需要具体逻辑实现时才读单个 Graph 文件
4. 不要一次性读取所有文件，按需逐级深入

## 文件更新时机

- 蓝图在编辑器中保存时自动更新
- 编辑器关闭前自动全量刷新
- 如内容过时，需在编辑器中打开并保存对应蓝图

## 目录结构

_index.txt              — 全局索引
{BlueprintName}/
  _summary.txt          — 变量表 + Graph 列表
  {GraphName}.txt       — 单个 Graph 的完整逻辑
```

**文件**：`BlueprintExporterModule.cpp`（在 `GenerateIndexFile` 末尾生成）

---

## 新增文件清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `Public/BlueprintExporterSettings.h` | 新建 | `UDeveloperSettings` 配置类 |
| `Private/BlueprintExporterSettings.cpp` | 新建 | 配置类实现 |
| `Public/BlueprintExporterModule.h` | 修改 | 新增成员变量和方法声明 |
| `Private/BlueprintExporterModule.cpp` | 修改 | 核心逻辑：回调注册、自动导出、全量扫描、索引生成、文件清理 |
| `Public/BlueprintTextFormatter.h` | 修改 | 新增 `FormatGraphOnly`、`FormatSummary` 方法声明 |
| `Private/BlueprintTextFormatter.cpp` | 修改 | 新增方法实现 |
| `BlueprintExporter.Build.cs` | 修改 | 可能新增 `AssetRegistry`、`DeveloperSettings` 模块依赖 |

---

## 实施优先级总结

| 优先级 | 需求 | 工作量估计 |
|--------|------|-----------|
| P0 | #1 保存时自动增量导出 | 小 |
| P0 | #2 编辑器关闭前全量刷新 | 中 |
| P1 | #3 可配置的导出过滤器 | 中 |
| P1 | #4 分层索引输出 | 中 |
| P1 | #5 过期文件清理 | 小 |
| P2 | #6 ClaudeCode 集成指引 | 小 |

---

## 实施顺序建议

1. **Phase A**（基础自动化）：先实现需求 #1 + #3，使单蓝图保存时能自动导出到目标目录（此时仍为单文件，不拆分）。验证过滤器正确排除非目标蓝图。
2. **Phase B**（分层输出）：实现需求 #4，将导出结果从单文件改为分层目录结构。调整 `ExportBlueprintToCache` 的输出逻辑。
3. **Phase C**（全量 + 清理）：实现需求 #2 + #5，添加编辑器关闭前全量刷新和过期清理。
4. **Phase D**（指引）：实现需求 #6。

---

## 验证方法

### 需求 #1 + #3 验证

1. 在 Project Settings → Blueprint Exporter 中配置过滤条件（如仅导出 Normal Blueprint + ActorComponent 子类）。
2. 打开 AC_EnemyAI 蓝图，修改任意节点，Ctrl+S 保存。
3. 确认 `Saved/BlueprintExports/` 目录下出现 AC_EnemyAI 的导出文件。
4. 打开一个 Widget Blueprint，保存，确认**不会**生成导出文件。
5. 打开一个只有空 EventGraph 的蓝图（节点数 < 2），保存，确认不会生成导出文件。

### 需求 #2 验证

1. 删除 `Saved/BlueprintExports/` 目录。
2. 关闭编辑器。
3. 确认 `Saved/BlueprintExports/` 目录被重新生成，包含所有符合过滤条件的蓝图。
4. 确认 `_index.txt` 存在且内容正确。

### 需求 #4 验证

1. 对 AC_EnemyAI 触发导出。
2. 确认生成以下目录结构：
   - `AC_EnemyAI/_summary.txt` — 包含变量表和 Graph 列表
   - `AC_EnemyAI/EventGraph.txt` — 包含 EventGraph 的完整节点和 Flow
   - `AC_EnemyAI/SetCurrentState.txt` — 包含该函数的完整逻辑
3. 确认 `_summary.txt` 中的变量表与之前整体导出一致。
4. 确认单 Graph 文件中**不包含**变量声明区。
5. 确认所有 Graph 文件内容拼合后，逻辑与之前整体导出一致（无信息丢失）。

### 需求 #5 验证

1. 手动在 `Saved/BlueprintExports/` 下创建一个 `FakeBlueprint/` 目录（模拟已删除的蓝图）。
2. 触发全量导出（关闭编辑器或手动菜单）。
3. 确认 `FakeBlueprint/` 目录被清理。

### 端到端验证（模拟 ClaudeCode 工作流）

1. 在编辑器中保存若干蓝图后关闭编辑器。
2. 在终端中：
   ```bash
   cat Saved/BlueprintExports/_index.txt        # 查看全局索引
   cat Saved/BlueprintExports/AC_EnemyAI/_summary.txt  # 查看蓝图结构
   cat Saved/BlueprintExports/AC_EnemyAI/Investigate.txt  # 查看特定函数
   ```
3. 确认每一层的信息足够用于 AI 理解，且不需要读取其他层级即可回答对应粒度的问题。
