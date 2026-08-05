//=============================================================================
// FileName: Attachable.cpp
// Creater: ZhangXuedong
// Date: 2004.10.19
// Comment: CAttachable class
//=============================================================================
#include "Core/stdafx.h"
#include "Entity/Attachable.h"
#include "Entity/GamePool.h"
#include <iostream>
#include <fstream>
#include <ostream>
// #include <fstream>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <string>
#include "PlatformCompat.h"

CAttachable::CAttachable()
{
	m_pCConjureLast = 0;
	m_pCConjureNext = 0;

	m_pCPassengerLast = 0;
	m_pCPassengerNext = 0;

	m_pCPlayer = 0;
	m_pCShipMaster = 0;
	m_pCShip = 0;
}

void CAttachable::Initially()
{
	Entity::Initially();

	m_pCConjureLast = 0;
	m_pCConjureNext = 0;

	m_pCPassengerLast = 0;
	m_pCPassengerNext = 0;

	m_pCPlayer = 0;
	m_pCShipMaster = 0;
	m_pCShip = 0;
}

void CAttachable::Finally()
{
	Entity::Finally();
	m_pCPlayer = 0;
}

void CAttachable::SetPlayer(CPlayer* pCPlayer)
{
	assert(!pCPlayer || GamePool::Instance().IsValidPlayerPtr(pCPlayer));
	m_pCPlayer = pCPlayer;
}
