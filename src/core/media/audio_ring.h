#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace oa::media {

// One producer, one consumer. Storage is allocated before either starts.
// A flush only publishes a discard watermark: the producer must never move
// the reader cursor and reuse memory that a callback may still be copying.
template<class T> class AudioRing {
public:
    explicit AudioRing(size_t capacity) : data_(capacity) {}
    size_t available() const {
        const uint64_t r = std::max(read_.load(std::memory_order_acquire),
                                    discard_.load(std::memory_order_acquire));
        const uint64_t w = write_.load(std::memory_order_acquire);
        return w > r ? size_t(w - r) : 0;
    }
    size_t writable() const {
        const uint64_t r = read_.load(std::memory_order_acquire);
        const uint64_t w = write_.load(std::memory_order_relaxed);
        return data_.size() - size_t(w - r);
    }
    size_t push(const T* src, size_t count) {
        count = std::min(count, writable());
        const uint64_t w = write_.load(std::memory_order_relaxed);
        copy_in(w, src, count);
        write_.store(w + count, std::memory_order_release);
        return count;
    }
    size_t pop(T* dst, size_t count) {
        const uint64_t r = std::max(read_.load(std::memory_order_relaxed),
                                    discard_.load(std::memory_order_acquire));
        const uint64_t w = write_.load(std::memory_order_acquire);
        count = std::min(count, w > r ? size_t(w - r) : size_t(0));
        const size_t start = size_t(r % data_.size());
        const size_t first = std::min(count, data_.size() - start);
        if (first) std::memcpy(dst, data_.data() + start, first * sizeof(T));
        if (count > first) std::memcpy(dst + first, data_.data(), (count - first) * sizeof(T));
        read_.store(r + count, std::memory_order_release);
        return count;
    }
    void clear() { discard_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }
private:
    void copy_in(uint64_t w, const T* src, size_t count) {
        const size_t start = size_t(w % data_.size());
        const size_t first = std::min(count, data_.size() - start);
        if (first) std::memcpy(data_.data() + start, src, first * sizeof(T));
        if (count > first) std::memcpy(data_.data(), src + first, (count - first) * sizeof(T));
    }
    std::vector<T> data_;
    alignas(64) std::atomic<uint64_t> read_{0};
    alignas(64) std::atomic<uint64_t> write_{0};
    std::atomic<uint64_t> discard_{0};
};

} // namespace oa::media
