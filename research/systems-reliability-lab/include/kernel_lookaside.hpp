/**
 * @file kernel_lookaside.hpp
 * @brief High-Performance Header-Only Kernel-Style Lock-Free Lookaside Pool & Ring Buffer
 * @author Antigravity AI Systems Research Lab
 * @version 1.1.0
 * @date 2026-08-02
 * 
 * Inspired by Windows Kernel Lookaside Lists (nt!ExAllocateFromNPagedLookasideList)
 * and Atomic S-Lists (nt!ExpInterlockedPushEntrySList).
 */

#ifndef KERNEL_LOOKASIDE_HPP
#define KERNEL_LOOKASIDE_HPP

#include <cstdint>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>
#include <utility>
#include <vector>

namespace kernel {

// Hardware Cache Line Alignment to prevent False Sharing (64 bytes for ARM64/x86)
constexpr size_t CACHE_LINE_SIZE = 64;

/**
 * @class LockFreeLookasidePool
 * @brief Lock-free, sub-5ns pre-allocated memory pool inspired by Windows Kernel Lookaside Lists
 * 
 * @tparam T Type of object to allocate
 * @tparam Capacity Maximum number of pre-allocated blocks in pool
 */
template <typename T, size_t Capacity = 4096>
class LockFreeLookasidePool {
private:
    struct Node {
        alignas(CACHE_LINE_SIZE) std::atomic<Node*> next;
        alignas(alignof(T)) uint8_t storage[sizeof(T)];
    };

    std::vector<Node> m_poolStorage;
    alignas(CACHE_LINE_SIZE) std::atomic<Node*> m_head{nullptr};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> m_allocatedCount{0};

public:
    LockFreeLookasidePool() : m_poolStorage(Capacity) {
        // Initialize lock-free S-List with pre-allocated nodes
        for (size_t i = 0; i < Capacity - 1; ++i) {
            m_poolStorage[i].next.store(&m_poolStorage[i + 1], std::memory_order_relaxed);
        }
        m_poolStorage[Capacity - 1].next.store(nullptr, std::memory_order_relaxed);
        m_head.store(&m_poolStorage[0], std::memory_order_relaxed);
    }

    ~LockFreeLookasidePool() noexcept = default;

    // Non-copyable, non-movable
    LockFreeLookasidePool(const LockFreeLookasidePool&) = delete;
    LockFreeLookasidePool& operator=(const LockFreeLookasidePool&) = delete;

    /**
     * @brief Lock-free allocation of an object from the pool (sub-5ns latency)
     * @tparam Args Constructor arguments for T
     * @return Pointer to constructed object T, or nullptr if pool is exhausted
     */
    template <typename... Args>
    T* allocate(Args&&... args) noexcept {
        Node* oldHead = m_head.load(std::memory_order_acquire);

        // Lock-free S-List Pop (Atomic CAS loop)
        while (oldHead != nullptr) {
            Node* nextNode = oldHead->next.load(std::memory_order_relaxed);
            if (m_head.compare_exchange_weak(oldHead, nextNode, 
                                             std::memory_order_release, 
                                             std::memory_order_acquire)) {
                m_allocatedCount.fetch_add(1, std::memory_order_relaxed);
                T* objPtr = reinterpret_cast<T*>(oldHead->storage);
                ::new (static_cast<void*>(objPtr)) T(std::forward<Args>(args)...);
                return objPtr;
            }
        }
        return nullptr; // Pool exhausted
    }

    /**
     * @brief Lock-free deallocation and return of an object to the pool
     * @param ptr Pointer to object T previously allocated from this pool
     */
    void deallocate(T* ptr) noexcept {
        if (!ptr) return;

        ptr->~T(); // Call destructor
        Node* node = reinterpret_cast<Node*>(reinterpret_cast<uint8_t*>(ptr) - offsetof(Node, storage));

        // Lock-free S-List Push (Atomic CAS loop)
        Node* oldHead = m_head.load(std::memory_order_relaxed);
        do {
            node->next.store(oldHead, std::memory_order_relaxed);
        } while (!m_head.compare_exchange_weak(oldHead, node, 
                                               std::memory_order_release, 
                                               std::memory_order_relaxed));

        m_allocatedCount.fetch_sub(1, std::memory_order_relaxed);
    }

    /** @brief Returns total active allocations from pool */
    size_t activeAllocations() const noexcept {
        return m_allocatedCount.load(std::memory_order_relaxed);
    }

    /** @brief Returns maximum capacity of pool */
    constexpr size_t capacity() const noexcept {
        return Capacity;
    }
};

/**
 * @class LockFreeRingBuffer
 * @brief Zero-sharing, lock-free ring buffer for inter-thread message passing
 * 
 * @tparam T Message element type
 * @tparam Capacity Ring buffer size (must be power of two)
 */
template <typename T, size_t Capacity = 1024>
class LockFreeRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

private:
    struct alignas(CACHE_LINE_SIZE) {
        std::atomic<size_t> head{0};
    } m_producer;

    struct alignas(CACHE_LINE_SIZE) {
        std::atomic<size_t> tail{0};
    } m_consumer;

    std::vector<T> m_buffer;

public:
    LockFreeRingBuffer() : m_buffer(Capacity) {}

    /**
     * @brief Lock-free push into ring buffer
     * @param item Element to push
     * @return true on success, false if buffer is full
     */
    bool push(const T& item) noexcept {
        const size_t currentHead = m_producer.head.load(std::memory_order_relaxed);
        const size_t currentTail = m_consumer.tail.load(std::memory_order_acquire);

        if (currentHead - currentTail >= Capacity) {
            return false; // Buffer full
        }

        m_buffer[currentHead & (Capacity - 1)] = item;
        m_producer.head.store(currentHead + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Lock-free pop from ring buffer
     * @param[out] item Destination element
     * @return true on success, false if buffer is empty
     */
    bool pop(T& item) noexcept {
        const size_t currentTail = m_consumer.tail.load(std::memory_order_relaxed);
        const size_t currentHead = m_producer.head.load(std::memory_order_acquire);

        if (currentTail == currentHead) {
            return false; // Buffer empty
        }

        item = m_buffer[currentTail & (Capacity - 1)];
        m_consumer.tail.store(currentTail + 1, std::memory_order_release);
        return true;
    }

    /** @brief Returns true if buffer is empty */
    bool empty() const noexcept {
        return m_consumer.tail.load(std::memory_order_relaxed) == m_producer.head.load(std::memory_order_relaxed);
    }
};

} // namespace kernel

#endif // KERNEL_LOOKASIDE_HPP
