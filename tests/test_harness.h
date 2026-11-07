// test_harness.h — a minimal Catch2-style test harness with no external
// dependencies (the plugin's own rule: nothing beyond Win32 + C++17 leaks
// into the build). One process runs all tests; failures print and count.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tvtest {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> t;
    return t;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

template <typename Fn>
inline bool registerTest(const char* name, Fn fn)
{
    registry().push_back({name, std::function<void()>(fn)});
    return true;
}

inline int runAll()
{
    for (const TestCase& t : registry()) {
        std::printf("[ RUN  ] %s\n", t.name);
        std::fflush(stdout);
        const int before = failures();
        t.fn();
        if (failures() == before)
            std::printf("[  OK  ] %s\n", t.name);
        else
            std::printf("[ FAIL ] %s\n", t.name);
        std::fflush(stdout);
    }
    std::printf("\n%s (%d failure%s)\n", failures() == 0 ? "all tests passed" : "TESTS FAILED",
        failures(), failures() == 1 ? "" : "s");
    return failures() == 0 ? 0 : 1;
}

} // namespace tvtest

#define TV_CONCAT_(a, b) a##b
#define TV_CONCAT(a, b) TV_CONCAT_(a, b)
#define TEST(name)                                                                                \
    static void TV_CONCAT(tv_test_fn_, __LINE__)();                                               \
    static const bool TV_CONCAT(tv_test_reg_, __LINE__) =                                         \
        tvtest::registerTest(name, []() { TV_CONCAT(tv_test_fn_, __LINE__)(); });                 \
    static void TV_CONCAT(tv_test_fn_, __LINE__)()

#define CHECK(cond)                                                                              \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            ++tvtest::failures();                                                                 \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
        }                                                                                         \
    } while (0)

#define CHECK_EQ(a, b)                                                                            \
    do {                                                                                          \
        const auto va_ = (a);                                                                     \
        const auto vb_ = (b);                                                                     \
        if (!(va_ == vb_)) {                                                                      \
            ++tvtest::failures();                                                                 \
            std::printf("    FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                \
        }                                                                                         \
    } while (0)
