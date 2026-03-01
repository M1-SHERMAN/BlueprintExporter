#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "BlueprintExporterSettings.generated.h"

UCLASS(config=EditorPerProjectUserSettings, defaultconfig,
       meta=(DisplayName="Blueprint Exporter"))
class UBlueprintExporterSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UBlueprintExporterSettings();
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override  { return TEXT("Blueprint Exporter"); }

	// Auto Export
	UPROPERTY(Config, EditAnywhere, Category="Auto Export")
	bool bAutoExportOnSave = false;
	UPROPERTY(Config, EditAnywhere, Category="Auto Export")
	bool bExportOnEditorClose = false;

	// Blueprint Type Filter
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
	bool bExportNormalBlueprint = true;
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
	bool bExportFunctionLibrary = true;
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
	bool bExportMacroLibrary = false;
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
	bool bExportInterface = false;
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Blueprint Type")
	bool bExportLevelScript = false;

	// Parent Class Filter
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Parent Class", meta=(AllowAbstract="true"))
	TArray<FSoftClassPath> ParentClassFilter;       // Whitelist, empty = no restriction
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Parent Class", meta=(AllowAbstract="true"))
	TArray<FSoftClassPath> ExcludedParentClasses;   // Blacklist, takes priority over whitelist

	// Content Filter
	UPROPERTY(Config, EditAnywhere, Category="Export Filter|Content", meta=(ClampMin=1, ClampMax=100))
	int32 MinNodeCount = 2;
};
