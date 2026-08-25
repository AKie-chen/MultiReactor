#include "test_framework.h"
#include "ThreadPool.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>

// 等 inFlight 归零 = 所有已受理任务执行完毕。
// 同时顺带证明 worker 已进入 loop()——规避"quit 早于 loop() 进入 → join 挂起"
// 竞态（EventLoop::loop() 开头的 looping_=true 会覆盖提前的 quit()）。
// 注意：CHECK 只能在主线程（testfw::assertionFailures 非原子），
// 断言不能放进任务 lambda，一律 waitIdle + stop 后检查。
static void waitIdle(ThreadPool& pool) {
    while (pool.inFlight() != 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

TEST_CASE(ThreadPool_RunsAllTasks) {
    ThreadPool pool(4, 0);
    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) CHECK(pool.tryRun([&done] { done++; }));
    waitIdle(pool); // 确定性：单提交者，stop 前已全部执行
    CHECK_EQ(done.load(), 100);
    pool.stop();
    CHECK_EQ(pool.inFlight(), 0);
}

TEST_CASE(ThreadPool_BoundRejects) {
    ThreadPool pool(2, 1); // 在途上限 1（排队+执行）
    std::atomic<bool> release{false};
    CHECK(pool.tryRun([&release] { while (!release.load()) std::this_thread::yield(); }));
    CHECK(!pool.tryRun([]{})); // 提交即占额（fetch_add），无需等任务启动
    release.store(true);
    waitIdle(pool);
    pool.stop();
    CHECK_EQ(pool.inFlight(), 0);
}

TEST_CASE(ThreadPool_RRDistributes) {
    ThreadPool pool(4, 0);
    std::mutex m;
    std::map<std::thread::id, int> counts;
    for (int i = 0; i < 40; ++i)
        pool.tryRun([&m, &counts] {
            std::lock_guard<std::mutex> lk(m);
            counts[std::this_thread::get_id()]++;
        });
    waitIdle(pool);
    pool.stop(); // 先 stop 后读，无并发
    CHECK_EQ(counts.size(), 4);             // 4 个不同 worker 线程
    for (auto& [id, n] : counts) CHECK_EQ(n, 10); // RR 恰好每 loop 10 个
}

TEST_CASE(ThreadPool_StopKeepsQueueAlive) {
    ThreadPool pool(1, 0);
    std::atomic<int> warm{0};
    CHECK(pool.tryRun([&warm] { warm++; }));
    waitIdle(pool); // 确保 worker 已进入 loop()
    CHECK_EQ(warm.load(), 1);
    pool.stop();
    CHECK_EQ(pool.inFlight(), 0); // stop 归零（丢弃计数）
    std::atomic<bool> ran{false};
    // stop 后仍受理（loops_ 存活——不变式，对应 deadline 情形 IO 线程投递），
    // 但永不执行：无死循环、析构安全
    CHECK(pool.tryRun([&ran] { ran = true; }));
    CHECK(!ran.load());
}
