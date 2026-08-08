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

		// Source-only Editor builds remain valid before Task 7 has generated
		// runtime terrain data. Packaging acceptance independently requires all
		// three files and audits their exact UFS members.
		string ProjectRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		if (File.Exists(Path.Combine(ProjectRoot, "Data", "character_map.json")))
		{
			RuntimeDependencies.Add("$(ProjectDir)/Data/character_map.json", StagedFileType.UFS);
		}
		if (File.Exists(Path.Combine(ProjectRoot, "Data", "Heights", "garner.block.raw")))
		{
			RuntimeDependencies.Add("$(ProjectDir)/Data/Heights/garner.block.raw", StagedFileType.UFS);
		}
		if (File.Exists(Path.Combine(ProjectRoot, "Data", "Heights", "garner.terrain.json")))
		{
			RuntimeDependencies.Add("$(ProjectDir)/Data/Heights/garner.terrain.json", StagedFileType.UFS);
		}
	}
}
