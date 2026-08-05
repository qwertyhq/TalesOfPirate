using System.IO;
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

		// Заголовки протокола подключаются НА МЕСТЕ, из sources/Libraries, а не
		// копируются в проект. Копия неизбежно разошлась бы с оригиналом,
		// который продолжает использовать GameServer, — и расхождение
		// проявилось бы как рассинхрон раскладки байтов в рантайме.
		string LibrariesRoot = Path.GetFullPath(
			Path.Combine(ModuleDirectory, "..", "..", "..", "sources", "Libraries"));
		PublicIncludePaths.Add(LibrariesRoot);

		// CommandMessages.h бросает исключения при разборе испорченных пакетов.
		// В модулях UE исключения выключены по умолчанию, поэтому включаем их
		// точечно здесь: переписывать 13 864 строки протокола под коды возврата
		// значит потерять гарантию совместимости с сервером.
		bEnableExceptions = true;

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
