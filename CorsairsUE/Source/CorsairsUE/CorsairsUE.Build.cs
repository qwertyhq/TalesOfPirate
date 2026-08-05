using UnrealBuildTool;

// Первичный игровой модуль. Намеренно пустой: вся логика живёт в CorsairsGame,
// здесь только точка входа, которую требует UE от проекта.
public class CorsairsUE : ModuleRules
{
	public CorsairsUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"CorsairsGame",
		});
	}
}
