#pragma once

#include "Modules/ModuleManager.h"

// Модуль сетевого слоя. Загружается в PreDefault, до игрового модуля: игровой
// слой подписывается на события соединения при своей инициализации.
class FCorsairsNetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
