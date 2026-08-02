#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include "../include/ecc_branchless.hpp"

int main() {
    std::srand((unsigned int)std::time(NULL));

    std::string message = "Header-Only ECC Library for High Reliability Systems!";
    std::cout << "Original Message: \"" << message << "\"\n";

    // 1. Encode string
    auto stream = ecc::FastHamming74::encodeString(message);
    std::cout << "Encoded Stream Size: " << stream.size() << " bytes\n";

    // 2. Inject random single-bit error into EVERY codeword
    for (auto& codeword : stream) {
        int bitPos = std::rand() % 7;
        codeword ^= (1 << bitPos);
    }

    // 3. Decode & Auto-Correct
    int corrections = 0;
    std::string restoredMessage = ecc::FastHamming74::decodeString(stream, corrections);

    std::cout << "Restored Message: \"" << restoredMessage << "\"\n";
    std::cout << "Total Auto-Corrections: " << corrections << "\n";
    std::cout << "Status: " << (message == restoredMessage ? "SUCCESS (100% Restored!)" : "FAILED") << "\n";

    return 0;
}
