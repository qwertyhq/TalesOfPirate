//=============================================================================
// FileName: Action.cpp
// Creater: ZhangXuedong
// Date: 2004.10.08
// Comment: CAction class
//=============================================================================
#include "Core/stdafx.h"
#include "Combat/Action.h"
#include "Network/NetCommand.h"
#include "Network/NetRetCode.h"
#include "Core/RoleCommon.h"
#include "Network/CompCommand.h"
#include "Core/CommFunc.h"
#include "Combat/MoveAble.h"
#include "Character/Character.h"
#include "App/GameApp.h"

;

CAction::CAction(Entity *pCEntity)
{
	m_pCEntity = pCEntity;
	memset(m_SAction, 0, sizeof(m_SAction));
	m_sActionNum = 0;
	m_sCurAction = -1;
}

bool CAction::Add(int16_t sActionType, void *pActionData)
{
	if (m_sCurAction != -1 || m_sActionNum >= defMAX_ACTION_NUM)
	{
		return false;
	}

	m_SAction[m_sActionNum].sType = sActionType;
	switch (m_SAction[m_sActionNum].sType)
	{
	case	enumACTION_MOVE:
		{
			memcpy(&m_SMoveInit, pActionData, sizeof(m_SMoveInit));
			m_SAction[m_sActionNum].pInit = &m_SMoveInit;
			m_sActionNum ++;

			break;
		}
	case	enumACTION_SKILL:
		{
			memcpy(&m_SFightInit, pActionData, sizeof(m_SFightInit));
			m_SAction[m_sActionNum].pInit = &m_SFightInit;
			m_sActionNum ++;

			break;
		}
	default:
		{
			break;
		}
	}

	return true;
}

//=============================================================================
// sActionType .0
// sActionState 
//=============================================================================
bool CAction::DoNext(int16_t sActionType, int16_t sActionState)
{
	if (m_sActionNum == 0 || m_pCEntity == NULL)
	{
		return false;
	}
	if (m_sCurAction >= m_sActionNum - 1)
	{
		m_sCurAction = -1;
		m_sActionNum = 0;
		return false;
	}

	switch (m_SAction[m_sCurAction + 1].sType)
	{
	case	enumACTION_MOVE:
		{
			CMoveAble *pCMoveAble = m_pCEntity->IsMoveAble();
			if (pCMoveAble)
			{
				m_sCurAction ++;

				if (pCMoveAble->DesireMoveBegin((CMoveAble::SMoveInit *)m_SAction[m_sCurAction].pInit) == false)
				{
					m_sCurAction = -1;
					m_sActionNum = 0;
				}
			}

			break;
		}
	case	enumACTION_SKILL:
		{
			if (sActionType == enumACTION_MOVE && sActionState & enumMSTATE_INRANGE)
			{
				CFightAble *pCFightAble = m_pCEntity->IsFightAble();
				if (pCFightAble)
				{
					m_sCurAction ++;

					if (pCFightAble->DesireFightBegin((SFightInit *)m_SAction[m_sCurAction].pInit) == false)
					{
						m_sCurAction = -1;
						m_sActionNum = 0;
					}
				}
			}
			else
				Interrupt();

			break;
		}
	default:
		{
			return false;

			break;
		}
	}

	return true;
}

void CAction::End()
{
	if (m_sActionNum == 0 || m_sCurAction == -1 || m_pCEntity == NULL)
	{
		return;
	}

	switch (m_SAction[m_sCurAction].sType)
	{
	case	enumACTION_MOVE:
		{
			CMoveAble *pCMoveAble = m_pCEntity->IsMoveAble();
			if (pCMoveAble)
			{
				pCMoveAble->DesireMoveStop();
			}

			break;
		}
	case	enumACTION_SKILL:
		{
			CFightAble *pCFightAble = m_pCEntity->IsFightAble();
			if (pCFightAble)
			{
				pCFightAble->DesireFightEnd();
			}

			break;
		}
	default:
		{
			break;
		}
	}

	m_sCurAction = -1;
	m_sActionNum = 0;

	return;
}

void CAction::Interrupt()
{
	m_sCurAction = -1;
	m_sActionNum = 0;

	return;
}

bool CAction::Has(int16_t sActionType, void *pActionData)
{
	//if (m_sActionNum == 0 || m_pCEntity == NULL)
	//	return false;
	//if (m_sCurAction >= m_sActionNum - 1)
	//	return false;

	//for (int16_t i = m_sCurAction + 1; i < m_sActionNum; i++)
	//{
	//	if (m_SAction[i].sType == sActionType)
	//	{
	//	}
	//}

	return false;
}

//=============================================================================
CActionCache::CActionCache(CCharacter *pCOwn)
{
	m_pCOwn = pCOwn;
	m_pSExecQueue = NULL;
	m_pSFreeQueue = NULL;
}

CActionCache::~CActionCache()
{
	SAction	*pSCarrier;

	pSCarrier = m_pSExecQueue;
	while (pSCarrier)
	{
		m_pSExecQueue = pSCarrier->pSNext;
		delete pSCarrier;
		pSCarrier = m_pSExecQueue;
	}

	pSCarrier = m_pSFreeQueue;
	while (pSCarrier)
	{
		m_pSFreeQueue = pSCarrier->pSNext;
		delete pSCarrier;
		pSCarrier = m_pSFreeQueue;
	}
}

void CActionCache::AddCommand(int16_t sCommand)
{
	SAction	*pSCarrier = NULL;

	if (m_pSFreeQueue)
	{
		pSCarrier = m_pSFreeQueue;
		m_pSFreeQueue = pSCarrier->pSNext;
	}
	else
	{
		pSCarrier = new SAction;
		if (!pSCarrier)
		{
			ThrowRuntimeError(RES_STRING(GM_ACTION_CPP_00001));
		}
	}

	pSCarrier->sCommand = sCommand;
	pSCarrier->chParamPos = 0;

	pSCarrier->pSNext = m_pSExecQueue;
	m_pSExecQueue = pSCarrier;
}

void CActionCache::PushParam(void *pParam, char chSize)
{
	if (m_pSExecQueue && (m_pSExecQueue->chParamPos + chSize <= defMAX_CACHE_ACTION_PARAM_LEN))
	{
		memcpy(m_pSExecQueue->szParam + m_pSExecQueue->chParamPos, pParam, chSize);
		m_pSExecQueue->chParamPos += chSize;
	}
}

void CActionCache::Run()
{
	SAction	*pSCarrier, *pSLastCarrier;
	pSCarrier = pSLastCarrier = m_pSExecQueue;
	while (pSCarrier)
	{
		ExecAction(pSCarrier);

		if (pSCarrier == m_pSExecQueue)
		{
			m_pSExecQueue = pSCarrier->pSNext;

			pSCarrier->pSNext = m_pSFreeQueue;
			m_pSFreeQueue = pSCarrier;

			pSLastCarrier = m_pSExecQueue;
			pSCarrier = pSLastCarrier;
		}
		else
		{
			pSLastCarrier->pSNext = pSCarrier->pSNext;

			pSCarrier->pSNext = m_pSFreeQueue;
			m_pSFreeQueue = pSCarrier;

			pSCarrier = pSLastCarrier->pSNext;
		}
	}
}

void CActionCache::ExecAction(SAction *pSCarrier)
{
	char	chCurParamPos = 0;
	switch (pSCarrier->sCommand)
	{
	case	enumCACHEACTION_MOVE:
		{
			int16_t sPing = *((int16_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(int16_t);
			char chPointNum = pSCarrier->szParam[chCurParamPos];
			chCurParamPos += sizeof(char);
			Corsairs::Util::Point *pPath = (Corsairs::Util::Point *)(pSCarrier->szParam + chCurParamPos);
			chCurParamPos += sizeof(Corsairs::Util::Point) * chPointNum;
			if (chCurParamPos == pSCarrier->chParamPos)
				m_pCOwn->Cmd_BeginMove(sPing, pPath, chPointNum);
			else if (chCurParamPos < pSCarrier->chParamPos)
			{
				char chStopState = pSCarrier->szParam[chCurParamPos];
				chCurParamPos += sizeof(char);
				if (chCurParamPos == pSCarrier->chParamPos)
					m_pCOwn->Cmd_BeginMove(sPing, pPath, chPointNum, chStopState);
			}
		}
		break;
	case	enumCACHEACTION_SKILL:
		{
			std::int32_t lSkillID = *((std::int32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::int32_t);
			std::uint32_t ulTarID = *((std::uint32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::uint32_t);
			std::int32_t lTarHandle = *((std::int32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::int32_t);
			// Цель восстанавливается по идентификатору: за время ожидания в
			// кэше она могла умереть, и сохранённый указатель был бы висячим.
			Entity *pCTar = g_pGameApp->IsMapEntity(ulTarID, lTarHandle);
			if (pCTar) {
				m_pCOwn->Cmd_BeginSkillDirect(lSkillID, pCTar);
			}
		}
		break;
	case	enumCACHEACTION_SKILL2:
		{
			std::int32_t lSkillID = *((std::int32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::int32_t);
			std::int32_t lSkillLv = *((std::int32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::int32_t);
			std::int32_t lPosX = *((std::int32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::int32_t);
			std::int32_t lPosY = *((std::int32_t *)(pSCarrier->szParam + chCurParamPos));
			chCurParamPos += sizeof(std::int32_t);
			m_pCOwn->Cmd_BeginSkillDirect2(lSkillID, lSkillLv, lPosX, lPosY);
		}
		break;
	}
}
