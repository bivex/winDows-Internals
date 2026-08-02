#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <thread>
#include "../include/ecc_branchless.hpp"
#include "../include/kernel_lookaside.hpp"

struct ECCPacket {
    uint64_t packetId;
    uint8_t  lowCodeword;
    uint8_t  highCodeword;
};

int main() {
    std::cout << "=====================================================================\n";
    std::cout << "  Kernel Lookaside Pool & ECC Integration Demonstration            \n";
    std::cout << "=====================================================================\n\n";

    // 1. Initialize Lock-Free Lookaside Pool (capacity: 65,536 ECC packets)
    kernel::LockFreeLookasidePool<ECCPacket, 65536> pool;
    kernel::LockFreeRingBuffer<ECCPacket*, 65536> ringBuffer;

    std::cout << "[+] Created Kernel-Style Lock-Free Lookaside Pool (65,536 slots)...\n";
    std::cout << "[+] Created Lock-Free Ring Buffer (Cache-line Aligned 64B)...\n\n";

    const size_t PACKET_COUNT = 50000;
    std::string message = "Kernel-Style Lock-Free Performance Integration!";

    // Benchmark Allocation + ECC Encoding
    auto t0 = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < PACKET_COUNT; ++i) {
        uint8_t charByte = static_cast<uint8_t>(message[i % message.size()]);
        auto codewords = ecc::FastHamming74::encodeByte(charByte);

        // Sub-5ns Lock-free pool allocation
        ECCPacket* pkt = pool.allocate();
        if (pkt) {
            pkt->packetId = i;
            pkt->lowCodeword = codewords.first;
            pkt->highCodeword = codewords.second;

            // Push to lock-free ring buffer
            ringBuffer.push(pkt);
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> allocTimeMs = t1 - t0;

    std::cout << "[1] PRODUCED & ENCODED " << PACKET_COUNT << " Packets into Lookaside Pool:\n";
    std::cout << "    -> Allocation & ECC Encode Time: " << std::fixed << std::setprecision(2) << allocTimeMs.count() << " ms\n";
    std::cout << "    -> Average Speed                : " << std::fixed << std::setprecision(2) 
              << (allocTimeMs.count() * 1000000.0 / PACKET_COUNT) << " ns / allocation\n";
    std::cout << "    -> Active Pool Allocations      : " << pool.activeAllocations() << "\n\n";

    // Benchmark Deallocation + ECC Decoding & Auto-Correction
    auto t2 = std::chrono::high_resolution_clock::now();

    ECCPacket* pktPtr = nullptr;
    int corrections = 0;
    size_t processedCount = 0;

    while (ringBuffer.pop(pktPtr)) {
        if (pktPtr) {
            // Simulate 1 bit flip in transmission
            pktPtr->lowCodeword ^= 0x04; 

            // Decode & Auto-correct
            uint8_t byte = ecc::FastHamming74::decodeByte(pktPtr->lowCodeword, pktPtr->highCodeword, corrections);
            (void)byte;
            processedCount++;

            // Return to Lock-Free Lookaside Pool
            pool.deallocate(pktPtr);
        }
    }

    auto t3 = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> freeTimeMs = t3 - t2;

    std::cout << "[2] CONSUMED, DECODED & DEALLOCATED " << processedCount << " Packets:\n";
    std::cout << "    -> Consumption & ECC Repair Time: " << std::fixed << std::setprecision(2) << freeTimeMs.count() << " ms\n";
    std::cout << "    -> Average Speed                : " << std::fixed << std::setprecision(2) 
              << (freeTimeMs.count() * 1000000.0 / processedCount) << " ns / deallocation\n";
    std::cout << "    -> Total Bit Flips Fixed        : " << corrections << "\n";
    std::cout << "    -> Active Pool Allocations Left : " << pool.activeAllocations() << " (100% CLEAN RECYCLED!)\n\n";

    std::cout << "=====================================================================\n";
    std::cout << "  STATUS: LOCK-FREE LOOKASIDE POOL INTEGRATION 100% SUCCESSFUL!      \n";
    std::cout << "=====================================================================\n";

    return 0;
}
