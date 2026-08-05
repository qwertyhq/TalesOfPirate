// POSIX-версия CrushSystem.
//
// Оригинал построен на механизмах Windows: структурная обработка исключений
// (SEH), MiniDumpWriteDump из dbghelp.dll, _set_invalid_parameter_handler из
// CRT Microsoft. Ни одного аналога в POSIX нет — там аварии приходят
// сигналами (SIGSEGV, SIGABRT, SIGBUS), а дампы пишет ядро по настройке
// core_pattern либо внешний сборщик.
//
// Здесь — минимальные реализации, чтобы сервер собирался и запускался. Полный
// обработчик аварий на сигналах с backtrace — отдельная задача; пока падение
// диагностируется штатным core dump.

#include "CrushSystem.h"

#include <csignal>
#include <cstdio>
#include <pthread.h>
#include <string>

namespace Corsairs::Util {

void SetPerThreadCRTExceptionBehavior() {
	// В CRT Microsoft это отключает диалог «программа перестала работать» и
	// перехват некорректных параметров. В POSIX такого механизма нет:
	// некорректные аргументы дают errno или сигнал.
}

void SetGlobalCRTExceptionBehavior() {
	// То же самое на уровне процесса. Оставлено пустым намеренно.
}

void SetupDumpSetting(const std::string& /*dumpPath*/) {
	// Windows-версия регистрирует обработчик, пишущий minidump. В POSIX за
	// это отвечает ядро: путь и формат задаются через core_pattern (Linux)
	// или /cores (macOS), а не процессом.
}

void SetupDumpSetting(const std::string& dumpPath,
					  const std::function<void()>& /*function*/) {
	SetupDumpSetting(dumpPath);
}

} // namespace Corsairs::Util

void SetThreadName(const std::string& name) {
	// pthread_setname_np на macOS принимает только имя и задаёт его текущему
	// потоку; на Linux сигнатура с дескриптором и лимит 16 байт вместе с
	// завершающим нулём.
#if defined(__APPLE__)
	::pthread_setname_np(name.c_str());
#elif defined(__linux__)
	const std::string trimmed = name.substr(0, 15);
	::pthread_setname_np(::pthread_self(), trimmed.c_str());
#endif
}
