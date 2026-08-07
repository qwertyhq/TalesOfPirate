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
			// Карты высот: персонаж удерживается на земле по ним, а не
			// физикой — у рельефа из glTF физической формы нет.
			"CorsairsImport",
			"Json",
			"JsonUtilities",
		});
	}
}
