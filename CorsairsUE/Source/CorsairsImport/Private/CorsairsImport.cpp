#include "CorsairsImport.h"

DEFINE_LOG_CATEGORY_STATIC(LogCorsairsImport, Log, All);

void FCorsairsImportModule::StartupModule()
{
	UE_LOG(LogCorsairsImport, Log, TEXT("CorsairsImport: модуль запущен"));
}

void FCorsairsImportModule::ShutdownModule()
{
	UE_LOG(LogCorsairsImport, Log, TEXT("CorsairsImport: модуль остановлен"));
}

IMPLEMENT_MODULE(FCorsairsImportModule, CorsairsImport)
