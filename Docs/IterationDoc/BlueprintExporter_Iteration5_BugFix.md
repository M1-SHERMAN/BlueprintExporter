# BlueprintExporter — 第五次迭代：右键菜单不显示 Bug 修复

## Bug 描述

第四次迭代新增的"选中节点导出"功能无法使用：在蓝图编辑器中右键选中的节点时，**看不到** "Copy Selected Nodes as Text" 和 "Export Selected Nodes to File..." 菜单项。

---

## 问题代码定位

文件：`BlueprintExporterModule.cpp`，第 68-145 行

```cpp
// Graph context menu extension for selected nodes
UToolMenu* GraphMenu = UToolMenus::Get()->ExtendMenu(
    "GraphEditor.GraphContextMenu.EdGraphSchema_K2");  // ← 菜单路径可能不正确
GraphMenu->AddDynamicSection(
    "BlueprintExporterSelectedNodes",
    FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InMenu)
    {
        const UGraphNodeContextMenuContext* GraphContext =
            InMenu->FindContext<UGraphNodeContextMenuContext>();
        if (!GraphContext || !GraphContext->Graph)  // ← 可能在此处提前返回
        {
            return;
        }
        // ... 后续菜单项注册
    })
);
```

---

## 可能原因分析（按可能性排序）

### 原因 1（最可能）：菜单路径名不正确

当前注册的菜单路径：
```
"GraphEditor.GraphContextMenu.EdGraphSchema_K2"
```

UE5 蓝图编辑器中存在**两类**右键菜单：
- **节点右键菜单**（右键点击节点）：通常路径包含 `NodeContextMenu` 或 `GraphNodeContextMenu`
- **空白区域右键菜单**（右键点击空白处）：通常路径包含 `GraphContextMenu`

`GraphContextMenu` 很可能对应的是空白区域菜单，而非节点上的菜单。节点右键菜单的正确路径可能是以下之一：
- `"GraphEditor.GraphNodeContextMenu.EdGraphSchema_K2"`
- `"GraphEditor.GraphNodeContextMenu"`
- 其他 UE5.6 特有路径

#### 诊断方法

在 `RegisterMenus()` 末尾添加调试代码，枚举所有已注册菜单名：

```cpp
// === DEBUG: 输出所有已注册的菜单名到 Output Log ===
UE_LOG(LogTemp, Warning, TEXT("=== BlueprintExporter: Enumerating all registered menus ==="));
UToolMenus::Get()->ForEachMenu([](UToolMenu* Menu)
{
    // 只输出包含 "Graph" 关键字的菜单名
    FString MenuName = Menu->GetMenuName().ToString();
    if (MenuName.Contains(TEXT("Graph")))
    {
        UE_LOG(LogTemp, Warning, TEXT("  Menu: %s"), *MenuName);
    }
});
UE_LOG(LogTemp, Warning, TEXT("=== End menu enumeration ==="));
```

在 Output Log 中搜索输出，找到包含 `Node` 或 `Context` 关键字的路径。

#### 修复方向

找到正确的菜单路径后，替换 `ExtendMenu` 的参数：

```cpp
UToolMenu* GraphMenu = UToolMenus::Get()->ExtendMenu(
    "<正确的菜单路径名>");  // 根据枚举结果替换
```

---

### 原因 2：UGraphNodeContextMenuContext 为 null

如果菜单路径本身能匹配到（Section 的 lambda 被调用了），但 `FindContext<UGraphNodeContextMenuContext>()` 返回 null，则整个 section 会因 `return` 提前退出，菜单项不会被添加。

这可能发生在：
- 菜单路径对了但 Context 类型不对（不同菜单携带不同的 Context 对象）
- UE5.6 中该 Context 类的名称或继承层次有变化

#### 诊断方法

在 lambda 最开头添加日志：

```cpp
GraphMenu->AddDynamicSection(
    "BlueprintExporterSelectedNodes",
    FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InMenu)
    {
        // === DEBUG ===
        UE_LOG(LogTemp, Warning, TEXT("BlueprintExporter: DynamicSection lambda called for menu: %s"),
            *InMenu->GetMenuName().ToString());

        const UGraphNodeContextMenuContext* GraphContext =
            InMenu->FindContext<UGraphNodeContextMenuContext>();

        UE_LOG(LogTemp, Warning, TEXT("BlueprintExporter: GraphContext = %s, Graph = %s"),
            GraphContext ? TEXT("valid") : TEXT("NULL"),
            (GraphContext && GraphContext->Graph) ? TEXT("valid") : TEXT("NULL"));

        if (!GraphContext || !GraphContext->Graph)
        {
            UE_LOG(LogTemp, Warning, TEXT("BlueprintExporter: Early return — no context or graph"));
            return;
        }
        // ... 后续代码
    })
);
```

- 如果 **lambda 未被调用**：说明菜单路径不匹配（原因 1）
- 如果 **lambda 被调用但 GraphContext 为 NULL**：需要查看该菜单实际携带的 Context 类型，可能需要换用其他 Context 类

#### 替代 Context 类

如果 `UGraphNodeContextMenuContext` 不可用，可尝试：
- 直接从 `InMenu` 的其他 Context 中获取 Graph
- 使用 `FGraphEditorModule` 提供的 API

---

### 原因 3：GetEditorName() 匹配失败

第 97 行：
```cpp
if (EditorInstance && EditorInstance->GetEditorName() == FName("BlueprintEditor"))
```

如果 UE 5.6 中蓝图编辑器的 `GetEditorName()` 返回了不同的名称（如 `"Kismet"` 或 `"BlueprintEditorApp"`），则 `bHasSelection` 始终为 false。

**但注意**：即使 `bHasSelection = false`，菜单项应该仍会出现（只是灰显/Disabled），除非整个 section 因为原因 1 或原因 2 没有被添加。所以这个原因**不太可能导致菜单项完全不出现**，但可能导致菜单项灰显无法点击。

#### 诊断方法

```cpp
if (EditorInstance)
{
    UE_LOG(LogTemp, Warning, TEXT("BlueprintExporter: EditorName = '%s'"),
        *EditorInstance->GetEditorName().ToString());
}
```

---

### 原因 4：UToolMenus 注册时序问题（低可能性）

`ExtendMenu` 在目标菜单尚未注册时会预创建占位，UToolMenus 框架会在实际注册时自动合并。所以时序问题的可能性较低，但在某些 UE 版本中可能存在 edge case。

---

## 修复实施步骤

### Step 1：添加诊断日志

在 `BlueprintExporterModule.cpp` 的 `RegisterMenus()` 中添加以下诊断代码：

```cpp
void FBlueprintExporterModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);

    // ... 现有的 Content Browser 菜单注册（不变）...

    // ============================================================
    // DEBUG: 枚举所有包含 "Graph" 的菜单路径
    // ============================================================
    UE_LOG(LogTemp, Warning, TEXT("=== BlueprintExporter DEBUG: Registered Graph-related menus ==="));
    UToolMenus::Get()->ForEachMenu([](UToolMenu* Menu)
    {
        FString MenuName = Menu->GetMenuName().ToString();
        if (MenuName.Contains(TEXT("Graph")) || MenuName.Contains(TEXT("Node")))
        {
            UE_LOG(LogTemp, Warning, TEXT("  [MENU] %s"), *MenuName);
        }
    });
    UE_LOG(LogTemp, Warning, TEXT("=== END DEBUG ==="));

    // Graph context menu extension for selected nodes
    UToolMenu* GraphMenu = UToolMenus::Get()->ExtendMenu(
        "GraphEditor.GraphContextMenu.EdGraphSchema_K2");
    GraphMenu->AddDynamicSection(
        "BlueprintExporterSelectedNodes",
        FNewToolMenuDelegate::CreateLambda([this](UToolMenu* InMenu)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("BlueprintExporter: DynamicSection ENTERED for: %s"),
                *InMenu->GetMenuName().ToString());

            const UGraphNodeContextMenuContext* GraphContext =
                InMenu->FindContext<UGraphNodeContextMenuContext>();

            UE_LOG(LogTemp, Warning,
                TEXT("BlueprintExporter: GraphContext=%s Graph=%s"),
                GraphContext ? TEXT("OK") : TEXT("NULL"),
                (GraphContext && GraphContext->Graph) ? TEXT("OK") : TEXT("NULL"));

            if (!GraphContext || !GraphContext->Graph)
            {
                return;
            }

            // ... 后续不变 ...
        })
    );
}
```

### Step 2：编译并测试

1. 编译插件
2. 打开任意蓝图，选中若干节点，右键
3. 查看 Output Log（过滤 `LogTemp`）：
   - 如果没有看到 `DynamicSection ENTERED`：菜单路径名不对 → 查看枚举输出找到正确路径
   - 如果看到 `ENTERED` 但 `GraphContext=NULL`：Context 类型不对
   - 如果看到 `GraphContext=OK` 和 `Graph=OK`：问题在后续逻辑

### Step 3：根据诊断结果修复

#### 情况 A：菜单路径名不对

从 Step 1 的枚举输出中找到正确的节点右键菜单路径，替换 `ExtendMenu` 参数。

常见的 UE5 蓝图节点右键菜单路径候选：
- `"GraphEditor.GraphNodeContextMenu.EdGraphSchema_K2"`（注意 `Node` 的位置）
- `"GraphEditor.GraphNodeContextMenu"`
- 也可能是某个带有 K2 Schema 后缀的变体

#### 情况 B：Context 类型不对

如果菜单路径对了但 Context 不可用，可以尝试以下替代方案来获取 Graph：

```cpp
// 替代方案：不依赖 UGraphNodeContextMenuContext，
// 而是直接从当前活跃的蓝图编辑器获取
UAssetEditorSubsystem* AssetEditorSub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
TArray<UObject*> EditedAssets = AssetEditorSub->GetAllEditedAssets();
for (UObject* Asset : EditedAssets)
{
    UBlueprint* BP = Cast<UBlueprint>(Asset);
    if (!BP) continue;

    IAssetEditorInstance* Editor = AssetEditorSub->FindEditorForAsset(BP, false);
    if (!Editor || Editor->GetEditorName() != FName("BlueprintEditor")) continue;

    FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(Editor);
    if (BPEditor->GetSelectedNodes().Num() > 0)
    {
        // 找到了有选中节点的编辑器
        const UEdGraph* FocusedGraph = BPEditor->GetFocusedGraph();
        // ... 使用 FocusedGraph 继续
    }
}
```

### Step 4：清理诊断代码

修复确认后，移除所有 `UE_LOG` 调试语句和菜单枚举代码。

---

## 涉及文件

| 文件 | 修改类型 |
|------|---------|
| `Private/BlueprintExporterModule.cpp` | 修改菜单注册路径 / Context 获取逻辑 |

其他文件（Extractor、Formatter、Types、Build.cs）**不需要修改**，它们在第四次迭代中的实现已经验证正确。

---

## 验证方法

1. 打开任意蓝图的 EventGraph
2. 框选 2-3 个节点
3. 右键 → 确认看到 "Blueprint Exporter" section 下的两个菜单项
4. 点击 "Copy Selected Nodes as Text" → 粘贴到文本编辑器确认内容正确
5. 点击 "Export Selected Nodes to File..." → 确认弹出保存对话框
6. 不选中任何节点 → 右键 → 确认两个菜单项为灰显状态
7. 在 Collapsed 子图编辑器中重复测试 2-6

---

## 附录：当前完整代码文件清单

以下为插件当前的所有源文件，供排查时参考：

```
BlueprintExporter/
├── BlueprintExporter.Build.cs
└── Source/BlueprintExporter/
    ├── Public/
    │   ├── BlueprintExporterModule.h
    │   ├── BlueprintExporterTypes.h
    │   ├── BlueprintGraphExtractor.h
    │   └── BlueprintTextFormatter.h
    └── Private/
        ├── BlueprintExporterModule.cpp    ← 本次修改目标
        ├── BlueprintGraphExtractor.cpp
        └── BlueprintTextFormatter.cpp
```
