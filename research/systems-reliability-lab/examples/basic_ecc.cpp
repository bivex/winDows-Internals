#include <iostream>
#include <iomanip>
#include <bitset>
#include "../include/ecc_branchless.hpp"

int main() {
    std::cout << "--- Basic ECC Usage Example ---\n";

    uint8_t dataNibble = 0b1011; // 11
    uint8_t codeword = ecc::FastHamming74::encode4(dataNibble);

    std::cout << "Original Data Nibble: 0b1011 (Decimal 11)\n";
    std::cout << "Encoded Codeword     : " << std::bitset<7>(codeword) << "\n";

    // Inject 1 bit flip at bit position 3
    uint8_t corruptedCodeword = codeword ^ (1 << 2);
    std::cout << "Corrupted Codeword   : " << std::bitset<7>(corruptedCodeword) << " (Bit #3 flipped)\n";

    bool corrected = false;
    uint8_t restored = ecc::FastHamming74::decode4(corruptedCodeword, corrected);

    std::cout << "Restored Data Nibble: 0b" << std::bitset<4>(restored) << " (Corrected: " << (corrected ? "YES" : "NO") << ")\n";

    return 0;
}
