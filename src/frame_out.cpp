#include "frame_out.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace behaviour_camera {
namespace {

// The header's sequence field, viewed as an atomic. Shared memory is not
// std::atomic-friendly across processes in the abstract, but on Windows x64 an
// aligned 64-bit load or store is atomic and both processes run the same ABI, so
// this is sound here. The acquire/release pairing is what actually matters: it
// stops the compiler or the CPU moving the pixel copy past the publish.
std::atomic<uint64_t>* asAtomic(uint64_t* p)
{
    static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t),
                  "atomic<uint64_t> must be lock-free and the same size");
    return reinterpret_cast<std::atomic<uint64_t>*>(p);
}

uint64_t nowUnixNanoseconds()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

uint64_t queryPerformanceCounter()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

uint64_t queryPerformanceFrequency()
{
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
}

std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), needed);
    return wide;
}

} // namespace

std::string FrameOut::nameForRig(const std::string& rig)
{
    // Session-local rather than Global: the recorder and the pose server are
    // launched by the same user in the same session, and Global would need the
    // SeCreateGlobalPrivilege that an ordinary account does not have.
    return "Local\\behaviour_camera_frame_" + rig;
}

FrameOut::FrameOut(const std::string& name, uint32_t width, uint32_t height,
                   uint32_t stride, size_t frameBytes, uint32_t pixelFormat)
    : name_(name), frameBytes_(frameBytes)
{
    const size_t total = kFrameOutHeaderBytes + frameBytes * kFrameOutSlots;

    const std::wstring wideName = widen(name_);
    HANDLE handle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,       // backed by the paging file, not a real file
        nullptr,
        PAGE_READWRITE,
        static_cast<DWORD>(total >> 32),
        static_cast<DWORD>(total & 0xFFFFFFFFu),
        wideName.c_str());

    if (handle == nullptr) {
        throw std::runtime_error("could not create shared memory block \"" + name_ +
                                 "\": CreateFileMapping failed with " +
                                 std::to_string(GetLastError()));
    }

    void* view = MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, total);
    if (view == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        throw std::runtime_error("could not map shared memory block \"" + name_ +
                                 "\": MapViewOfFile failed with " +
                                 std::to_string(error));
    }

    mapping_ = handle;
    base_ = static_cast<uint8_t*>(view);
    std::memset(base_, 0, total);

    header_ = reinterpret_cast<FrameOutHeader*>(base_);
    for (uint32_t i = 0; i < kFrameOutSlots; ++i) {
        slots_[i] = base_ + kFrameOutHeaderBytes + static_cast<size_t>(i) * frameBytes;
    }

    header_->width = width;
    header_->height = height;
    header_->stride = stride;
    header_->pixel_format = pixelFormat;
    header_->frame_bytes = static_cast<uint32_t>(frameBytes);
    header_->slot_count = kFrameOutSlots;
    header_->qpc_frequency = queryPerformanceFrequency();
    header_->start_qpc = queryPerformanceCounter();
    header_->start_unix_ns = nowUnixNanoseconds();
    header_->version = kFrameOutVersion;

    // Magic last, with a release fence in front of it, so a reader that attaches
    // mid-construction either sees a zero magic and waits or sees a header that is
    // complete behind it. Never a half-filled one.
    asAtomic(&header_->sequence)->store(0, std::memory_order_release);
    header_->magic = kFrameOutMagic;
}

FrameOut::~FrameOut()
{
    if (header_ != nullptr) {
        // Clear the magic so a reader still attached knows the block is dead
        // rather than sitting on the last frame forever.
        header_->magic = 0;
        asAtomic(&header_->sequence)->store(header_->sequence, std::memory_order_release);
    }
    if (base_ != nullptr) {
        UnmapViewOfFile(base_);
        base_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
}

void FrameOut::publish(const void* data, size_t size, uint64_t frameID,
                       uint64_t framesCaptured)
{
    if (header_ == nullptr || size > frameBytes_) {
        return;
    }

    // sequence_ counts frames published. The slot to write is the one the next
    // sequence number points at, which is never the slot a reader is currently
    // reading - that one is (sequence_ - 1) % 2.
    const uint32_t index = static_cast<uint32_t>(sequence_ % kFrameOutSlots);

    std::memcpy(slots_[index], data, size);

    FrameOutSlot& slot = header_->slots[index];
    slot.frame_id = frameID;
    slot.capture_qpc = queryPerformanceCounter();
    slot.size = static_cast<uint32_t>(size);

    header_->frames_captured = framesCaptured;

    // The release store publishes everything written above it. A reader using an
    // acquire load either sees this frame whole or does not see it at all.
    ++sequence_;
    asAtomic(&header_->sequence)->store(sequence_, std::memory_order_release);
}

} // namespace behaviour_camera
