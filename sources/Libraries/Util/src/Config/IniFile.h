#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace Corsairs::Util {
	struct IniItem {
		std::string name;
		std::string value;
	};

	class IniSection {
		friend class IniFile;

	public:
		int ItemCount() const {
			return static_cast<int>(m_items.size());
		}

		const std::string& GetName() const {
			return m_name;
		}

		// Получить строковое значение. Если ключ не найден — возвращает defaultValue.
		std::string GetString(std::string_view key, std::string_view defaultValue = "") const;

		// Получить целочисленное значение. Если ключ не найден или не парсится — возвращает defaultValue.
		int64_t GetInt64(std::string_view key, int64_t defaultValue = 0) const;

		// Установить значение ключа (создаёт если не существует).
		void SetString(std::string_view key, std::string_view value);
		void SetInt64(std::string_view key, int64_t value);

	private:
		std::string m_name;
		std::vector<IniItem> m_items;

		IniItem* FindItem(std::string_view key);
		const IniItem* FindItem(std::string_view key) const;
	};

	class IniFile {
	public:
		explicit IniFile(std::string_view filename = "");
		~IniFile() = default;

		IniFile(const IniFile&) = delete;
		IniFile& operator=(const IniFile&) = delete;
		IniFile(IniFile&&) = default;
		IniFile& operator=(IniFile&&) = default;

		int SectCount() const {
			return static_cast<int>(m_sections.size());
		}

		// Получить секцию по имени. Если не найдена — создаёт пустую.
		IniSection& operator[](std::string_view sectname);

		// Доступ по порядковому номеру, для обхода всех секций.
		//
		// Именованный метод, а не перегрузка operator[]: с целочисленной
		// перегрузкой вызов `cfg[0]` стал бы неоднозначным — литерал 0
		// одинаково подходит и под int, и под const char* для string_view.
		IniSection& SectionAt(int index) {
			return m_sections.at(static_cast<std::size_t>(index));
		}

		const IniSection& SectionAt(int index) const {
			return m_sections.at(static_cast<std::size_t>(index));
		}

		// Сохранить в файл (по умолчанию — в тот же, из которого загружали).
		void Save(std::string_view filename = "") const;

	private:
		void ReadFile(std::string_view filename);

		std::vector<IniSection> m_sections;
		std::string m_filename;
	};
} // namespace Corsairs::Util
