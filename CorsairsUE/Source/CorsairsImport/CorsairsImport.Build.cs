using UnrealBuildTool;

// Editor-модуль: импорт манифестов сцен, карт высот и проходимости из
// результатов AssetConverter. В рантайм не попадает.
public class CorsairsImport : ModuleRules
{
	public CorsairsImport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
		});
	}
}
