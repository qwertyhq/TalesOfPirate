#pragma once

//     LogManager.
//  std::format (C++20),         .

#define  WIN32_LEAN_AND_MEAN

#include <atomic>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <tuple>
// Windows-заголовки только на Windows; на POSIX — словарь совместимости.
#include "PlatformCompat.h"

// P2905 (C++26): std::make_format_args требует lvalue-аргументы.
// SafeVFormat принимает ар��ументы по значению (= lvalues внутри функции).
template <class... _Types>
std::string SafeVFormat(std::string_view _Fmt, _Types... _Args) {
	return std::vformat(_Fmt, std::make_format_args(_Args...));
}

namespace Corsairs::Util {
	//        
	enum class LogLevel {
		Trace,
		Debug,
		Info,
		Warning,
		Error,
		Fatal
	};

	//     ,    
	struct LogUtilEntry {
		char MessageTime[23]{};
		std::string Message{};
		LogLevel LogLevel{LogLevel::Debug};
		std::string LogSystem{};

		LogUtilEntry();
	};

	//      .
	//      .
	class LogStream {
		std::ofstream _logStream{};
		std::string _path{};
		std::string _logSystem{};
		SYSTEMTIME _currentSystemTime{};

		[[nodiscard]] std::string GenerateFileName() const;

	public:
		LogStream(const std::string& path, const std::string& logSystem);
		~LogStream();

		void Write(const LogUtilEntry& entry, const SYSTEMTIME& sysTime);
		void Flush();
		LogLevel MinimumLogLevel{LogLevel::Trace};
	};

	//  :  ,      .
	//      "common".
	//         stdout.
	class LogManager {
		std::map<std::string, std::shared_ptr<LogStream>> _channels{};
		std::mutex _queueMutex{};
		std::queue<LogUtilEntry> _logsQueue{};
		std::thread _logThread{};
		std::atomic<bool> _stopped{false};
		std::string _filePath{};
		bool _enabledGlobalConsole;

		//
		static void PrintConsoleMessage(const LogUtilEntry& logEntry);

		// Достать все записи из очереди и записать в каналы. Если flushAfter=true,
		// дополнительно сбросить буферы ofstream на диск. Дёргается из логгер-
		// потока (worker-loop + финальный drain после _stopped=true).
		void DrainQueue(bool flushAfter);

	public:
		LogManager();
		~LogManager();

		void InitLogger(const std::string& path);
		bool AddLogger(const std::string& loggerName);
		void EnableGlobalConsole(bool status);

		//    
		const std::string& GetLogDirectory() const { return _filePath; }

		void Shutdown();
		void
		InternalLog(LogLevel logLevel, const std::string& logSystem, const std::string& value);
		void InternalLog(const LogUtilEntry& logEntry);

		template <class... _Types>
		void LogTrace(const std::string& logSystem,
					  std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
			InternalLog(LogLevel::Trace, logSystem, std::format(_Fmt, std::forward<_Types>(_Args)...));
		}

		template <class... _Types>
		void LogDebug(const std::string& logSystem,
					  std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
			InternalLog(LogLevel::Debug, logSystem, std::format(_Fmt, std::forward<_Types>(_Args)...));
		}

		template <class... _Types>
		void LogInfo(const std::string& logSystem,
					 std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
			InternalLog(LogLevel::Info, logSystem, std::format(_Fmt, std::forward<_Types>(_Args)...));
		}

		template <class... _Types>
		void LogWarning(const std::string& logSystem,
						std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
			InternalLog(LogLevel::Warning, logSystem, std::format(_Fmt, std::forward<_Types>(_Args)...));
		}

		template <class... _Types>
		void LogError(const std::string& logSystem,
					  std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
			InternalLog(LogLevel::Error, logSystem, std::format(_Fmt, std::forward<_Types>(_Args)...));
		}

		template <class... _Types>
		void LogFatal(const std::string& logSystem,
					  std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
			InternalLog(LogLevel::Fatal, logSystem, std::format(_Fmt, std::forward<_Types>(_Args)...));
		}
	};

	//    
	extern LogManager g_logManager;

	//        Debug
	template <class... _Types>
	void ToLogService(const std::string& logger,
					  std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
		g_logManager.InternalLog(LogLevel::Debug, logger, std::format(_Fmt, std::forward<_Types>(_Args)...));
	}

	//
	template <class... _Types>
	void ToLogService(const std::string& logger, LogLevel level,
					  std::format_string<std::type_identity_t<_Types>...> _Fmt, _Types&&... _Args) {
		g_logManager.InternalLog(level, logger, std::format(_Fmt, std::forward<_Types>(_Args)...));
	}
}

//   LogLevel  ostream (  LogStream::Write)
std::ostream& operator<<(std::ostream& stream, const Corsairs::Util::LogLevel& io);

//  std::formatter  LogLevel ( std::format)
template <>
struct std::formatter<Corsairs::Util::LogLevel> : std::formatter<std::string> {
	template <class format_context>
	auto format(const Corsairs::Util::LogLevel& io, format_context& ctx) const {
		switch (io) {
		case Corsairs::Util::LogLevel::Trace:
			return formatter<string>::format("Trace", ctx);
		case Corsairs::Util::LogLevel::Debug:
			return formatter<string>::format("Debug", ctx);
		case Corsairs::Util::LogLevel::Info:
			return formatter<string>::format("Info", ctx);
		case Corsairs::Util::LogLevel::Warning:
			return formatter<string>::format("Warning", ctx);
		case Corsairs::Util::LogLevel::Error:
			return formatter<string>::format("Error", ctx);
		case Corsairs::Util::LogLevel::Fatal:
			return formatter<string>::format("Fatal", ctx);
		}

		return formatter<string>::format("UNKNOWN", ctx);
	}
};

// CLAUDE.md запрещает `using namespace ...` в глобальной области .h, поэтому
// проброс делается per-name. 2300+ callsites используют эти имена без квалификации;
// возможный follow-up — отказаться от шима и перейти на полную квалификацию.
using Corsairs::Util::LogLevel;
using Corsairs::Util::LogManager;
using Corsairs::Util::LogStream;
using Corsairs::Util::LogUtilEntry;
using Corsairs::Util::g_logManager;
using Corsairs::Util::ToLogService;
