// Publishing the newest frame to another process, for live pose estimation.
//
// A named shared memory block that holds the most recent frame and nothing else.
// Deliberately not a queue: the consumer always wants the freshest frame it can
// get, and a queue would let it fall behind and make decisions on stale data.
//
// The recording must not be able to suffer for this. So the writer never waits
// for a reader, never checks whether one exists, and cannot be blocked by one -
// the synchronisation is a seqlock over two slots, which has no lock at all. The
// worst a misbehaving reader can do is notice it read a torn frame and try again.
//
// Two slots rather than one because it costs 1.3 MB and removes the failure mode:
// the writer has to lap the reader twice before a copy can tear, which is two
// whole frame periods (20 ms at 100 fps) against a copy that takes about 0.3 ms.
// In practice a reader never retries.
//
// Layout of the block:
//
//     [ Header, 256 bytes ][ slot 0 pixels ][ slot 1 pixels ]
//
// The header is a fixed size and its first field is a magic number, so a reader
// built against a different version of this file fails loudly instead of
// misreading pixels. Everything in it is a fixed-width integer in native byte
// order; both processes are on the same machine, so there is no endianness
// question to answer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace behaviour_camera {

// Bumped whenever the header layout changes. Readers must check it.
constexpr uint32_t kFrameOutMagic = 0x42434D31;   // "BCM1"
constexpr uint32_t kFrameOutVersion = 1;
constexpr uint32_t kFrameOutSlots = 2;
constexpr size_t kFrameOutHeaderBytes = 256;

// Pixel formats. Only Mono8 is produced today; the field exists so a reader can
// refuse a format it does not understand rather than guess.
constexpr uint32_t kFrameOutMono8 = 1;

#pragma pack(push, 8)
struct FrameOutSlot
{
    uint64_t frame_id;        // the camera's own frame ID - the join key
    uint64_t capture_qpc;     // QueryPerformanceCounter when the frame arrived
    uint32_t size;            // bytes actually written
    uint32_t reserved;
};

struct FrameOutHeader
{
    uint32_t magic;
    uint32_t version;

    uint32_t width;
    uint32_t height;
    uint32_t stride;          // bytes per row
    uint32_t pixel_format;
    uint32_t frame_bytes;     // size of one slot's pixel area
    uint32_t slot_count;

    // Frames published since the recorder started. The seqlock: a reader takes
    // this, reads the slot it points at, and takes it again. See publish().
    uint64_t sequence;

    // For turning capture_qpc into something meaningful. start_unix_ns and
    // start_qpc are read at the same moment, so a frame's wall-clock time is
    // start_unix_ns + (capture_qpc - start_qpc) * 1e9 / qpc_frequency.
    uint64_t qpc_frequency;
    uint64_t start_qpc;
    uint64_t start_unix_ns;

    // Total frames the recorder has captured, published for free so a reader can
    // tell "the camera has stopped" from "nothing new since I last looked".
    uint64_t frames_captured;

    FrameOutSlot slots[kFrameOutSlots];
};
#pragma pack(pop)

static_assert(sizeof(FrameOutHeader) <= kFrameOutHeaderBytes,
              "FrameOutHeader must fit in the reserved header area");

// Writer side. Created by the recorder; does nothing if creation fails beyond
// reporting it, because losing live pose is not a reason to lose a recording.
class FrameOut
{
public:
    // Builds the canonical block name for a rig. Readers must agree on this.
    static std::string nameForRig(const std::string& rig);

    // Throws std::runtime_error if the block cannot be created.
    FrameOut(const std::string& name, uint32_t width, uint32_t height,
             uint32_t stride, size_t frameBytes, uint32_t pixelFormat);
    ~FrameOut();

    FrameOut(const FrameOut&) = delete;
    FrameOut& operator=(const FrameOut&) = delete;

    // Called from the capture thread, once per frame. Never blocks.
    void publish(const void* data, size_t size, uint64_t frameID, uint64_t framesCaptured);

    const std::string& name() const { return name_; }

private:
    std::string name_;
    void* mapping_ = nullptr;        // HANDLE, kept as void* to spare the header windows.h
    uint8_t* base_ = nullptr;
    FrameOutHeader* header_ = nullptr;
    uint8_t* slots_[kFrameOutSlots] = { nullptr, nullptr };
    size_t frameBytes_ = 0;
    uint64_t sequence_ = 0;
};

} // namespace behaviour_camera
