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
		// Только корень: добавлять сюда CorsairsNet/include нельзя. Там лежит
		// свой CorsairsNet.h, который перекрыл бы одноимённый публичный
		// заголовок этого модуля и заодно втянул бы транспорт на сырых сокетах.
		// По той же причине все включения идут полным путём от корня:
		// Packet.h в короткой форме разрешался в чужой заголовок движка.
		PublicIncludePaths.Add(LibrariesRoot);

		// Реализации из sources/Libraries включают свои заголовки короткими
		// именами ("Packet.h", "Crypto/Blake2s.h"), поэтому их каталоги нужны
		// на приватном пути. Наружу они не отдаются: публичные заголовки этого
		// модуля включают всё полным путём от корня Libraries.
		//
		// Публичный заголовок модуля называется CorsairsNetModule.h, а не
		// CorsairsNet.h: последний есть и в библиотеке, и совпадение имён
		// приводило к тому, что вместо нашего подключался её заголовок,
		// втягивая транспорт на сырых сокетах.
		PrivateIncludePaths.Add(Path.Combine(LibrariesRoot, "CorsairsNet", "include"));
		PrivateIncludePaths.Add(Path.Combine(LibrariesRoot, "common", "src"));

		// Реализации протокола втягиваются тонкими файлами Private/Protocol_*.cpp и
		// Private/Mpack_*.cpp — каждый включает один .cpp из sources/Libraries.
		// Порознь, потому что собранные в одну единицу трансляции исходники mpack
		// конфликтуют на MPACK_EMIT_INLINE_DEFS.
		

		// CommandMessages.h бросает исключения при разборе испорченных пакетов.
		// В модулях UE исключения выключены по умолчанию, поэтому включаем их
		// точечно здесь: переписывать 13 864 строки протокола под коды возврата
		// значит потерять гарантию совместимости с сервером.
		bEnableExceptions = true;
		// Общий wire-контракт клиента и GameServer всегда включает PacketId в
		// BEGIN/NOTIACTION. Макрос должен быть публичным: CommandMessages.h
		// разбирается и в потребляющем модуле CorsairsGame.
		PublicDefinitions.Add("defPROTOCOL_HAVE_PACKETID=1");

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
