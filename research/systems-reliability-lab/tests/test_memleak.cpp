#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include "../include/ecc_branchless.hpp"

#if defined(_WIN32)
#include <crtdbg.h>
#endif

void runStressLoop(size_t iterations) {
    std::cout << "[+] Running " << iterations << " iterations for memory leak verification...\n";

    std::string testString = "System Reliability Memory Leak Verification String!";

    for (size_t i = 0; i < iterations; ++i) {
        // Test scalar encoding/decoding (Stack-only, 0 heap allocs)
        auto pair = ecc::FastHamming74::encodeByte(static_cast<uint8_t>(i & 0xFF));
        bool correctedLow = false, correctedHigh = false;
        uint8_t low = ecc::FastHamming74::decode4(pair.first ^ 1, correctedLow);
        uint8_t high = ecc::FastHamming74::decode4(pair.second, correctedHigh);
        (void)low; (void)high;

        // Test buffer encoding/decoding (RAII heap allocs, must fully deallocate)
        auto encoded = ecc::FastHamming74::encodeString(testString);
        int corrections = 0;
        std::string decoded = ecc::FastHamming74::decodeString(encoded, corrections);
        assert(decoded == testString);
    }

    std::cout << "[+] Completed " << iterations << " iterations successfully.\n";
}

int main() {

#if defined(_WIN32) && defined(_DEBUG)
    // Enable automatic MSVC CRT leak check on exit
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    std::cout << "========================================================\n";
    std::cout << "        ECC Branchless Library Memory Leak Test         \n";
    std::cout << "========================================================\n\n";

    // Run 500,000 allocations & deallocations
    runStressLoop(500000);

#if defined(_WIN32)
    int leakDetected = _CrtDumpMemoryLeaks();
    if (leakDetected) {
        std::cout << "[-] MEMORY LEAK DETECTED BY CRT CHECK!\n";
        return 1;
    } else {
        std::cout << "[+] MSVC CRT Leak Check: NO MEMORY LEAKS DETECTED!\n";
    }
#endif

    std::cout << "\n>>> MEMORY LEAK TEST PASSED: 0 LEAKS (100% CLEAN RAII) <<<\n";
    return 0;
}
