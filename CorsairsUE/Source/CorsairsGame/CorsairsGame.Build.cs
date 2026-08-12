using UnrealBuildTool;

// Игровой слой: состояние мира, персонажи, ввод. Пишется в UE-идиомах;
// существующий Client/src/Scene используется как построчная спецификация
// поведения, а не как источник для переноса.
public class CorsairsGame : ModuleRules
{
	public CorsairsGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"CorsairsNet",
			"Json",
			"JsonUtilities",
		});
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

		RuntimeDependencies.Add("$(ProjectDir)/Data/skills.json", StagedFileType.UFS);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Data/Heights/garner.height.r16",
			StagedFileType.UFS);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Data/Heights/garner.block.raw",
			StagedFileType.UFS);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Data/Heights/garner.region.raw",
			StagedFileType.UFS);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Data/Heights/garner.terrain.json",
			StagedFileType.UFS);
		RuntimeDependencies.Add(
			"$(ProjectDir)/Data/Heights/garner.runtime.json",
			StagedFileType.UFS);
	}
}
