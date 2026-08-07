#pragma once

#include "CorsairsSession.h"
#include "CorsairsNet/include/CommandMessages.h"

FCorsairsCharacterLook MakeCharacterLook(
	const Corsairs::Net::Msg::ChaLookInfo& Source);
