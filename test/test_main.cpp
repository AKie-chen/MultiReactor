#include "test_framework.h"
#include "Log.h"
#include <cstdio>

int main() {
    Logger::setLevel(LogLevel::ERROR);  // 静默预期内的 WARN（如 StaticFileHandler 目录缺失/读失败场景）
    size_t failed = 0;
    for (auto& tc : testfw::registry()) {
        int before = testfw::assertionFailures;
        tc.fn();
        if (testfw::assertionFailures == before) {
            std::printf("[PASS] %s\n", tc.name);
        } else {
            std::printf("[FAIL] %s\n", tc.name);
            ++failed;
        }
    }
    std::printf("\n%zu/%zu test cases passed, %d assertion failures\n",
                testfw::registry().size() - failed, testfw::registry().size(),
                testfw::assertionFailures);
    return (failed == 0 && testfw::assertionFailures == 0) ? 0 : 1;
}
