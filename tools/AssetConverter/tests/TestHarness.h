#pragma once

#include <cstddef>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace Corsairs::Tools::AssetConverter::Testing {

struct TestCase {
    std::string_view Name;
    void (*Body)(bool&);
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(std::string_view name, void (*body)(bool&)) {
        Registry().push_back(TestCase{name, body});
    }
};

// Возвращает число провалившихся тестов; 0 — успех.
inline int RunAllTests() {
    std::size_t failed = 0;
    for (const TestCase& test : Registry()) {
        bool ok = true;
        test.Body(ok);
        if (ok) {
            std::cout << std::format("  PASS  {}\n", test.Name);
        }
        else {
            std::cout << std::format("  FAIL  {}\n", test.Name);
            ++failed;
        }
    }
    std::cout << std::format("\n{} тестов, провалено {}\n", Registry().size(), failed);
    return static_cast<int>(failed);
}

} // namespace Corsairs::Tools::AssetConverter::Testing

#define CORSAIRS_TEST(name)                                                        \
    static void name(bool& corsairsTestOk);                                        \
    static const ::Corsairs::Tools::AssetConverter::Testing::Registrar              \
        name##_registrar{#name, &name};                                            \
    static void name(bool& corsairsTestOk)

#define REQUIRE(cond)                                                              \
    do {                                                                           \
        if (!(cond)) {                                                             \
            corsairsTestOk = false;                                                \
            std::cout << std::format("        {}:{} REQUIRE({}) не выполнено\n",   \
                                     __FILE__, __LINE__, #cond);                   \
            return;                                                                \
        }                                                                          \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                               \
    do {                                                                           \
        const auto corsairsActual = (actual);                                      \
        const auto corsairsExpected = (expected);                                  \
        if (!(corsairsActual == corsairsExpected)) {                               \
            corsairsTestOk = false;                                                \
            std::cout << std::format("        {}:{} {} == {}: получено {}, ожидалось {}\n", \
                                     __FILE__, __LINE__, #actual, #expected,       \
                                     corsairsActual, corsairsExpected);            \
            return;                                                                \
        }                                                                          \
    } while (false)
