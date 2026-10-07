// Moving frames off the capture thread.
//
// Capture used to grab, write to disk and draw the preview on one thread, so any
// hiccup in writing or drawing stalled the camera directly - and a stalled camera
// drops frames at the driver. Now the capture thread does the least possible work:
// copy the frame into a pooled buffer, hand the Spinnaker image straight back, and
// publish. Writing and drawing happen elsewhere.
//
// Two rules the whole design rests on:
//
//   1. No allocation on the capture path. Buffers are taken from a fixed pool and
//      returned when written, so the hot loop never calls into the allocator.
//
//   2. Only the writer may hold anything up. The preview gets whatever the latest
//      frame happens to be and is free to miss as many as it likes; it can never
//      cost a recorded frame. If the writer genuinely cannot keep up, frames are
//      dropped deliberately and counted, never silently.
//
// Locking is a plain mutex and condition variable throughout. At 30-170 fps there
// are 6-33 ms between frames and a lock costs tens of nanoseconds, so there is no
// case for anything cleverer - and this version can be read and debugged.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace behaviour_camera {

// One captured frame, owned by the pool it came from.
struct Frame
{
    std::vector<uint8_t> data;   // allocated once, at pool construction
    size_t size = 0;             // bytes actually used
    uint64_t frameID = 0;        // the camera's own counter
};

// Fixed set of preallocated buffers. acquire() returns nullptr when they are all
// in flight, which is the signal that the writer has fallen behind.
class FramePool
{
public:
    FramePool(size_t frameBytes, size_t count)
        : frameBytes_(frameBytes)
    {
        storage_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            auto frame = std::make_unique<Frame>();
            frame->data.resize(frameBytes);
            free_.push_back(frame.get());
            storage_.push_back(std::move(frame));
        }
    }

    Frame* acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty()) {
            return nullptr;
        }
        Frame* frame = free_.back();
        free_.pop_back();
        return frame;
    }

    void release(Frame* frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back(frame);
    }

    size_t capacity() const { return storage_.size(); }
    size_t frameBytes() const { return frameBytes_; }

    size_t inFlight() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return storage_.size() - free_.size();
    }

private:
    mutable std::mutex mutex_;
    size_t frameBytes_;
    std::vector<std::unique_ptr<Frame>> storage_;
    std::vector<Frame*> free_;
};

// Hands frames from the capture thread to the writer. Unbounded by design: the
// pool is what limits how many frames can be outstanding, so bounding this as
// well would just add a second place to drop them.
class FrameQueue
{
public:
    void push(Frame* frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(frame);
            if (queue_.size() > highWater_) {
                highWater_ = queue_.size();
            }
        }
        ready_.notify_one();
    }

    // Blocks until a frame is available, or until close() has been called and the
    // queue is empty - which returns nullptr and means "no more work".
    Frame* pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) {
            return nullptr;
        }
        Frame* frame = queue_.front();
        queue_.pop_front();
        return frame;
    }

    // No more frames will be pushed. Whatever is queued is still drained first, so
    // the last frames of a session are never thrown away.
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    size_t depth() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t highWater() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return highWater_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame*> queue_;
    bool closed_ = false;
    size_t highWater_ = 0;
};

// The most recent frame, for the preview to draw whenever it gets round to it.
//
// Deliberately a copy into its own buffer rather than a reference into the pool:
// it means the preview can hold a frame for as long as it likes without keeping a
// pooled buffer out of circulation, which would eat into the writer's slack. The
// copy only happens when the preview has asked for one, so at the display rate
// rather than the capture rate.
class PreviewSlot
{
public:
    explicit PreviewSlot(size_t frameBytes) : buffer_(frameBytes) {}

    // Called from the capture thread, and only when wanted() is true.
    void publish(const void* data, size_t size, uint64_t frameID)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size > buffer_.size()) {
            return;
        }
        std::memcpy(buffer_.data(), data, size);
        size_ = size;
        frameID_ = frameID;
        fresh_ = true;
        wanted_.store(false, std::memory_order_relaxed);
    }

    // Called from the display thread. False if nothing new has arrived.
    bool take(std::vector<uint8_t>& into, uint64_t& frameID)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fresh_) {
            return false;
        }
        into.assign(buffer_.begin(), buffer_.begin() + size_);
        frameID = frameID_;
        fresh_ = false;
        return true;
    }

    void request() { wanted_.store(true, std::memory_order_relaxed); }
    bool wanted() const { return wanted_.load(std::memory_order_relaxed); }

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> buffer_;
    size_t size_ = 0;
    uint64_t frameID_ = 0;
    bool fresh_ = false;
    std::atomic<bool> wanted_{ true };
};

// Everything the preview and the summary want to show. Atomic so they can be read
// at any time without coordinating with the threads updating them.
struct Counters
{
    std::atomic<int64_t> framesCaptured{ 0 };
    std::atomic<int64_t> framesWritten{ 0 };
    std::atomic<int64_t> droppedInTransit{ 0 };   // gaps in the camera's counter
    std::atomic<int64_t> droppedNoBuffer{ 0 };    // writer fell behind; our fault
    std::atomic<int64_t> incompleteFrames{ 0 };
    std::atomic<int64_t> cameraResets{ 0 };
    std::atomic<int64_t> bytesWritten{ 0 };
};

} // namespace behaviour_camera
