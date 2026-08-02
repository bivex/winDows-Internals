#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <ctime>

// Hamming(7, 4) Error Correcting Code Engine
class Hamming74 {
public:
    // Encode 4 bits of data into a 7-bit codeword (d3 d2 d1 d0 -> p1 p2 d3 p4 d2 d1 d0)
    static uint8_t encode(uint8_t data4bit) {
        uint8_t d0 = (data4bit >> 0) & 1;
        uint8_t d1 = (data4bit >> 1) & 1;
        uint8_t d2 = (data4bit >> 2) & 1;
        uint8_t d3 = (data4bit >> 3) & 1;

        // Parity bits calculation (XOR parity matrix)
        uint8_t p1 = d0 ^ d1 ^ d3;
        uint8_t p2 = d0 ^ d2 ^ d3;
        uint8_t p4 = d1 ^ d2 ^ d3;

        // Position mapping:
        // Bit 1: p1, Bit 2: p2, Bit 3: d0, Bit 4: p4, Bit 5: d1, Bit 6: d2, Bit 7: d3
        uint8_t codeword = (p1 << 0) | (p2 << 1) | (d0 << 2) |
                           (p4 << 3) | (d1 << 4) | (d2 << 5) | (d3 << 6);
        return codeword;
    }

    // Decode 7-bit codeword, detect & correct single-bit error, return original 4 bits
    static uint8_t decodeAndCorrect(uint8_t& codeword, bool& hadError, int& errorPos) {
        uint8_t b1 = (codeword >> 0) & 1;
        uint8_t b2 = (codeword >> 1) & 1;
        uint8_t b3 = (codeword >> 2) & 1; // d0
        uint8_t b4 = (codeword >> 3) & 1;
        uint8_t b5 = (codeword >> 4) & 1; // d1
        uint8_t b6 = (codeword >> 5) & 1; // d2
        uint8_t b7 = (codeword >> 6) & 1; // d3

        // Calculate Syndrome (s1, s2, s4)
        uint8_t s1 = b1 ^ b3 ^ b5 ^ b7;
        uint8_t s2 = b2 ^ b3 ^ b6 ^ b7;
        uint8_t s4 = b4 ^ b5 ^ b6 ^ b7;

        // Error position equals syndrome integer value (1..7)
        errorPos = (s4 << 2) | (s2 << 1) | (s1 << 0);

        if (errorPos != 0) {
            hadError = true;
            // FIX: Flip the corrupted bit back to correct state!
            codeword ^= (1 << (errorPos - 1));
        } else {
            hadError = false;
        }

        // Extract corrected 4-bit data
        uint8_t corrected_d0 = (codeword >> 2) & 1;
        uint8_t corrected_d1 = (codeword >> 4) & 1;
        uint8_t corrected_d2 = (codeword >> 5) & 1;
        uint8_t corrected_d3 = (codeword >> 6) & 1;

        return (corrected_d3 << 3) | (corrected_d2 << 2) | (corrected_d1 << 1) | corrected_d0;
    }

    static void printBits(uint8_t val, int count) {
        for (int i = count - 1; i >= 0; --i) {
            std::cout << ((val >> i) & 1);
        }
    }
};

int main() {
    std::srand((unsigned int)std::time(NULL));

    std::cout << "========================================================\n";
    std::cout << "     ECC Hamming(7,4) Self-Healing Hardware PoC         \n";
    std::cout << "========================================================\n\n";

    // 1. Original 4-bit payload (e.g. 0b1011 = 11 decimal)
    uint8_t originalData = 0b1011;

    std::cout << "[1] Original Data Payload (4 bits) : ";
    Hamming74::printBits(originalData, 4);
    std::cout << " (Decimal: " << (int)originalData << ")\n";

    // 2. Encode to 7-bit codeword
    uint8_t encoded = Hamming74::encode(originalData);
    std::cout << "[2] Encoded ECC Codeword (7 bits)   : ";
    Hamming74::printBits(encoded, 7);
    std::cout << " [p1 p2 d0 p4 d1 d2 d3]\n\n";

    // 3. Simulate Random Bit Flip (Noise / Cosmic Ray / Memory Corruption)
    int flipBitIndex = (std::rand() % 7) + 1; // 1..7
    uint8_t corrupted = encoded ^ (1 << (flipBitIndex - 1));

    std::cout << "[!] HARDWARE NOISE INCIDENT (Bit Flip)!\n";
    std::cout << "    Flipping Bit #" << flipBitIndex << " in memory...\n";
    std::cout << "[3] Corrupted Memory State (7 bits) : ";
    Hamming74::printBits(corrupted, 7);
    std::cout << "\n\n";

    // 4. Hardware/Kernel Auto-Correction
    bool hadError = false;
    int errorPos = 0;
    uint8_t receivedCodeword = corrupted;
    uint8_t recoveredData = Hamming74::decodeAndCorrect(receivedCodeword, hadError, errorPos);

    std::cout << "[4] ECC Self-Healing Engine Output:\n";
    if (hadError) {
        std::cout << "    [+] Error Detected : YES!\n";
        std::cout << "    [+] Fault Location : Bit #" << errorPos << "\n";
        std::cout << "    [+] Corrected Memory: ";
        Hamming74::printBits(receivedCodeword, 7);
        std::cout << "\n";
    }

    std::cout << "\n[5] Final Recovered Data Payload    : ";
    Hamming74::printBits(recoveredData, 4);
    std::cout << " (Decimal: " << (int)recoveredData << ")\n\n";

    if (recoveredData == originalData) {
        std::cout << ">>> SUCCESS: Data 100% restored without retransmission! <<<\n";
    } else {
        std::cout << ">>> ERROR: Recovery failed! <<<\n";
    }

    return 0;
}
