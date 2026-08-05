// GameServer.cpp : Defines the entry point for the console application.
//

#include "Core/stdafx.h"
#include <thread>
namespace Corsairs::Common::Localization {}
using namespace Corsairs::Common::Localization;

#include "Database/AssetDatabase.h"
#include "App/GameAppNet.h"
#include "App/GameApp.h"
#include "Services/SystemDialog/SystemDialog.h"
#include "App/Config.h"
#include "CrushSystem.h"
#include "Db/GameDB.h"
#include "Localization/LanguageRecordStore.h"


// #pragma comment( linker, "/subsystem:\"windows\" /entry:\"mainCRTStartup\"" ) //

extern BOOL GameServer_Begin();
extern void GameServer_End();
BOOL g_bGameEnd = FALSE;
CGameApp* g_pGameApp = NULL;
std::string g_strLogName = "GameServerLog";
HANDLE hGameT;
HANDLE hConsole = NULL;

void DisableCloseButton();
void AppExit(void);


//#pragma init_seg(user)
#pragma init_seg( lib )

#include <signal.h>

void sigintHandler(int sig_num) {
	signal(SIGINT, sigintHandler);
	C_PRINT("Notify Logout to GameServer...\n");
	g_pGameApp->m_CTimerReset.Begin(1000);
	g_pGameApp->m_ulLeftSec = 3;
	fflush(stdout);
}

int main(int argc, char* argv[]) {
	SetConsoleOutputCP(CP_UTF8);
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

	C_TITLE("GameServer.exe")
	C_PRINT("Loading %s\n", szConfigFileN);

	::SetThreadName("main");
	Corsairs::Util::SetGlobalCRTExceptionBehavior();
	Corsairs::Util::SetPerThreadCRTExceptionBehavior();
	Corsairs::Util::SetupDumpSetting("log\\game_server\\dumps");
	g_logManager.InitLogger("log\\game_server");
	g_logManager.EnableGlobalConsole(true);

	DisableCloseButton();

	// SEHTranslator translator;

	if (argc >= 2) {
		strncpy(szConfigFileN, argv[1], defCONFIG_FILE_NAME_LEN - 1);
		szConfigFileN[defCONFIG_FILE_NAME_LEN - 1] = '\0';
	}
	if (!g_Config.Load(szConfigFileN)) {
		ToLogService("common", "config init...Fail!");
		return FALSE;
	}

	AssetDatabase::Instance()->Open(g_Config._assetDbPath);
	LanguageRecordStore::Instance()->Load(AssetDatabase::Instance()->GetDb(), "english", LanguageTarget::Server);

	if (!g_Command.Load("cmd.cfg")) {
		g_Command.SetDefault();
	}

#ifdef __CATCH
	ToLogService("common", "Define __CATCH");
#endif

	atexit(AppExit);
	if (!GameServer_Begin()) {
		return 0;
	}
	signal(SIGINT, sigintHandler);

	MSG msg;
	while (!g_bGameEnd) {
		if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT) {
				if (!g_bGameEnd)
					g_pGameApp->SaveAllPlayer();
				g_bGameEnd = TRUE;
				break;
			}
		}
		else {
			SystemReport(GetTickCount());
			Sleep(50);
		}
	}
	GameServer_End();
	ToLogService("common", "game map server succeed to exit");
	return 0;
}


//// CorsairsNet: ThreadPool     TcpClient

extern DWORD WINAPI g_GameLogicProcess(LPVOID lpParameter);

BOOL GameServer_Begin() {
	_setmaxstdio(2048);

	//LG("init", "[%s]...\n", g_Config.m_szName);
	ToLogService("common", "game map server [{}] startup...", g_Config.m_szName);

	g_pGameApp = new CGameApp();
	if (!g_pGameApp->Init()) {
		//LG("init", "GameApp , !\n");
		ToLogService("common", "GameApp initialization failed, exit!");
		return FALSE;
	}

	Corsairs::Net::InitWinSock();

	g_gmsvr = new GameServerApp();
	// connect thread    GameServerApp
	ToLogService("common", "startup Gate server connect thread...");

	//
	//LG("init", "...\n");
	ToLogService("common", "startup game thread...");
#ifdef _WIN32
	DWORD dwThreadID;
	hGameT = CreateThread(NULL, 0, g_GameLogicProcess, 0, 0, &dwThreadID);
	ToLogService("common", "Game Thread ID = {}", dwThreadID);
#else
	// На POSIX поток запускается стандартными средствами и отсоединяется:
	// владение им, как и на Windows, остаётся у процесса до завершения.
	std::thread(g_GameLogicProcess, nullptr).detach();
	ToLogService("common", "Game Thread started (std::thread)");
#endif
	//

	//LG("init",  "Win32 \n");
	ToLogService("common", "start create Win32 control dialog box");
#ifdef _WIN32
	// Диалог управления — часть Windows-версии; на POSIX сервер консольный.
	HINSTANCE hInst = GetModuleHandle(0);
	CreateMainDialog(hInst, NULL);
#endif

	//       CGameApp::Log
	g_pGameApp->Log("restart", "GameServer restart", g_Config.m_mapList[0].c_str(), "", "", "");

	return TRUE;
}


void GameServer_End() {
	//LG("init", "\n");
	ToLogService("common", "start to exit game map server");
#ifdef _WIN32
	CloseHandle(hGameT);

	HWND hConsole = GetConsoleWindow();
	if (hConsole) {
		SendMessage(hConsole, WM_CLOSE, 0, 0);
	}
#endif

	Sleep(400);

	SAFE_DELETE(g_pGameApp);

	delete g_gmsvr;

	Corsairs::Net::CleanupWinSock();
}

#ifdef _WIN32
typedef HWND (*LPGETCONSOLEWINDOW)(void);

// Гасит кнопку закрытия окна консоли, чтобы сервер не уронили случайным
// кликом. Понятие существует только в Windows: в POSIX-терминале окном
// управляет эмулятор терминала, а не процесс.
void DisableCloseButton() {
	HMODULE hMod = LoadLibrary("kernel32.dll");

	LPGETCONSOLEWINDOW lpfnGetConsoleWindow =
		(LPGETCONSOLEWINDOW)GetProcAddress(hMod, "GetConsoleWindow");

	HWND hWnd = lpfnGetConsoleWindow();
	HMENU hMenu = GetSystemMenu(hWnd, FALSE);
	if (hMenu != NULL) {
		EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
	}

	FreeLibrary(hMod);
}
#else
void DisableCloseButton() {
}
#endif

void AppExit(void) {
	//int	*p = NULL;
	// *p = 1;
}

/*
 GameServer

 GameServer



[GameData]

Map
MgrUnit
Player
Character
Item
Skill
SkillState
Mission


GameData

[GameControl]
App
TimerMgr
AI        AI

[EventHandler]

GameServer

GameControl, AI,

GameControl  EventHandler Event, AI,
  EventHandler Event, , ,

EventHandlerEvent, ,

EventHandlerEvent, Modify GameData

EventHandler ,
Modify GameData


Control -> Event






*/
