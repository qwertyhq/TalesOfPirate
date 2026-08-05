//      Windows.
//  SetConsoleTextAttribute    .

#include "ConsoleColor.h"
#include <algorithm>
// Windows-заголовки только на Windows; на POSIX — словарь совместимости.
#include "PlatformCompat.h"

namespace Corsairs::Util {

    // CODES/NAMES — function-local statics (Meyers' singleton), не namespace-scope.
    // Why: namespace-scope `const std::map` без inline даёт per-TU копию (internal
    // linkage const) и непредсказуемый destruction order. При shutdown logger thread
    // в DrainQueue звал stoc() → CODES.find() уже после деструкции CODES → crash.
    // С function-local static порядок init/destruction контролируем через ForceInit().
    static const std::map<std::string, int>& Codes() {
        static const std::map<std::string, int> codes = {
            {"black", 0}, {"k", 0},
            {"blue", 1}, {"b", 1},
            {"green", 2}, {"g", 2},
            {"aqua", 3}, {"a", 3},
            {"red", 4}, {"r", 4},
            {"purple", 5}, {"p", 5},
            {"yellow", 6}, {"y", 6},
            {"white", 7}, {"w", 7},
            {"grey", 8}, {"e", 8},
            {"light blue", 9}, {"lb", 9},
            {"light green", 10}, {"lg", 10},
            {"light aqua", 11}, {"la", 11},
            {"light red", 12}, {"lr", 12},
            {"light purple", 13}, {"lp", 13},
            {"light yellow", 14}, {"ly", 14},
            {"bright white", 15}, {"bw", 15}
        };
        return codes;
    }

    static const std::map<int, std::string>& Names() {
        static const std::map<int, std::string> names = {
            {0, "black"}, {1, "blue"}, {2, "green"}, {3, "aqua"},
            {4, "red"}, {5, "purple"}, {6, "yellow"}, {7, "white"},
            {8, "grey"}, {9, "light blue"}, {10, "light green"},
            {11, "light aqua"}, {12, "light red"}, {13, "light purple"},
            {14, "light yellow"}, {15, "bright white"}
        };
        return names;
    }

    void ForceInit() {
        // Касаемся обоих singleton-ов чтобы они инициализировались до вызывающего —
        // это переносит их в destruction-list ПЕРЕД вызывающим объектом
        // (function-local static destruction = reverse of init order).
        (void)Codes();
        (void)Names();
    }

    int stoc(std::string a) {
        //    ,  '_'  '-'
        std::transform(a.begin(), a.end(), a.begin(), [](char c) {
            if ('A' <= c && c <= 'Z')
                c = c - 'A' + 'a';
            else if (c == '_' || c == '-')
                c = ' ';
            return c;
        });

        const auto& codes = Codes();
        auto it = codes.find(a);
        return (it != codes.end()) ? it->second : BAD_COLOR;
    }

    int stoc(std::string a, std::string b) {
        return itoc(stoc(a), stoc(b));
    }

    std::string ctos(int c) {
        if (c < 0 || c >= 256) {
            return "BAD COLOR";
        }
        const auto& names = Names();
        return "(text) " + names.at(c % 16) + " + " + "(background) " + names.at(c / 16);
    }

    int get() {
        CONSOLE_SCREEN_BUFFER_INFO i;
        return GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i) ? i.wAttributes : BAD_COLOR;
    }

    int get_text() {
        return (get() != BAD_COLOR) ? get() % 16 : BAD_COLOR;
    }

    int get_background() {
        return (get() != BAD_COLOR) ? get() / 16 : BAD_COLOR;
    }

    void set(int c) {
        if (is_good(c)) {
            SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), static_cast<WORD>(c));
        }
    }

    void set(int a, int b) {
        set(a + b * 16);
    }

    void set(std::string a, std::string b) {
        set(stoc(a) + stoc(b) * 16);
    }

    void set_text(std::string a) {
        set(stoc(a), get_background());
    }

    void set_background(std::string b) {
        set(get_text(), stoc(b));
    }

    void reset() {
        set(DEFAULT_COLOR);
    }

    int invert(int c) {
        if (is_good(c)) {
            int a = c % 16;
            int b = c / 16;
            return b + a * 16;
        } else
            return BAD_COLOR;
    }

    std::ostream &reset(std::ostream &os) {
        reset();
        return os;
    }

    std::ostream &black(std::ostream &os) {
        set_text("k");
        return os;
    }

    std::ostream &blue(std::ostream &os) {
        set_text("b");
        return os;
    }

    std::ostream &green(std::ostream &os) {
        set_text("g");
        return os;
    }

    std::ostream &aqua(std::ostream &os) {
        set_text("a");
        return os;
    }

    std::ostream &bright_white_on_light_yellow(std::ostream &os) {
        set("bw", "ly");
        return os;
    }

    std::ostream &bright_white_on_bright_white(std::ostream &os) {
        set("bw", "bw");
        return os;
    }

}
