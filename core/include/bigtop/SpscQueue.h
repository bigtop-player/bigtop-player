/// @file SpscQueue.h
/// @brief A fixed-size, lock-free single-producer/single-consumer queue.
///
/// The control thread uses it to hand commands to the audio thread without a
/// lock, so the audio thread can never be blocked by the UI.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace bigtop {

/// @brief Lock-free FIFO holding up to exactly @p Capacity items of @p T.
///
/// One thread may push and one (other) thread may pop, concurrently. Head and
/// tail are free-running counters (not wrapped indices), so "full" is
/// tail - head == Capacity and every slot is usable; no slot is sacrificed to
/// tell full from empty.
/// @tparam T Item type; must be trivially copyable so a push/pop is a plain
///         memory copy that can never allocate or throw.
/// @tparam Capacity Maximum number of items held at once; > 0.
template <class T, size_t Capacity>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue items must be trivially copyable");
    static_assert(Capacity > 0, "SpscQueue needs at least one slot");

public:
    /// @brief Appends an item.
    /// @param item [in] Item to copy into the queue.
    /// @return False (and nothing stored) when the queue is full.
    /// @note Producer thread only. Wait-free, no allocation.
    bool push(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire pairs with pop's release: the slot we reuse has been read.
        if (tail - head_.load(std::memory_order_acquire) == Capacity) return false;
        items_[tail % Capacity] = item;
        // Release publishes the item before the consumer can see the new tail.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// @brief Removes the oldest item.
    /// @param item [out] Receives the item; untouched when the queue is empty.
    /// @return False when the queue is empty.
    /// @note Consumer thread only. Wait-free, no allocation.
    bool pop(T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (tail_.load(std::memory_order_acquire) == head) return false;
        item = items_[head % Capacity];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> items_{};  ///< @brief Ring storage, indexed by counter % Capacity.
    std::atomic<size_t> head_{0};      ///< @brief Count of items ever popped (consumer writes).
    std::atomic<size_t> tail_{0};      ///< @brief Count of items ever pushed (producer writes).
};

}  // namespace bigtop
