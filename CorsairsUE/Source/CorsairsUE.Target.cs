using UnrealBuildTool;

public class CorsairsUETarget : TargetRules
{
	public CorsairsUETarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(new string[] { "CorsairsNet", "CorsairsGame", "CorsairsUE" });
	}
}
