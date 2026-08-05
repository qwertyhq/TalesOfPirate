// Мост к бинарному протоколу из sources/Libraries/CorsairsNet.
//
// Файл существует, чтобы доказать компилируемость протокола внутри UE и
// зафиксировать это тестом: если раскладка байтов или зависимости разъедутся,
// сборка модуля упадёт здесь, а не на живом сервере.

#include "ProtocolBridge.h"

// Заголовки протокола лежат вне UE-проекта и подключаются по пути, заданному в
// CorsairsNet.Build.cs.
THIRD_PARTY_INCLUDES_START
#include "CorsairsNet/include/BinaryIO.h"
#include "CorsairsNet/include/CommandMessages.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsProtocol, Log, All);

namespace Corsairs::Protocol
{

bool CheckByteOrder()
{
	// Протокол big-endian; сериализация обязана давать именно такой порядок.
	// Совместимость с F#-серверами держится на этом.
	uint8 Buffer[4] = {0, 0, 0, 0};
	Corsairs::Net::writeUInt32(Buffer, 0x11223344u);

	const bool bBigEndian =
		Buffer[0] == 0x11 && Buffer[1] == 0x22 && Buffer[2] == 0x33 && Buffer[3] == 0x44;

	const bool bRoundTrip = Corsairs::Net::readUInt32(Buffer) == 0x11223344u;

	if (!bBigEndian || !bRoundTrip)
	{
		UE_LOG(LogCorsairsProtocol, Error,
			   TEXT("Протокол: порядок байт нарушен (BE=%d, round-trip=%d)"),
			   bBigEndian ? 1 : 0, bRoundTrip ? 1 : 0);
		return false;
	}

	return true;
}

} // namespace Corsairs::Protocol
