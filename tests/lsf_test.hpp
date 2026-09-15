#pragma once

// Minimal, dependency free test harness for the Link State Fabric suite.
//
// The harness deliberately has no timeouts and no watchdogs: a hanging test is
// a defect in the runtime under test, and it must be diagnosed rather than
// masked by terminating the process.

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace lsf_test {

struct Case {
  std::string name;
  void (*function)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

inline int& check_count() {
  static int checks = 0;
  return checks;
}

inline std::string& current_case() {
  static std::string name;
  return name;
}

struct Registrar {
  Registrar(const char* name, void (*function)()) { registry().push_back(Case{name, function}); }
};

inline bool check(bool condition, const char* expression, const char* file, int line) {
  ++check_count();
  if (!condition) {
    ++failure_count();
    std::printf("FAIL %s\n  %s:%d\n  expression: %s\n", current_case().c_str(), file, line,
                expression);
    std::fflush(stdout);
  }
  return condition;
}

inline void fail(const char* message, const char* file, int line) {
  ++check_count();
  ++failure_count();
  std::printf("FAIL %s\n  %s:%d\n  %s\n", current_case().c_str(), file, line, message);
  std::fflush(stdout);
}

struct Abort {};

inline void require(bool condition, const char* expression, const char* file, int line) {
  if (!check(condition, expression, file, line)) {
    throw Abort{};
  }
}

inline int run_all() {
  int executed = 0;
  for (const Case& test : registry()) {
    current_case() = test.name;
    const int before = failure_count();
    try {
      test.function();
    } catch (const Abort&) {
      // The failure was already reported by require().
    } catch (const std::exception& error) {
      fail(error.what(), "test case", 0);
    } catch (...) {
      fail("test case threw a non standard exception", "test case", 0);
    }
    ++executed;
    const int after = failure_count();
    std::printf("%-70s %s\n", test.name.c_str(), after == before ? "ok" : "FAILED");
    std::fflush(stdout);
  }
  std::printf("\n%d test cases executed, %d checks, %d failures\n", executed, check_count(),
              failure_count());
  std::fflush(stdout);
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace lsf_test

#define LSF_TEST(name)                                                     \
  static void name();                                                      \
  static const ::lsf_test::Registrar lsf_registrar_##name(#name, &name);   \
  static void name()

#define LSF_CHECK(condition)   ::lsf_test::check((condition), #condition, __FILE__, __LINE__)

#define LSF_REQUIRE(condition)   ::lsf_test::require((condition), #condition, __FILE__, __LINE__)

#define LSF_CHECK_EQ(lhs, rhs)                                                          \
  do {                                                                                  \
    const auto& lsf_lhs = (lhs);                                                        \
    const auto& lsf_rhs = (rhs);                                                        \
    if (!::lsf_test::check(lsf_lhs == lsf_rhs, #lhs " == " #rhs, __FILE__, __LINE__)) {  \
      std::printf("  lhs: %s\n", ::lsf_test::render_value(lsf_lhs).c_str());            \
      std::printf("  rhs: %s\n", ::lsf_test::render_value(lsf_rhs).c_str());            \
      std::fflush(stdout);                                                              \
    }                                                                                   \
  } while (false)

namespace lsf_test {

template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
inline std::string render_value(T value) {
  return std::to_string(value);
}

template <class T, std::enable_if_t<!std::is_integral_v<T>, int> = 0>
inline std::string render_value(const T& value) {
  return value.to_string();
}

inline std::string render_value(bool value) { return value ? "true" : "false"; }
inline std::string render_value(const std::string& value) { return value; }
inline std::string render_value(const char* value) { return value == nullptr ? "<null>" : value; }

}  // namespace lsf_test
