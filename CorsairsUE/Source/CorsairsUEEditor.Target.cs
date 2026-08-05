using UnrealBuildTool;

public class CorsairsUEEditorTarget : TargetRules
{
	public CorsairsUEEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.AddRange(
			new string[] { "CorsairsNet", "CorsairsGame", "CorsairsUE", "CorsairsImport" });
	}
}
