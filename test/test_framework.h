#pragma once
// 极简零依赖测试框架：TEST_CASE 注册 + CHECK/CHECK_EQ 断言。
// 无第三方依赖（项目原则），单测目标只有纯逻辑模块，不需要 mock/夹具框架。
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

// 静态初始化注册：TEST_CASE(name) 展开为一个静态函数 + 静态注册器
struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline int assertionFailures = 0;

template <typename A, typename B>
void checkEq(const char* file, int line, const char* sa, const char* sb,
             const A& a, const B& b) {
    if (a == b) return;
    ++assertionFailures;
    std::ostringstream os;
    os << "  FAIL " << file << ":" << line << "  " << sa << " == " << sb
       << " (got [" << a << "] vs [" << b << "])";
    std::printf("%s\n", os.str().c_str());
}

}  // namespace testfw

#define TEST_CASE(name)                                          \
    static void testfn_##name();                                 \
    static ::testfw::Registrar reg_##name(#name, testfn_##name); \
    static void testfn_##name()

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++::testfw::assertionFailures;                              \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

// CHECK_EQ(a, b)：两边需支持 operator<<（std::string/int/RouterResult 等）
#define CHECK_EQ(a, b) ::testfw::checkEq(__FILE__, __LINE__, #a, #b, (a), (b))
