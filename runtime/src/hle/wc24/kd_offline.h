#pragma once

// Offline WiiConnect24 KD devices. One Piece Unlimited Cruise 1 has no online features; its RVL SDK still opens
// /dev/net/kd/request (NWC24 scheduler suspend, ioctl 0x01) and /dev/net/kd/time (RTC counter, ioctl 0x17) during
// boot. These answer those local requests with success and never touch the host network. Every other /dev/net
// path is left unopened, so IOS_Open falls through to the NAND layer and fails like a missing device.

#include <cstdint>

extern "C" int32_t KD_HLE_OpenDevice(const char* path);
extern "C" bool KD_HLE_IsFd(uint32_t fd);
extern "C" int32_t KD_HLE_Close(uint32_t fd);
extern "C" int32_t KD_HLE_Ioctl(uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
                                uint32_t outBuf, uint32_t outLen);
extern "C" int32_t KD_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                 uint32_t vectorPtr);
