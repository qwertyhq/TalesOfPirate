using UnrealBuildTool;

// Сетевой модуль: сюда переезжает бинарный протокол WPacket/RPacket из
// sources/Libraries/CorsairsNet (CommandMessages.h, mpack) практически без
// изменений. Транспорт пишется заново на FSocket — legacy-реализация на сырых
// сокетах и собственных потоках не переносится.
public class CorsairsNet : ModuleRules
{
	public CorsairsNet(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Sockets",
			"Networking",
		});
	}
}
