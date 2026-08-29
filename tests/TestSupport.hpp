#pragma once

#include <iostream>
#include <string_view>

inline int check(const bool condition, const std::string_view expression, const int line) {
    if (condition) {
        return 0;
    }
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    return 1;
}

#define MBS_CHECK(expression) failures += check((expression), #expression, __LINE__)
