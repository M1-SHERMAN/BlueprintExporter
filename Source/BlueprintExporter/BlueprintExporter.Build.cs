using UnrealBuildTool;

public class BlueprintExporter : ModuleRules
{
	public BlueprintExporter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"BlueprintGraph",
			"KismetCompiler",
			"Kismet",
			"GraphEditor",
			"ToolMenus",
			"ContentBrowser",
			"AssetTools",
			"Slate",
			"SlateCore",
			"InputCore",
			"DesktopPlatform",
			"ApplicationCore",
			"AssetRegistry",
			"DeveloperSettings",
		});
	}
}
