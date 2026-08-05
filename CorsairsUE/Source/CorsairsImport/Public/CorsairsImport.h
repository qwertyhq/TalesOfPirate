#pragma once

#include "Modules/ModuleManager.h"

class FCorsairsImportModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
