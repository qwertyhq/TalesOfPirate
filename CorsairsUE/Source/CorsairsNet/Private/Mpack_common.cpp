// Втягивает mpack-common.cpp из sources/Libraries в сборку модуля.
//
// Каждый исходник mpack компилируется отдельной единицей трансляции: собранные
// в одну, они переопределяют MPACK_EMIT_INLINE_DEFS и сборка падает.
//
// Файлы лежат в каталоге include, а не src — так их разложил автор библиотеки.
#include "CorsairsNet/include/mpack-common.cpp"
