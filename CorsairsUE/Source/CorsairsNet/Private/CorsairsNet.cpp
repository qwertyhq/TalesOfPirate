#include "CorsairsNetModule.h"

#include "Modules/ModuleManager.h"
#include "ProtocolBridge.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsNet, Log, All);

void FCorsairsNetModule::StartupModule()
{
	// Проверка на старте, а не в тесте: если раскладка байтов разъедется с
	// серверной, узнать об этом надо немедленно, а не при первом же логине.
	const bool bProtocolOk = Corsairs::Protocol::CheckByteOrder();
	UE_LOG(LogCorsairsNet, Log, TEXT("CorsairsNet: модуль запущен, протокол %s"),
		   bProtocolOk ? TEXT("исправен") : TEXT("СЛОМАН"));
}

void FCorsairsNetModule::ShutdownModule()
{
	UE_LOG(LogCorsairsNet, Log, TEXT("CorsairsNet: модуль остановлен"));
}

IMPLEMENT_MODULE(FCorsairsNetModule, CorsairsNet)
