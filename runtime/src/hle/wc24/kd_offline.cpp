#include "hle/wc24/kd_offline.h"

#include "hle/runtime_parse_helpers.h"
#include "memory.h"

#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

namespace KdOffline {

enum class KdDevice { Request, Time };

constexpr int32_t kIosEInval = -4;
constexpr int32_t kIosEBadFd = -101;
constexpr uint32_t kMaxIoVectors = 32;

std::mutex g_mutex;
std::map<uint32_t, KdDevice> g_devices;
uint32_t g_nextFd = 2000;  // above the NAND, SHA and Bluetooth descriptor ranges

int64_t g_utcDiff = 0;  // NWC24 universal time minus host time, set by SET_UNIVERSAL_TIME
uint64_t g_rtcCounter = 0;

bool Lookup(uint32_t fd, KdDevice& device) {
    std::lock_guard<std::mutex> lock(KdOffline::g_mutex);
    const auto it = g_devices.find(fd);
    if (it == g_devices.end()) {
        return false;
    }
    device = it->second;
    return true;
}

// KD replies carry the NWC24 result word at out+0.
void WriteResult(uint32_t outBuf, uint32_t outLen, int32_t value) {
    if (outBuf && outLen >= 4 && Memory::Contains(outBuf, 4)) {
        Memory::Write32(outBuf, static_cast<uint32_t>(value));
    }
}

uint64_t UniversalTime() {
    return static_cast<uint64_t>(static_cast<int64_t>(std::time(nullptr)) + g_utcDiff);
}

// /dev/net/kd/request: scheduler and mail requests. Offline there is no scheduler to suspend and no mail to
// send, so every request succeeds with nothing to do (scheduler spans of 1 / 2 minutes for the time-trigger query,
// as Dolphin reports them).
int32_t RequestIoctl(uint32_t cmd, uint32_t outBuf, uint32_t outLen) {
    WriteResult(outBuf, outLen, 0);
    if (cmd == 0x04 && outBuf && outLen >= 0x0C && Memory::Contains(outBuf, 0x0C)) {
        Memory::Write32(outBuf + 4u, 1);  // mail span
        Memory::Write32(outBuf + 8u, 2);  // download span
    }
    return 0;
}

// /dev/net/kd/time: Dolphin's NetKDTimeDevice. u64 payloads follow the result word at out+4.
int32_t TimeIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    enum : uint32_t {
        kGetUniversalTime = 0x14,
        kSetUniversalTime = 0x15,
        kUnimplemented = 0x16,
        kSetRtcCounter = 0x17,
        kGetTimeDiff = 0x18,
    };
    int32_t ipcResult = 0;
    switch (cmd) {
    case kGetUniversalTime:
        if (outBuf && outLen >= 0x0C && Memory::Contains(outBuf, 0x0C)) {
            Memory::Write64(outBuf + 4u, UniversalTime());
        }
        break;
    case kSetUniversalTime:
        if (inBuf && inLen >= 8 && Memory::Contains(inBuf, 8)) {
            g_utcDiff = static_cast<int64_t>(Memory::Read64(inBuf)) - static_cast<int64_t>(std::time(nullptr));
        }
        break;
    case kSetRtcCounter:
        if (inBuf && inLen >= 4 && Memory::Contains(inBuf, 4)) {
            g_rtcCounter = Memory::Read32(inBuf);
        }
        break;
    case kGetTimeDiff:
        if (outBuf && outLen >= 0x0C && Memory::Contains(outBuf, 0x0C)) {
            Memory::Write64(outBuf + 4u, UniversalTime() - g_rtcCounter);
        }
        break;
    case kUnimplemented:
        ipcResult = -9;
        break;
    default:
        break;
    }
    WriteResult(outBuf, outLen, 0);
    return ipcResult;
}

int32_t Dispatch(KdDevice device, uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    return device == KdDevice::Request ? RequestIoctl(cmd, outBuf, outLen)
                                       : TimeIoctl(cmd, inBuf, inLen, outBuf, outLen);
}

}  // namespace KdOffline

using KdOffline::KdDevice;

extern "C" int32_t KD_HLE_OpenDevice(const char* path) {
    if (!path) {
        return 0;
    }
    KdDevice device;
    if (std::strcmp(path, "/dev/net/kd/request") == 0) {
        device = KdDevice::Request;
    } else if (std::strcmp(path, "/dev/net/kd/time") == 0) {
        device = KdDevice::Time;
    } else {
        return 0;
    }
    std::lock_guard<std::mutex> lock(KdOffline::g_mutex);
    const uint32_t fd = KdOffline::g_nextFd++;
    KdOffline::g_devices.emplace(fd, device);
    return static_cast<int32_t>(fd);
}

extern "C" bool KD_HLE_IsFd(uint32_t fd) {
    KdDevice device;
    return KdOffline::Lookup(fd, device);
}

extern "C" int32_t KD_HLE_Close(uint32_t fd) {
    std::lock_guard<std::mutex> lock(KdOffline::g_mutex);
    return KdOffline::g_devices.erase(fd) != 0 ? 0 : KdOffline::kIosEBadFd;
}

extern "C" int32_t KD_HLE_Ioctl(uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
                                uint32_t outBuf, uint32_t outLen) {
    KdDevice device;
    if (!KdOffline::Lookup(fd, device)) {
        return KdOffline::kIosEBadFd;
    }
    return KdOffline::Dispatch(device, cmd, inBuf, inLen, outBuf, outLen);
}

extern "C" int32_t KD_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                 uint32_t vectorPtr) {
    KdDevice device;
    if (!KdOffline::Lookup(fd, device)) {
        return KdOffline::kIosEBadFd;
    }
    // numIn/numOut are guest-controlled; sum in 64 bits so a wrap cannot shorten the check.
    const uint64_t count = static_cast<uint64_t>(numIn) + numOut;
    if (count > KdOffline::kMaxIoVectors ||
        (count != 0 && (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(count) * 8u)))) {
        return KdOffline::kIosEInval;
    }
    uint32_t inBuf = 0, inLen = 0, outBuf = 0, outLen = 0;
    if (numIn > 0) {
        const RuntimeHle::IoVector in = RuntimeHle::ReadIoVector(vectorPtr, 0);
        inBuf = in.address;
        inLen = in.size;
    }
    if (numOut > 0) {
        const RuntimeHle::IoVector out = RuntimeHle::ReadIoVector(vectorPtr, numIn);
        outBuf = out.address;
        outLen = out.size;
    }
    return KdOffline::Dispatch(device, cmd, inBuf, inLen, outBuf, outLen);
}
