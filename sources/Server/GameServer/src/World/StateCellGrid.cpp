// StateCellGrid.cpp — реализации ранее inline-методов из StateCellGrid.h.
#include "Core/stdafx.h"
#include "World/StateCellGrid.h"

#include <cassert>
#include <format>
#include <iostream>
// <stacktrace> из C++23 есть в libstdc++ (Linux, GCC 14+), но отсутствует в
// libc++ на macOS. Проверяем наличие, а не платформу: как только реализация
// появится, ветка включится сама.
#if __has_include(<stacktrace>)
#include <stacktrace>
#define CORSAIRS_HAS_STACKTRACE 1
#endif

void StateCellGrid::Init(short linCount, short colCount) {
	_linCount = linCount;
	_colCount = colCount;
	_cells.assign(static_cast<size_t>(linCount) * colCount, nullptr);
}

CStateCell* StateCellGrid::Get(short x, short y) const {
	assert(IsValidCoord(x, y));
	return _cells[static_cast<size_t>(y) * _colCount + x];
}

void StateCellGrid::Set(short x, short y, CStateCell* cell) {
	assert(IsValidCoord(x, y));
	_cells[static_cast<size_t>(y) * _colCount + x] = cell;
}

CStateCell** StateCellGrid::SlotAddress(short x, short y) {
	assert(IsValidCoord(x, y));
	return &_cells[static_cast<size_t>(y) * _colCount + x];
}

bool StateCellGrid::IsValidCoord(short x, short y) const {
	if (x >= 0 && x < _colCount && y >= 0 && y < _linCount) {
		return true;
	}
	auto msg = std::format(
		"StateCellGrid: invalid coords ({}, {}), valid range: x=[0, {}), y=[0, {})\nStacktrace:\n{}",
		x, y, _colCount, _linCount,
#if defined(CORSAIRS_HAS_STACKTRACE)
			std::stacktrace::current()
#else
			// Без <stacktrace> сообщение остаётся информативным: координаты и
			// размеры сетки указывают на место не хуже, чем стек.
			"(стек недоступен: нет <stacktrace>)"
#endif
		);
	OutputDebugStringA(msg.c_str());
	std::cerr << msg << std::endl;
	return false;
}

short StateCellGrid::GetLinCount() const { return _linCount; }
short StateCellGrid::GetColCount() const { return _colCount; }
