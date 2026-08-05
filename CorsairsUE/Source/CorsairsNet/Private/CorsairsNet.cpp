#include "CorsairsNet.h"

#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsNet, Log, All);

void FCorsairsNetModule::StartupModule()
{
	UE_LOG(LogCorsairsNet, Log, TEXT("CorsairsNet: модуль запущен"));
}

void FCorsairsNetModule::ShutdownModule()
{
	UE_LOG(LogCorsairsNet, Log, TEXT("CorsairsNet: модуль остановлен"));
}

IMPLEMENT_MODULE(FCorsairsNetModule, CorsairsNet)
