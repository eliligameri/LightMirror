#pragma once

#include "AudioTypes.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace lm {

class StereoRingBuffer {
public:
    explicit StereoRingBuffer(size_t capacityFrames = 262144)
        : data_(capacityFrames), capacity_(capacityFrames) {}

    void Clear() {
        head_ = tail_ = size_ = 0;
    }

    size_t Size() const { return size_; }
    size_t Capacity() const { return capacity_; }
    size_t Free() const { return capacity_ - size_; }

    bool Push(const StereoFrame& frame) {
        if (size_ == capacity_) return false;
        data_[head_] = frame;
        head_ = (head_ + 1) % capacity_;
        ++size_;
        return true;
    }

    StereoFrame Peek(size_t offset) const {
        if (offset >= size_) return {};
        return data_[(tail_ + offset) % capacity_];
    }

    void Pop(size_t count) {
        count = std::min(count, size_);
        tail_ = (tail_ + count) % capacity_;
        size_ -= count;
    }

    void DropOldest(size_t count) { Pop(count); }

private:
    std::vector<StereoFrame> data_;
    size_t capacity_ = 0;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
};

} // namespace lm
