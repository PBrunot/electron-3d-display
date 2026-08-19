C++ Best Practices

This document collects widely recommended C++ best practices from forums, expert recommendations, and modern C++ guidelines to improve code readability, safety, and maintainability.

## 1. Coding Style
- Follow consistent naming conventions:
  - `camelCase` or `snake_case` for variables and functions consistently.
  - `PascalCase` for classes and structs.
- Keep line length reasonable (80-120 characters), and use indentation for readability.
- Comment **why**, not **what**. Avoid redundant comments.

## 2. Modern C++ Concepts
- Prefer **RAII** (Resource Acquisition Is Initialization) to manage resources safely.
- Use **smart pointers** (`std::unique_ptr`, `std::shared_ptr`) over raw pointers when ownership semantics are needed.
- Avoid `new`/`delete` directly if possible—use containers or smart pointers.
- Utilize **`constexpr`**, **`noexcept`**, and **`[[nodiscard]]`** for modern safety and optimization.

## 3. Standard Library Usage
- Prefer `std::vector`, `std::array`, `std::string` over C-style arrays and strings.
- Use algorithms in `<algorithm>` instead of manual loops when possible:
  ```cpp
  std::sort(vec.begin(), vec.end());
  std::for_each(vec.begin(), vec.end(), [](int x){ std::cout << x << '
'; });
Use std::optional instead of sentinel values or null pointers to represent optional values.

## 4. Functions and Classes
Prefer small, single-responsibility functions.
Pass arguments by reference (const T&) when objects are large; pass by value for small/primitive types.
Avoid public data members; favor accessors/mutators or structured classes.
Mark functions as const whenever they don't modify object state.

## 5. Exception Safety
Use try/catch only around code that can actually throw.
Avoid catching ... unless rethrowing or logging.
Ensure resource cleanup—use RAII patterns to handle memory and file streams.
Don't use try/catch on ESP32 platforms

## 6. Multi-threading and Concurrency
Use C++ standard library threads (<thread>, <mutex>, <atomic>) for portability.
Avoid data races by using locks, std::atomic, or higher-level abstractions.
Minimize shared mutable state; favor immutable data when possible.

## 7. Performance Optimization
Measure first—profile before optimizing.
Prefer algorithmic efficiency over micro-optimizations.
Use emplace_back instead of push_back when constructing objects directly in containers.

## 8. Tools and Compilation
Enable compiler warnings (-Wall -Wextra in GCC/Clang) and treat warnings as errors during development.
Use static analysis tools like clang-tidy or cppcheck to detect issues early.
Write unit tests for critical functionality; use frameworks like Google Test.

Example:

#include <iostream>
#include <vector>
#include <algorithm>
#include <memory>

struct MyStruct {
    int value;
};

int main() {
    std::vector<std::unique_ptr<MyStruct>> vec;
    vec.emplace_back(std::make_unique<MyStruct>(MyStruct{10}));
    vec.emplace_back(std::make_unique<MyStruct>(MyStruct{20}));

    std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b){
        return a->value < b->value;
    });

    for (const auto& item : vec) {
        std::cout << item->value << std::endl;
    }
}