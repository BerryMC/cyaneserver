#pragma once

#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace cyane::test {

using TestFn = void (*)();

struct Case {
    std::string_view name;
    TestFn fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline std::uint64_t& failure_count() {
    static std::uint64_t count = 0;
    return count;
}

struct Registrar {
    Registrar(std::string_view name, TestFn fn) { registry().push_back(Case{name, fn}); }
};

inline void report_failure(std::string_view file, int line, std::string_view expr, std::string_view detail) {
    ++failure_count();
    if (detail.empty()) {
        std::print(stderr, "    FAIL {}:{}  {}\n", file, line, expr);
    } else {
        std::print(stderr, "    FAIL {}:{}  {}  ({})\n", file, line, expr, detail);
    }
}

inline int run_all() {
    std::uint64_t failed_cases = 0;
    for (const auto& test_case : registry()) {
        const std::uint64_t before = failure_count();
        try {
            test_case.fn();
        } catch (const std::exception& error) {
            report_failure("<test>", 0, "unhandled exception", error.what());
        } catch (...) {
            report_failure("<test>", 0, "unhandled unknown exception", "");
        }
        const bool ok = failure_count() == before;
        if (!ok) {
            ++failed_cases;
        }
        std::print("  {:<44} {}\n", test_case.name, ok ? "ok" : "FAILED");
    }
    std::print("\n{} tests, {} failed, {} assertions failed\n", registry().size(), failed_cases, failure_count());
    return failed_cases == 0 ? 0 : 1;
}

template <typename L, typename R>
void check_eq(const L& lhs, const R& rhs, std::string_view file, int line, std::string_view expr) {
    if (lhs == rhs) {
        return;
    }
    if constexpr (std::formattable<L, char> && std::formattable<R, char>) {
        report_failure(file, line, expr, std::format("lhs={} rhs={}", lhs, rhs));
    } else {
        report_failure(file, line, expr, "values differ");
    }
}

}

#define CYANE_TEST(name)                                                                       \
    static void name();                                                                        \
    namespace {                                                                                \
    [[maybe_unused]] const ::cyane::test::Registrar cyane_registrar_##name{#name, &name};       \
    }                                                                                          \
    static void name()

#define CYANE_CHECK(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            ::cyane::test::report_failure(__FILE__, __LINE__, #expr, "");     \
        }                                                                     \
    } while (false)

#define CYANE_CHECK_EQ(lhs, rhs)                                                                    \
    do {                                                                                            \
        ::cyane::test::check_eq((lhs), (rhs), __FILE__, __LINE__, #lhs " == " #rhs);                 \
    } while (false)

#define CYANE_CHECK_NEAR(actual, expected, tolerance)                                                \
    do {                                                                                             \
        const double cyane_actual = static_cast<double>(actual);                                      \
        const double cyane_expected = static_cast<double>(expected);                                  \
        if (!(cyane_actual >= cyane_expected - (tolerance) && cyane_actual <= cyane_expected + (tolerance))) { \
            ::cyane::test::report_failure(__FILE__, __LINE__, #actual " ~= " #expected,               \
                                          std::format("actual={} expected={} tol={}", cyane_actual,   \
                                                      cyane_expected, tolerance));                     \
        }                                                                                            \
    } while (false)
