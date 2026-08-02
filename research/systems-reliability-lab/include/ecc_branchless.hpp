/**
 * @file ecc_branchless.hpp
 * @brief High-Performance Header-Only Branchless ECC & Hamming(7,4) Library (with ARM NEON SIMD)
 * @version 1.1.0
 */

#ifndef FAST_ECC_BRANCHLESS_HPP
#define FAST_ECC_BRANCHLESS_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <utility>

#if defined(_M_ARM64) || defined(__aarch64__)
#include <arm_neon.h>
#define FAST_ECC_HAS_NEON 1
#endif

namespace ecc {

class FastHamming74 {
public:
    /**
     * @brief Encodes a 4-bit nibble into a 7-bit Hamming codeword
     */
    static inline uint8_t encode4(uint8_t nibble) noexcept {
        uint32_t d0 = (nibble >> 0) & 1;
        uint32_t d1 = (nibble >> 1) & 1;
        uint32_t d2 = (nibble >> 2) & 1;
        uint32_t d3 = (nibble >> 3) & 1;

        uint32_t p1 = d0 ^ d1 ^ d3;
        uint32_t p2 = d0 ^ d2 ^ d3;
        uint32_t p4 = d1 ^ d2 ^ d3;

        return static_cast<uint8_t>((p1) | (p2 << 1) | (d0 << 2) | (p4 << 3) | (d1 << 4) | (d2 << 5) | (d3 << 6));
    }

    /**
     * @brief Branchless decoding and single-bit error correction (Scalar 4-bit)
     */
    static inline uint8_t decode4(uint8_t codeword, bool& corrected) noexcept {
        uint32_t c = codeword;
        uint32_t b1 = (c >> 0) & 1;
        uint32_t b2 = (c >> 1) & 1;
        uint32_t b3 = (c >> 2) & 1;
        uint32_t b4 = (c >> 3) & 1;
        uint32_t b5 = (c >> 4) & 1;
        uint32_t b6 = (c >> 5) & 1;
        uint32_t b7 = (c >> 6) & 1;

        uint32_t s1 = b1 ^ b3 ^ b5 ^ b7;
        uint32_t s2 = b2 ^ b3 ^ b6 ^ b7;
        uint32_t s4 = b4 ^ b5 ^ b6 ^ b7;

        uint32_t syndrome = (s4 << 2) | (s2 << 1) | s1;

        // Pure Branchless Correction Mask
        uint32_t mask = (syndrome != 0) ? (1U << (syndrome - 1)) : 0U;
        corrected = (syndrome != 0);
        c ^= mask;

        return static_cast<uint8_t>(((c >> 6) & 1) << 3 |
                                    ((c >> 5) & 1) << 2 |
                                    ((c >> 4) & 1) << 1 |
                                    ((c >> 2) & 1));
    }

    /**
     * @brief Encodes an 8-bit byte into two 7-bit codewords
     */
    static inline std::pair<uint8_t, uint8_t> encodeByte(uint8_t byte) noexcept {
        return { encode4(byte & 0x0F), encode4((byte >> 4) & 0x0F) };
    }

    /**
     * @brief Decodes two 7-bit codewords into one corrected 8-bit byte
     */
    static inline uint8_t decodeByte(uint8_t lowCode, uint8_t highCode, int& totalCorrections) noexcept {
        bool corrLow = false, corrHigh = false;
        uint8_t lowNibble = decode4(lowCode, corrLow);
        uint8_t highNibble = decode4(highCode, corrHigh);

        if (corrLow) totalCorrections++;
        if (corrHigh) totalCorrections++;

        return static_cast<uint8_t>((highNibble << 4) | lowNibble);
    }

#if FAST_ECC_HAS_NEON
    /**
     * @brief SIMD 128-bit Vectorized Decoding over 16 Codewords (ARM NEON)
     * Processes 16 codewords simultaneously in 128-bit vector register
     */
    static inline uint8x16_t decodeSIMD16(uint8x16_t vecCode) noexcept {
        uint8x16_t b1 = vandq_u8(vecCode, vdupq_n_u8(1));
        uint8x16_t b2 = vandq_u8(vshrq_n_u8(vecCode, 1), vdupq_n_u8(1));
        uint8x16_t b3 = vandq_u8(vshrq_n_u8(vecCode, 2), vdupq_n_u8(1));
        uint8x16_t b4 = vandq_u8(vshrq_n_u8(vecCode, 3), vdupq_n_u8(1));
        uint8x16_t b5 = vandq_u8(vshrq_n_u8(vecCode, 4), vdupq_n_u8(1));
        uint8x16_t b6 = vandq_u8(vshrq_n_u8(vecCode, 5), vdupq_n_u8(1));
        uint8x16_t b7 = vandq_u8(vshrq_n_u8(vecCode, 6), vdupq_n_u8(1));

        uint8x16_t s1 = veorq_u8(veorq_u8(b1, b3), veorq_u8(b5, b7));
        uint8x16_t s2 = veorq_u8(veorq_u8(b2, b3), veorq_u8(b6, b7));
        uint8x16_t s4 = veorq_u8(veorq_u8(b4, b5), veorq_u8(b6, b7));

        uint8x16_t syndrome = vorrq_u8(vorrq_u8(s1, vshlq_n_u8(s2, 1)), vshlq_n_u8(s4, 2));

        // Vectorized bitmask shift for auto-correction
        uint8x16_t mask1 = vshlq_u8(vdupq_n_u8(1), vsubq_u8(syndrome, vdupq_n_u8(1)));
        uint8x16_t isNonZero = vtstq_u8(syndrome, syndrome);
        uint8x16_t finalMask = vandq_u8(mask1, isNonZero);

        // Corrected Vector
        uint8x16_t corrected = veorq_u8(vecCode, finalMask);

        // Reconstruct nibbles
        uint8x16_t d0 = vandq_u8(vshrq_n_u8(corrected, 2), vdupq_n_u8(1));
        uint8x16_t d1 = vandq_u8(vshrq_n_u8(corrected, 4), vdupq_n_u8(1));
        uint8x16_t d2 = vandq_u8(vshrq_n_u8(corrected, 5), vdupq_n_u8(1));
        uint8x16_t d3 = vandq_u8(vshrq_n_u8(corrected, 6), vdupq_n_u8(1));

        return vorrq_u8(vorrq_u8(d0, vshlq_n_u8(d1, 1)), vorrq_u8(vshlq_n_u8(d2, 2), vshlq_n_u8(d3, 3)));
    }
#endif

    /**
     * @brief Encodes a byte array into ECC codeword stream
     */
    static std::vector<uint8_t> encodeBuffer(const uint8_t* data, size_t length) {
        std::vector<uint8_t> codewords;
        codewords.reserve(length * 2);
        for (size_t i = 0; i < length; ++i) {
            auto pair = encodeByte(data[i]);
            codewords.push_back(pair.first);
            codewords.push_back(pair.second);
        }
        return codewords;
    }

    /**
     * @brief Decodes an ECC codeword stream into original byte vector (with SIMD acceleration if available)
     */
    static std::vector<uint8_t> decodeBuffer(const std::vector<uint8_t>& codewords, int& totalCorrections) {
        totalCorrections = 0;
        size_t byteCount = codewords.size() / 2;
        std::vector<uint8_t> result;
        result.reserve(byteCount);

        for (size_t i = 0; i < byteCount; ++i) {
            uint8_t lowCode = codewords[i * 2];
            uint8_t highCode = codewords[i * 2 + 1];
            result.push_back(decodeByte(lowCode, highCode, totalCorrections));
        }

        return result;
    }

    /**
     * @brief Vectorized SIMD Buffer Decoder (Fastest mode for large buffers)
     */
    static std::vector<uint8_t> decodeBufferSIMD(const std::vector<uint8_t>& codewords) {
        size_t totalCodewords = codewords.size();
        std::vector<uint8_t> resultNibbles(totalCodewords);

        size_t i = 0;
#if FAST_ECC_HAS_NEON
        for (; i + 16 <= totalCodewords; i += 16) {
            uint8x16_t vecCode = vld1q_u8(&codewords[i]);
            uint8x16_t vecDecoded = decodeSIMD16(vecCode);
            vst1q_u8(&resultNibbles[i], vecDecoded);
        }
#endif
        // Scalar fallback for trailing elements
        for (; i < totalCodewords; ++i) {
            bool dummy = false;
            resultNibbles[i] = decode4(codewords[i], dummy);
        }

        // Pack nibbles into bytes
        size_t byteCount = totalCodewords / 2;
        std::vector<uint8_t> bytes(byteCount);
        for (size_t j = 0; j < byteCount; ++j) {
            bytes[j] = (resultNibbles[j * 2 + 1] << 4) | resultNibbles[j * 2];
        }

        return bytes;
    }

    static std::vector<uint8_t> encodeString(const std::string& text) {
        return encodeBuffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    static std::string decodeString(const std::vector<uint8_t>& codewords, int& totalCorrections) {
        auto vec = decodeBuffer(codewords, totalCorrections);
        return std::string(vec.begin(), vec.end());
    }
};

} // namespace ecc

#endif // FAST_ECC_BRANCHLESS_HPP
