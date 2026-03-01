#pragma once

#include "Modules/ModuleManager.h"

class UBlueprintExporterSettings;

class FBlueprintExporterModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void RegisterGraphEditorExtender();
	void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason);
	void ExportSelectedBlueprints(const TArray<FAssetData>& SelectedAssets);
	void CopySelectedNodesToClipboard(const UEdGraph* Graph);
	void ExportSelectedNodesToFile(const UEdGraph* Graph);

	// --- Iteration 6 ---
	void OnPackageSaved(const FString& PackageFilename, UPackage* Package,
	                    FObjectPostSaveContext SaveContext);
	void OnEditorPreExit();
	bool ExportBlueprintToCache(UBlueprint* Blueprint);
	void ExportAllBlueprints();
	void GenerateIndexFile();
	void CleanupStaleExports(const TSet<FString>& CurrentBPNames);
	bool ShouldExport(UBlueprint* Blueprint, const UBlueprintExporterSettings* Settings) const;
	static FString SanitizeFileName(const FString& Name);
	/** 比对内容并写入文件，内容相同则跳过。返回 true 表示实际写入了文件。 */
	static bool WriteFileIfChanged(const FString& FilePath, const FString& Content);
	/** 获取蓝图 .uasset 文件的磁盘修改时间。返回 FDateTime::MinValue() 表示无法获取。 */
	static FDateTime GetAssetFileTimestamp(const FAssetData& AssetData);

	int32 GraphEditorMenuExtenderIndex = INDEX_NONE;
	FDelegateHandle ModulesChangedHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle PreExitHandle;
};
