#pragma once

#include <functional>
#include <string>

#define  WIN32_LEAN_AND_MEAN
#define  NOMINMAX
#include <winsock2.h>
// Windows-заголовки только на Windows; на POSIX — словарь совместимости.
#include "PlatformCompat.h"

//     Win32 API ( )
void SetThreadName(const std::string& name);

namespace Corsairs::Util {
	//       
	void SetupDumpSetting(const std::string& dumpPath);
	void SetupDumpSetting(const std::string& dumpPath, const std::function<void()>& function);

	//      
	void SetPerThreadCRTExceptionBehavior();
	//    
	void SetGlobalCRTExceptionBehavior();
	//    
	void CreateMiniDump(EXCEPTION_POINTERS* pep);
	//   UnhandledExceptionFilter
	long __stdcall CrushDumpFilter(EXCEPTION_POINTERS* pep);
}
