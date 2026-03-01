#include "BlueprintExporterModule.h"
#include "BlueprintGraphExtractor.h"
#include "BlueprintTextFormatter.h"
#include "BlueprintExporterSettings.h"

#include "ToolMenus.h"
#include "ToolMenuContext.h"
#include "ContentBrowserMenuContexts.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "BlueprintEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "HAL/PlatformApplicationMisc.h"
#include "GraphEditorModule.h"
#include "UObject/Package.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectHash.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintExporter, Log, All);

#define LOCTEXT_NAMESPACE "BlueprintExporter"

void FBlueprintExporterModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBlueprintExporterModule::RegisterMenus));

	ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddRaw(
		this, &FBlueprintExporterModule::OnModulesChanged);

	if (FModuleManager::Get().IsModuleLoaded(TEXT("GraphEditor")))
	{
		RegisterGraphEditorExtender();
	}

	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(
		this, &FBlueprintExporterModule::OnPackageSaved);
	PreExitHandle = FEditorDelegates::OnShutdownPostPackagesSaved.AddRaw(
		this, &FBlueprintExporterModule::OnEditorPreExit);
}

void FBlueprintExporterModule::ShutdownModule()
{
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	FEditorDelegates::OnShutdownPostPackagesSaved.Remove(PreExitHandle);

	FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);

	if (GraphEditorMenuExtenderIndex != INDEX_NONE)
	{
		if (FGraphEditorModule* GraphEditorModule =
			FModuleManager::GetModulePtr<FGraphEditorModule>(TEXT("GraphEditor")))
		{
			TArray<FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode>& Extenders =
				GraphEditorModule->GetAllGraphEditorContextMenuExtender();
			if (Extenders.IsValidIndex(GraphEditorMenuExtenderIndex))
			{
				Extenders.RemoveAt(GraphEditorMenuExtenderIndex);
			}
		}
		GraphEditorMenuExtenderIndex = INDEX_NONE;
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FBlueprintExporterModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.Blueprint");
	FToolMenuSection& Section = Menu->AddSection("BlueprintExporter",
		LOCTEXT("BlueprintExporterSection", "Blueprint Exporter"));

	Section.AddMenuEntry(
		"ExportBlueprintLogic",
		LOCTEXT("ExportLabel", "Export Blueprint Logic"),
		LOCTEXT("ExportTooltip", "Export all graphs to AI-readable plain text"),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateLambda([this](const FToolMenuContext& Context)
		{
			const UContentBrowserAssetContextMenuContext* CBContext =
				Context.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!CBContext)
			{
				return;
			}

			TArray<FAssetData> SelectedAssets;
			for (const FAssetData& AssetData : CBContext->SelectedAssets)
			{
				SelectedAssets.Add(AssetData);
			}

			if (SelectedAssets.Num() > 0)
			{
				ExportSelectedBlueprints(SelectedAssets);
			}
		})
	);

	Section.AddMenuEntry(
		"ExportAllBlueprintsToCache",
		LOCTEXT("ExportAllLabel", "Export All Blueprints to Cache"),
		LOCTEXT("ExportAllTooltip", "Scan all Blueprints and export to Saved/BlueprintExports/"),
		FSlateIcon(),
		FToolMenuExecuteAction::CreateLambda([this](const FToolMenuContext&)
		{
			ExportAllBlueprints();
		})
	);
}

void FBlueprintExporterModule::RegisterGraphEditorExtender()
{
	if (GraphEditorMenuExtenderIndex != INDEX_NONE)
	{
		return; // Already registered
	}

	FGraphEditorModule& GraphEditorModule =
		FModuleManager::GetModuleChecked<FGraphEditorModule>(TEXT("GraphEditor"));
	GraphEditorMenuExtenderIndex =
		GraphEditorModule.GetAllGraphEditorContextMenuExtender().Add(
			FGraphEditorModule::FGraphEditorMenuExtender_SelectedNode::CreateLambda(
				[this](const TSharedRef<FUICommandList>, const UEdGraph* Graph,
					   const UEdGraphNode* /*Node*/, const UEdGraphPin* /*Pin*/,
					   bool /*bIsReadOnly*/) -> TSharedRef<FExtender>
				{
					TSharedRef<FExtender> Extender = MakeShared<FExtender>();
					if (!Graph)
					{
						return Extender;
					}
					UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
					if (!BP)
					{
						return Extender;
					}

					bool bHasSelection = false;
					UAssetEditorSubsystem* Sub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
					if (Sub)
					{
						IAssetEditorInstance* Inst = Sub->FindEditorForAsset(BP, false);
						if (Inst && Inst->GetEditorName() == FName("BlueprintEditor"))
						{
							bHasSelection = static_cast<FBlueprintEditor*>(Inst)->GetSelectedNodes().Num() > 0;
						}
					}

					Extender->AddMenuExtension(
						"EdGraphSchemaNodeActions", EExtensionHook::After, nullptr,
						FMenuExtensionDelegate::CreateLambda(
							[this, Graph, bHasSelection](FMenuBuilder& MenuBuilder)
							{
								MenuBuilder.BeginSection("BlueprintExporterNodes",
									LOCTEXT("SelectedNodesSection", "Blueprint Exporter"));
								MenuBuilder.AddMenuEntry(
									LOCTEXT("CopySelectedLabel", "Copy Selected Nodes as Text"),
									bHasSelection
										? LOCTEXT("CopySelectedTooltip", "Copy selected nodes as AI-readable plain text to clipboard")
										: LOCTEXT("CopySelectedTooltipDisabled", "No nodes selected"),
									FSlateIcon(),
									FUIAction(
										FExecuteAction::CreateLambda([this, Graph]() { CopySelectedNodesToClipboard(Graph); }),
										FCanExecuteAction::CreateLambda([bHasSelection]() { return bHasSelection; })
									)
								);
								MenuBuilder.AddMenuEntry(
									LOCTEXT("ExportSelectedLabel", "Export Selected Nodes to File..."),
									bHasSelection
										? LOCTEXT("ExportSelectedTooltip", "Export selected nodes as AI-readable plain text to a file")
										: LOCTEXT("ExportSelectedTooltipDisabled", "No nodes selected"),
									FSlateIcon(),
									FUIAction(
										FExecuteAction::CreateLambda([this, Graph]() { ExportSelectedNodesToFile(Graph); }),
										FCanExecuteAction::CreateLambda([bHasSelection]() { return bHasSelection; })
									)
								);
								MenuBuilder.EndSection();
							}
						)
					);
					return Extender;
				}
			)
		);
}

void FBlueprintExporterModule::OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
{
	if (ModuleName == TEXT("GraphEditor") && Reason == EModuleChangeReason::ModuleLoaded)
	{
		RegisterGraphEditorExtender();
	}
}

void FBlueprintExporterModule::ExportSelectedBlueprints(const TArray<FAssetData>& SelectedAssets)
{
	for (const FAssetData& AssetData : SelectedAssets)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
		if (!Blueprint)
		{
			continue;
		}

		// Extract
		FBlueprintGraphExtractor Extractor;
		FExportedBlueprint ExportedBP = Extractor.Extract(Blueprint);

		// Format
		FBlueprintTextFormatter Formatter;
		FString OutputText = Formatter.Format(ExportedBP);

		// Save file dialog
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (!DesktopPlatform)
		{
			continue;
		}

		const FString DefaultFileName = FString::Printf(TEXT("%s_exported.txt"), *Blueprint->GetName());
		const FString DefaultPath = FPaths::ProjectDir();

		TArray<FString> OutFiles;
		const bool bOpened = DesktopPlatform->SaveFileDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
			TEXT("Export Blueprint Logic"),
			DefaultPath,
			DefaultFileName,
			TEXT("Text Files (*.txt)|*.txt"),
			0,
			OutFiles);

		if (bOpened && OutFiles.Num() > 0)
		{
			FFileHelper::SaveStringToFile(OutputText, *OutFiles[0],
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}
}

static bool GetSelectedNodesForGraph(
	const UEdGraph* Graph,
	TSet<UEdGraphNode*>& OutNodes,
	FString& OutBlueprintName)
{
	UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
	if (!BP)
	{
		return false;
	}

	OutBlueprintName = BP->GetName();

	UAssetEditorSubsystem* AssetEditorSubsystem =
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return false;
	}

	IAssetEditorInstance* EditorInstance =
		AssetEditorSubsystem->FindEditorForAsset(BP, false);
	if (!EditorInstance || EditorInstance->GetEditorName() != FName("BlueprintEditor"))
	{
		return false;
	}

	FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(EditorInstance);
	const FGraphPanelSelectionSet& RawSelection = BPEditor->GetSelectedNodes();
	for (UObject* Obj : RawSelection)
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Obj))
		{
			OutNodes.Add(Node);
		}
	}

	return OutNodes.Num() > 0;
}

void FBlueprintExporterModule::CopySelectedNodesToClipboard(const UEdGraph* Graph)
{
	TSet<UEdGraphNode*> SelectedNodes;
	FString BlueprintName;
	if (!GetSelectedNodesForGraph(Graph, SelectedNodes, BlueprintName))
	{
		return;
	}

	FBlueprintGraphExtractor Extractor;
	FExportedGraph ExportedGraph = Extractor.ExtractSelectedNodes(SelectedNodes, Graph);

	FBlueprintTextFormatter Formatter;
	FString OutputText = Formatter.FormatSelectedNodes(ExportedGraph, BlueprintName);

	FPlatformApplicationMisc::ClipboardCopy(*OutputText);
}

void FBlueprintExporterModule::ExportSelectedNodesToFile(const UEdGraph* Graph)
{
	TSet<UEdGraphNode*> SelectedNodes;
	FString BlueprintName;
	if (!GetSelectedNodesForGraph(Graph, SelectedNodes, BlueprintName))
	{
		return;
	}

	FBlueprintGraphExtractor Extractor;
	FExportedGraph ExportedGraph = Extractor.ExtractSelectedNodes(SelectedNodes, Graph);

	FBlueprintTextFormatter Formatter;
	FString OutputText = Formatter.FormatSelectedNodes(ExportedGraph, BlueprintName);

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return;
	}

	const FString DefaultFileName = FString::Printf(TEXT("%s_selection.txt"), *BlueprintName);
	const FString DefaultPath = FPaths::ProjectDir();

	TArray<FString> OutFiles;
	const bool bOpened = DesktopPlatform->SaveFileDialog(
		FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
		TEXT("Export Selected Nodes"),
		DefaultPath,
		DefaultFileName,
		TEXT("Text Files (*.txt)|*.txt"),
		0,
		OutFiles);

	if (bOpened && OutFiles.Num() > 0)
	{
		FFileHelper::SaveStringToFile(OutputText, *OutFiles[0],
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

// --- Iteration 6 ---

FString FBlueprintExporterModule::SanitizeFileName(const FString& Name)
{
	FString Result;
	Result.Reserve(Name.Len());
	for (TCHAR Ch : Name)
	{
		if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-'))
		{
			Result.AppendChar(Ch);
		}
		else
		{
			Result.AppendChar(TEXT('_'));
		}
	}
	return Result;
}

bool FBlueprintExporterModule::WriteFileIfChanged(const FString& FilePath, const FString& Content)
{
	FString ExistingContent;
	if (FFileHelper::LoadFileToString(ExistingContent, *FilePath))
	{
		if (ExistingContent == Content)
		{
			return false;  // 内容完全相同，跳过写入
		}
	}

	FFileHelper::SaveStringToFile(Content, *FilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return true;
}

FDateTime FBlueprintExporterModule::GetAssetFileTimestamp(const FAssetData& AssetData)
{
	FString PackagePath;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			AssetData.PackageName.ToString(), PackagePath,
			FPackageName::GetAssetPackageExtension()))
	{
		return FDateTime::MinValue();
	}

	return IFileManager::Get().GetTimeStamp(*PackagePath);
}

bool FBlueprintExporterModule::ShouldExport(UBlueprint* Blueprint,
	const UBlueprintExporterSettings* Settings) const
{
	if (!Blueprint || !Settings)
	{
		return false;
	}

	// Blueprint type filter
	switch (Blueprint->BlueprintType)
	{
	case BPTYPE_Normal:
		if (!Settings->bExportNormalBlueprint) return false;
		break;
	case BPTYPE_FunctionLibrary:
		if (!Settings->bExportFunctionLibrary) return false;
		break;
	case BPTYPE_MacroLibrary:
		if (!Settings->bExportMacroLibrary) return false;
		break;
	case BPTYPE_Interface:
		if (!Settings->bExportInterface) return false;
		break;
	case BPTYPE_LevelScript:
		if (!Settings->bExportLevelScript) return false;
		break;
	default:
		break;
	}

	// Blacklist: excluded parent classes
	UClass* ParentClass = Blueprint->ParentClass;
	for (const FSoftClassPath& ExcludedPath : Settings->ExcludedParentClasses)
	{
		UClass* ExcludedClass = ExcludedPath.ResolveClass();
		if (ExcludedClass && ParentClass && ParentClass->IsChildOf(ExcludedClass))
		{
			return false;
		}
	}

	// Whitelist: parent class filter (empty = no restriction)
	if (Settings->ParentClassFilter.Num() > 0)
	{
		bool bPassesWhitelist = false;
		for (const FSoftClassPath& WhitelistPath : Settings->ParentClassFilter)
		{
			UClass* WhitelistClass = WhitelistPath.ResolveClass();
			if (WhitelistClass && ParentClass && ParentClass->IsChildOf(WhitelistClass))
			{
				bPassesWhitelist = true;
				break;
			}
		}
		if (!bPassesWhitelist)
		{
			return false;
		}
	}

	// Node count filter
	int32 TotalNodes = 0;
	for (const UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		TotalNodes += Graph->Nodes.Num();
	}
	for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		TotalNodes += Graph->Nodes.Num();
	}

	if (TotalNodes < Settings->MinNodeCount)
	{
		return false;
	}

	return true;
}

void FBlueprintExporterModule::OnPackageSaved(const FString& /*PackageFilename*/,
	UPackage* Package, FObjectPostSaveContext /*SaveContext*/)
{
	const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
	if (!Settings || !Settings->bAutoExportOnSave || !Package)
	{
		return;
	}

	ForEachObjectWithPackage(Package, [this, Settings](UObject* Obj) -> bool
	{
		UBlueprint* BP = Cast<UBlueprint>(Obj);
		if (BP && ShouldExport(BP, Settings))
		{
			ExportBlueprintToCache(BP);
			GenerateIndexFile();
		}
		return true;
	});
}

void FBlueprintExporterModule::OnEditorPreExit()
{
	const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
	if (!Settings || !Settings->bExportOnEditorClose)
	{
		return;
	}

	ExportAllBlueprints();
}

bool FBlueprintExporterModule::ExportBlueprintToCache(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return false;
	}

	FBlueprintGraphExtractor Extractor;
	FExportedBlueprint ExportedBP = Extractor.Extract(Blueprint);

	FBlueprintTextFormatter Formatter;

	const FString BPName = Blueprint->GetName();
	const FString OutputDir = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("BlueprintExports"), BPName);

	IFileManager::Get().MakeDirectory(*OutputDir, /*Tree=*/true);

	int32 FilesWritten = 0;

	// Write individual graph files
	for (const FExportedGraph& Graph : ExportedBP.Graphs)
	{
		FString GraphText = Formatter.FormatGraphOnly(Graph);
		if (GraphText.IsEmpty())
		{
			continue;
		}

		const FString FileName = SanitizeFileName(Graph.GraphName) + TEXT(".txt");
		const FString FilePath = FPaths::Combine(OutputDir, FileName);
		if (WriteFileIfChanged(FilePath, GraphText))
		{
			FilesWritten++;
		}
	}

	// Write summary file
	const FString SummaryText = Formatter.FormatSummary(ExportedBP);
	const FString SummaryPath = FPaths::Combine(OutputDir, TEXT("_summary.txt"));
	if (WriteFileIfChanged(SummaryPath, SummaryText))
	{
		FilesWritten++;
	}

	if (FilesWritten > 0)
	{
		UE_LOG(LogBlueprintExporter, Log,
			TEXT("Exported %s: %d file(s) updated"), *Blueprint->GetName(), FilesWritten);
	}
	else
	{
		UE_LOG(LogBlueprintExporter, Verbose,
			TEXT("Exported %s: no changes detected, all files up-to-date"), *Blueprint->GetName());
	}

	return FilesWritten > 0;
}

void FBlueprintExporterModule::ExportAllBlueprints()
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	AssetRegistry.SearchAllAssets(/*bSynchronousSearch=*/true);

	TArray<FAssetData> BPAssets;
	AssetRegistry.GetAssetsByClass(
		UBlueprint::StaticClass()->GetClassPathName(), BPAssets, /*bSearchSubClasses=*/true);

	const UBlueprintExporterSettings* Settings = GetDefault<UBlueprintExporterSettings>();
	const FString ExportDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));

	TSet<FString> CurrentBPNames;
	int32 ExportedCount = 0;
	int32 SkippedCount  = 0;
	int32 FilteredCount = 0;

	for (const FAssetData& AssetData : BPAssets)
	{
		const FString BPName       = AssetData.AssetName.ToString();
		const FString SanitizedName = SanitizeFileName(BPName);

		// ── 第一层：时间戳比对，跳过未修改的蓝图 ──
		const FString SummaryPath      = ExportDir / SanitizedName / TEXT("_summary.txt");
		const FDateTime ExportTimestamp = IFileManager::Get().GetTimeStamp(*SummaryPath);

		if (ExportTimestamp > FDateTime::MinValue())
		{
			const FDateTime UassetTimestamp = GetAssetFileTimestamp(AssetData);
			if (UassetTimestamp > FDateTime::MinValue() && ExportTimestamp >= UassetTimestamp)
			{
				// .uasset 未发生变更，跳过加载
				SkippedCount++;
				CurrentBPNames.Add(SanitizedName);
				continue;
			}
		}

		// ── 需要加载并检查 ──
		UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset());
		if (!BP)
		{
			continue;
		}

		if (!ShouldExport(BP, Settings))
		{
			FilteredCount++;
			continue;
		}

		// ── 第二层：内容比对在 ExportBlueprintToCache 内部执行 ──
		const bool bActuallyWrote = ExportBlueprintToCache(BP);
		ExportedCount++;
		CurrentBPNames.Add(SanitizedName);

		if (!bActuallyWrote)
		{
			UE_LOG(LogBlueprintExporter, Verbose,
				TEXT("  %s: loaded but content unchanged (timestamp drift)"), *BPName);
		}
	}

	CleanupStaleExports(CurrentBPNames);
	GenerateIndexFile();

	UE_LOG(LogBlueprintExporter, Log,
		TEXT("ExportAll complete: %d exported, %d skipped (unchanged), %d filtered out, %d total assets"),
		ExportedCount, SkippedCount, FilteredCount, BPAssets.Num());
}

void FBlueprintExporterModule::GenerateIndexFile()
{
	const FString BaseDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));

	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *(BaseDir / TEXT("*")), /*bFiles=*/false, /*bDirectories=*/true);

	TArray<FString> IndexLines;
	TArray<FString> ReadmeLines;

	const FString Timestamp = FDateTime::Now().ToString();

	IndexLines.Add(FString::Printf(TEXT("Blueprint Export Index — %s"), *Timestamp));
	IndexLines.Add(TEXT(""));

	ReadmeLines.Add(TEXT("# Blueprint Exports"));
	ReadmeLines.Add(TEXT(""));
	ReadmeLines.Add(FString::Printf(TEXT("Generated: %s"), *Timestamp));
	ReadmeLines.Add(TEXT(""));
	ReadmeLines.Add(TEXT("| Blueprint | Parent | Graphs | Variables |"));
	ReadmeLines.Add(TEXT("|-----------|--------|--------|-----------|"));

	int32 BPCount = 0;

	for (const FString& SubDir : SubDirs)
	{
		if (SubDir.StartsWith(TEXT("_")))
		{
			continue;
		}

		const FString SummaryPath = FPaths::Combine(BaseDir, SubDir, TEXT("_summary.txt"));
		FString SummaryContent;
		if (!FFileHelper::LoadFileToString(SummaryContent, *SummaryPath))
		{
			continue;
		}

		++BPCount;

		FString BPName = SubDir;
		FString ParentName;
		int32 GraphCount = 0;
		int32 VarCount = 0;

		TArray<FString> SummaryLines;
		SummaryContent.ParseIntoArrayLines(SummaryLines);

		bool bInVarSection = false;
		bool bInGraphSection = false;

		for (const FString& Line : SummaryLines)
		{
			// Parse blueprint header
			if (Line.StartsWith(TEXT("=== Blueprint:")))
			{
				FString Temp = Line;
				Temp.RemoveFromStart(TEXT("=== Blueprint:"));
				Temp.RemoveFromEnd(TEXT("==="));
				Temp.TrimStartAndEndInline();

				int32 ParenIdx = Temp.Find(TEXT("(Parent:"));
				if (ParenIdx != INDEX_NONE)
				{
					BPName = Temp.Left(ParenIdx).TrimEnd();
					ParentName = Temp.Mid(ParenIdx + 8); // skip "(Parent:"
					ParentName.RemoveFromEnd(TEXT(")"));
					ParentName.TrimStartAndEndInline();
				}
				else
				{
					BPName = Temp.TrimStartAndEnd();
				}
				bInVarSection = false;
				bInGraphSection = false;
				continue;
			}

			if (Line == TEXT("=== Variables ==="))
			{
				bInVarSection = true;
				bInGraphSection = false;
				continue;
			}

			if (Line == TEXT("=== Graphs ==="))
			{
				bInVarSection = false;
				bInGraphSection = true;
				continue;
			}

			if (Line.StartsWith(TEXT("===")))
			{
				bInVarSection = false;
				bInGraphSection = false;
				continue;
			}

			if (bInVarSection && Line.StartsWith(TEXT("  ")) && !Line.TrimStart().IsEmpty())
			{
				++VarCount;
			}

			if (bInGraphSection && Line.StartsWith(TEXT("  ")) && !Line.TrimStart().IsEmpty())
			{
				++GraphCount;
			}
		}

		// Index entry
		IndexLines.Add(FString::Printf(TEXT("%s"), *SubDir));
		if (!ParentName.IsEmpty())
		{
			IndexLines.Add(FString::Printf(TEXT("  Parent: %s"), *ParentName));
		}
		IndexLines.Add(FString::Printf(TEXT("  Graphs: %d"), GraphCount));
		IndexLines.Add(FString::Printf(TEXT("  Variables: %d"), VarCount));
		IndexLines.Add(TEXT(""));

		// README row
		ReadmeLines.Add(FString::Printf(TEXT("| %s | %s | %d | %d |"),
			*BPName, *ParentName, GraphCount, VarCount));
	}

	// Insert total count after first line
	IndexLines.Insert(FString::Printf(TEXT("Total: %d blueprints"), BPCount), 1);

	FFileHelper::SaveStringToFile(
		FString::Join(IndexLines, TEXT("\n")),
		*FPaths::Combine(BaseDir, TEXT("_index.txt")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	FFileHelper::SaveStringToFile(
		FString::Join(ReadmeLines, TEXT("\n")),
		*FPaths::Combine(BaseDir, TEXT("README.md")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FBlueprintExporterModule::CleanupStaleExports(const TSet<FString>& CurrentBPNames)
{
	const FString BaseDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintExports"));

	TArray<FString> SubDirs;
	IFileManager::Get().FindFiles(SubDirs, *(BaseDir / TEXT("*")), /*bFiles=*/false, /*bDirectories=*/true);

	for (const FString& SubDir : SubDirs)
	{
		if (SubDir.StartsWith(TEXT("_")))
		{
			continue;
		}

		if (!CurrentBPNames.Contains(SubDir))
		{
			const FString FullPath = FPaths::Combine(BaseDir, SubDir);
			IFileManager::Get().DeleteDirectory(*FullPath, /*RequireExists=*/false, /*Tree=*/true);
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FBlueprintExporterModule, BlueprintExporter)
