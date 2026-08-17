using UnrealBuildTool;
using System.IO;

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
		PrivateDependencyModuleNames.Add("AssetRegistry");

		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

		RuntimeDependencies.Add("$(ProjectDir)/Data/skills.json", StagedFileType.UFS);

		// Source-only Editor builds remain valid before Task 7 has generated
		// runtime terrain data. Packaging acceptance independently requires all
		// three files and audits their exact UFS members.
		string ProjectRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		RegisterDataIfPresent(ProjectRoot, "Data/character_map.json");
		RegisterDataIfPresent(ProjectRoot, "Data/Heights/garner.height.r16");
		RegisterDataIfPresent(ProjectRoot, "Data/Heights/garner.block.raw");
		RegisterDataIfPresent(ProjectRoot, "Data/Heights/garner.region.raw");
		RegisterDataIfPresent(ProjectRoot, "Data/Heights/garner.terrain.json");
		RegisterDataIfPresent(ProjectRoot, "Data/Heights/garner.runtime.json");
	}

	private void RegisterDataIfPresent(string projectRoot, string relativePath)
	{
		if (File.Exists(Path.Combine(projectRoot, relativePath.Replace('/', Path.DirectorySeparatorChar))))
		{
			RuntimeDependencies.Add("$(ProjectDir)/" + relativePath, StagedFileType.UFS);
		}
	}
}
