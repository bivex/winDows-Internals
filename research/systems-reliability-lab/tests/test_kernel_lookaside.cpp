#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include "../include/kernel_lookaside.hpp"

void testLookasidePoolBasic() {
    std::cout << "[TEST 1] Testing LockFreeLookasidePool allocation and deallocation...\n";
    kernel::LockFreeLookasidePool<uint64_t, 1024> pool;

    uint64_t* p1 = pool.allocate(100);
    uint64_t* p2 = pool.allocate(200);

    assert(p1 != nullptr && *p1 == 100);
    assert(p2 != nullptr && *p2 == 200);
    assert(pool.activeAllocations() == 2);

    pool.deallocate(p1);
    pool.deallocate(p2);
    assert(pool.activeAllocations() == 0);

    std::cout << " -> PASSED: Basic Lookaside Pool test succeeded!\n\n";
}

void testLookasidePoolMultithreaded() {
    std::cout << "[TEST 2] Testing LockFreeLookasidePool under 8 concurrent threads...\n";
    kernel::LockFreeLookasidePool<uint64_t, 65536> pool;

    const size_t THREADS = 8;
    const size_t ALLOCS_PER_THREAD = 5000;
    std::vector<std::thread> workers;

    for (size_t t = 0; t < THREADS; ++t) {
        workers.emplace_back([&pool, ALLOCS_PER_THREAD]() {
            for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i) {
                uint64_t* ptr = pool.allocate(i);
                assert(ptr != nullptr);
                pool.deallocate(ptr);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    assert(pool.activeAllocations() == 0);
    std::cout << " -> PASSED: Concurrent multithreaded allocations (40,000 ops) 100% verified!\n\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "  Kernel Lockaside Pool Automated Test Suite           \n";
    std::cout << "========================================================\n\n";

    testLookasidePoolBasic();
    testLookasidePoolMultithreaded();

    std::cout << ">>> ALL LOOKASIDE POOL TESTS PASSED SUCCESSFULLY! <<<\n";
    return 0;
}
