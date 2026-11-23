#include <cstdlib>
#include <cstdio>
#include <string>
// NAND/ISFS HLE: the IOS_* device layer.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"
#include "host_file.h"

#include "runtime_log.h"


struct ISFSFileStats {
    uint32_t length;    // File size in bytes
    uint32_t position;  // Current file position
};

// ============================================================================
// Device identifiers and ioctl commands
// ============================================================================

// Special FD for /dev/fs (the ISFS device)
static constexpr int32_t ISFS_DEV_FD = 1;
static constexpr int32_t ES_DEV_FD = 3;
static constexpr uint32_t ES_IOCTL_GETDEVICEID = 0x07;
static constexpr uint32_t ES_IOCTL_GETDEVICECERT = 0x1E;
static constexpr uint32_t ES_IOCTL_GETTITLEID = 0x20;
static constexpr uint32_t ES_IOCTL_SIGN = 0x30;
static constexpr int32_t ES_EINVAL = -1017;  // IOS ES error: invalid argument

static std::string ReadGuestCString(uint32_t address, size_t maxLength = 1024) {
    std::string text;
    if (address == 0) {
        return text;
    }

    for (size_t i = 0; i < maxLength; ++i) {
        const uint32_t current = address + static_cast<uint32_t>(i);
        if (!Memory::Contains(current, 1)) {
            break;
        }
        const char ch = static_cast<char>(Memory::Read8(current));
        if (ch == '\0') {
            break;
        }
        text.push_back(ch);
    }
    return text;
}

// Same guest layout as the /dev/net ioctlv descriptors; see runtime_parse_helpers.h.
using IosVector = RuntimeHle::IoVector;
using RuntimeHle::ReadIoVector;

static IosVector ReadIosVector(uint32_t vectorPtr, uint32_t index) {
    return ReadIoVector(vectorPtr, index);
}

static uint64_t CurrentMkwTitleId() {
    uint32_t low = CurrentMkwTitleIdLo();
    return (static_cast<uint64_t>(kNandTitleIdHi) << 32) | low;
}

static bool IsValidGuestRange(uint32_t address, uint32_t size) {
    return size == 0 || (address != 0 && Memory::Contains(address, size));
}

// ---- OPUC: OPUC_TRACE_NAND=1 logs every IOS call into the NAND/ISFS HLE with its path, command and result ----
static int32_t NAND_IOS_Open_Impl(uint32_t pathPtr, uint32_t mode);
static int32_t NAND_IOS_Close_Impl(uint32_t fd);
static int32_t NAND_IOS_Read_Impl(uint32_t fd, uint32_t bufferPtr, uint32_t length);
static int32_t NAND_IOS_Write_Impl(uint32_t fd, uint32_t bufferPtr, uint32_t length);
static int32_t NAND_IOS_Seek_Impl(uint32_t fd, int32_t offset, int32_t whence);
static int32_t NAND_IOS_Ioctl_Impl(uint32_t fd, uint32_t cmd, uint32_t inBufPtr, uint32_t inLen, uint32_t outBufPtr, uint32_t outLen);
static int32_t NAND_IOS_Ioctlv_Impl(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr);
namespace {
bool OpucNandTrace() {
    static const bool on = [] { const char* v = std::getenv("OPUC_TRACE_NAND"); return v && v[0] == '1'; }();
    return on;
}
std::string OpucGuestPath(uint32_t addr) {
    std::string out;
    if (addr == 0) return out;
    try {
        for (uint32_t i = 0; i < 96; ++i) {
            const uint8_t c = Memory::Read8(addr + i);
            if (c == 0) break;
            out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
        }
    } catch (...) {}
    return out;
}
// ISFS ioctls carry a path at in+0 (GetAttr/Delete/Rename) or in+6 (ISFSAttr: owner u32, group u16, path).
std::string OpucIoctlPath(uint32_t in) {
    std::string p = OpucGuestPath(in);
    if (!p.empty() && p[0] == '/') return p;
    p = OpucGuestPath(in + 6);
    return (!p.empty() && p[0] == '/') ? p : std::string();
}
} // namespace
extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode) {
    const int32_t r = NAND_IOS_Open_Impl(pathPtr, mode);
    if (OpucNandTrace()) std::fprintf(stderr, "[opuc-nand] open '%s' mode=%u -> %d\n", OpucGuestPath(pathPtr).c_str(), mode, r);
    return r;
}
extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd) {
    const int32_t r = NAND_IOS_Close_Impl(fd);
    if (OpucNandTrace()) std::fprintf(stderr, "[opuc-nand] close fd=%u -> %d\n", fd, r);
    return r;
}
extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    const int32_t r = NAND_IOS_Read_Impl(fd, bufferPtr, length);
    if (OpucNandTrace()) std::fprintf(stderr, "[opuc-nand] read fd=%u len=0x%X -> %d\n", fd, length, r);
    return r;
}
extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    const int32_t r = NAND_IOS_Write_Impl(fd, bufferPtr, length);
    if (OpucNandTrace()) std::fprintf(stderr, "[opuc-nand] write fd=%u len=0x%X -> %d\n", fd, length, r);
    return r;
}
extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence) {
    const int32_t r = NAND_IOS_Seek_Impl(fd, offset, whence);
    if (OpucNandTrace()) std::fprintf(stderr, "[opuc-nand] seek fd=%u off=%d whence=%d -> %d\n", fd, offset, whence, r);
    return r;
}
extern "C" int32_t NAND_IOS_Ioctl_HLE(uint32_t fd, uint32_t cmd, uint32_t inBufPtr, uint32_t inLen, uint32_t outBufPtr, uint32_t outLen) {
    const int32_t r = NAND_IOS_Ioctl_Impl(fd, cmd, inBufPtr, inLen, outBufPtr, outLen);
    if (OpucNandTrace()) std::fprintf(stderr, "[opuc-nand] ioctl fd=%u cmd=0x%X in=0x%X/%u out=%u '%s' -> %d\n", fd, cmd, inBufPtr, inLen, outLen, OpucIoctlPath(inBufPtr).c_str(), r);
    return r;
}
extern "C" int32_t NAND_IOS_Ioctlv_HLE(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    const int32_t r = NAND_IOS_Ioctlv_Impl(fd, cmd, numIn, numOut, vectorPtr);
    if (OpucNandTrace()) {
        uint32_t v0 = 0;
        try { v0 = vectorPtr ? Memory::Read32(vectorPtr) : 0; } catch (...) {}
        std::fprintf(stderr, "[opuc-nand] ioctlv fd=%u cmd=0x%X in=%u out=%u '%s' -> %d\n", fd, cmd, numIn, numOut, OpucGuestPath(v0).c_str(), r);
    }
    return r;
}
// ---- end OPUC trace ----

static int32_t NAND_IOS_Open_Impl(uint32_t pathPtr, uint32_t mode) {
    const std::string pathStorage = ReadGuestCString(pathPtr);
    const char* path = pathPtr == 0 ? nullptr : pathStorage.c_str();
    
    if (!path) {
        LogNandError("IOS_Open", "null path");
        return ISFS_EINVAL;
    }
    
    // Handle special device paths
    if (std::strncmp(path, "/dev/", 5) == 0) {
        if (std::strcmp(path, "/dev/fs") == 0) {
            return ISFS_DEV_FD;
        }
        if (std::strcmp(path, "/dev/es") == 0) {
            return ES_DEV_FD;
        }
        if (const int32_t kdFd = KD_HLE_OpenDevice(path)) {
            return kdFd;
        }
        if (const int32_t btFd = Bluetooth_HLE_OpenDevice(path)) {
            return btFd;
        }
        LogNandWarning("IOS_Open", "unknown device '%s' mode=%u", path, mode);
        return ISFS_ENOENT;
    }
    
    // It's a NAND file path
    const std::filesystem::path hostPath = TranslateNandPath(path);
    if (hostPath.empty()) {
        LogNandError("IOS_Open", "FAILED to translate path '%s'", path);
        return ISFS_EINVAL;
    }

    if (const auto result = NandCheckSystemSaveRead("IOS_Open", hostPath, mode, true))
        return *result;
    
    // Determine file mode. IOS never creates files on open - creation happens
    // exclusively through ISFS CreateFile (which we implement). The previous
    // create-on-open fallback ("w+b") silently materialized 0-byte files (for
    // example /shared2/sys/net/02/config.dat) that later reads treated as
    // valid, poisoning persistent state across sessions.
    const char* fopenMode = "rb";
    if (mode == 2 || mode == 3) {
        if (!PathExists(hostPath)) {
            LogNandWarning("IOS_Open", "'%s' does not exist; open mode %u never creates it",
                    HostPathText(hostPath).c_str(), mode);
            return ISFS_ENOENT;
        }
        fopenMode = "r+b";      // Write-only opens still need read for seeks
    }

    FILE* file = NandFopen(hostPath, fopenMode);

    if (!file) {
        LogNandError("IOS_Open", "FAILED to open '%s'", HostPathText(hostPath).c_str());
        return ISFS_ENOENT;
    }
    
    int32_t fd = AllocateFd(hostPath, file, mode);
    return fd;
}
PPC_NATIVE_OVERRIDE(802B7628 /*mkw:801938F8*/, NAND_IOS_Open_HLE, int32_t, (uint32_t pathPtr, uint32_t mode), (pathPtr, mode));

extern "C" void NAND_IOS_OpenBody_HLE_801938FC(CpuContext* ctx) {
    const int32_t result = NAND_IOS_Open_HLE(ctx->gpr[3], ctx->gpr[4]);
    ctx->gpr[3] = static_cast<uint32_t>(result);
    ctx->gpr[1] = ctx->gpr[1] + 32u;
}
OPUC_UNBOUND(REGISTER_NATIVE_REPLACEMENT_AS, 0x801938FC, NAND_IOS_OpenBody_HLE_801938FC, "NAND_IOS_OpenBody_HLE_801938FC");

static int32_t NAND_IOS_Close_Impl(uint32_t fd) {
    if (fd == ISFS_DEV_FD) {
        return ISFS_OK;
    }
    if (fd == ES_DEV_FD) {
        return ISFS_OK;
    }
    if (KD_HLE_IsFd(fd)) {
        return KD_HLE_Close(fd);
    }
    if (Bluetooth_HLE_IsFd(fd)) {
        return Bluetooth_HLE_Close(fd);
    }
    
    auto* handle = GetHandle(fd);
    if (!handle) {
        LogNandError("IOS_Close", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    CloseFd(fd);
    return ISFS_OK;
}
PPC_NATIVE_OVERRIDE(802B7808 /*mkw:80193AD8*/, NAND_IOS_Close_HLE, int32_t, (uint32_t fd), (fd));

static int32_t NAND_IOS_Read_Impl(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Read", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    // The host pointer is only valid for the range it was resolved with.
    if (!Memory::Contains(bufferPtr, length)) {
        LogNandError("IOS_Read", "invalid buffer 0x%08X length=%u", bufferPtr, length);
        return ISFS_EINVAL;
    }
    uint8_t* buffer = Memory::GetPointer(bufferPtr, length);
    
    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    handle->position += static_cast<uint32_t>(bytesRead);
    
    return static_cast<int32_t>(bytesRead);
}
PPC_NATIVE_OVERRIDE(802B79B0 /*mkw:80193C80*/, NAND_IOS_Read_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

static int32_t NAND_IOS_Write_Impl(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Write", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    if (!Memory::Contains(bufferPtr, length)) {
        LogNandError("IOS_Write", "invalid buffer 0x%08X length=%u", bufferPtr, length);
        return ISFS_EINVAL;
    }
    const uint8_t* buffer = Memory::GetPointer(bufferPtr, length);
    
    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    handle->position += static_cast<uint32_t>(bytesWritten);
    
    return static_cast<int32_t>(bytesWritten);
}
PPC_NATIVE_OVERRIDE(802B7BB8 /*mkw:80193E88*/, NAND_IOS_Write_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

static int32_t NAND_IOS_Seek_Impl(uint32_t fd, int32_t offset, int32_t whence) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Seek", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        LogNandError("IOS_Seek", "fd=%d offset=%d whence=%d FAILED", fd, offset, whence);
        return ISFS_EIO;
    }
    
    handle->position = static_cast<uint32_t>(std::ftell(handle->file));
    return static_cast<int32_t>(handle->position);
}
PPC_NATIVE_OVERRIDE(802B7DA0 /*mkw:80194070*/, NAND_IOS_Seek_HLE, int32_t, (uint32_t fd, int32_t offset, int32_t whence), (fd, offset, whence));

// ============================================================================
// IOS_Ioctl HLE - Handles filesystem commands
// ============================================================================

// ISFS Ioctl commands
enum ISFSCommand {
    ISFS_IOCTL_FORMAT = 1,
    ISFS_IOCTL_GETSTATS = 2,
    ISFS_IOCTL_CREATEDIR = 3,
    ISFS_IOCTL_READDIR = 4,
    ISFS_IOCTL_SETATTR = 5,
    ISFS_IOCTL_GETATTR = 6,
    ISFS_IOCTL_DELETE = 7,
    ISFS_IOCTL_RENAME = 8,
    ISFS_IOCTL_CREATEFILE = 9,
    ISFS_IOCTL_SETFILEVERCTRL = 10,
    ISFS_IOCTL_GETFILESTATS = 11,
    ISFS_IOCTL_GETUSAGE = 12,
    ISFS_IOCTL_SHUTDOWN = 13,
};

static int32_t NAND_IOS_Ioctl_Impl(
    uint32_t fd,
    uint32_t cmd,
    uint32_t inBufPtr, uint32_t inLen,
    uint32_t outBufPtr, uint32_t outLen)
{

    if (KD_HLE_IsFd(fd)) {
        return KD_HLE_Ioctl(fd, cmd, inBufPtr, inLen, outBufPtr, outLen);
    }

    if (Bluetooth_HLE_IsFd(fd)) {
        LogNandWarning("IOS_Ioctl", "/dev/usb/oh1 does not support scalar ioctl cmd=%u", cmd);
        return -4;
    }
    
    // Handle /dev/fs ISFS commands
    if (fd == ISFS_DEV_FD) {
        switch (cmd) {
            case ISFS_IOCTL_CREATEDIR: {
                // Input buffer: path + attributes
                if (!inBufPtr || inLen < 0x4c) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr + 6);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                if (hostPath.empty()) {
                    return ISFS_EINVAL;
                }

                if (CreateDirectoryPath(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_DELETE: {
                if (!inBufPtr || inLen < 0x40) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                // fs::remove refuses a non-empty directory, matching rmdir.
                if (NandRemove(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_ENOENT;
            }
            
            case ISFS_IOCTL_GETATTR: {
                if (!inBufPtr || !outBufPtr) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                if (!PathExists(hostPath)) {
                    return ISFS_ENOENT;
                }
                
                // Return fake attributes (owner UID, group ID, permissions)
                // Format: u32 ownerID, u16 groupID, u8 ownerPerm, u8 groupPerm, u8 otherPerm, u8 attrs
                if (outLen >= 0x4c) {
                    if (!Memory::Contains(outBufPtr, outLen)) {
                        return ISFS_EINVAL;
                    }
                    std::memset(Memory::GetPointer(outBufPtr, outLen), 0, outLen);
                    // Owner UID = 0
                    Memory::Write32(outBufPtr, 0);
                    // Group ID = 0
                    Memory::Write16(outBufPtr + 4, 0);
                    // Permissions: 3 = read/write for all
                    Memory::Write8(outBufPtr + 0x49, 3); // owner perm
                    Memory::Write8(outBufPtr + 0x46, 3); // group perm
                    Memory::Write8(outBufPtr + 0x47, 3); // other perm
                    Memory::Write8(outBufPtr + 0x48, IsDirectory(hostPath) ? 2 : 1); // attrs (2=dir, 1=file)
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_CREATEFILE: {
                if (!inBufPtr || inLen < 0x4c || !Memory::Contains(inBufPtr, 0x4c)) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr + 6);
                if (!std::memchr(path, '\0', 64)) {
                    return ISFS_EINVAL;
                }
                const std::filesystem::path hostPath = TranslateNandPath(path);
                if (hostPath.empty()) {
                    return ISFS_EINVAL;
                }
                CreateParentDirectories(hostPath);

                const int result = RuntimeHostFile::CreateExclusive(hostPath);
                return result == 0 ? ISFS_OK : result == EEXIST ? ISFS_EEXIST : ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETFILESTATS: {
                // GETFILESTATS is addressed to a file fd, never to /dev/fs.
                LogNandWarning("IOS_Ioctl", "GETFILESTATS on ISFS device - unexpected");
                return ISFS_EINVAL;
            }
            
            case ISFS_IOCTL_RENAME: {
                if (!inBufPtr || inLen < 0x80) {
                    return ISFS_EINVAL;
                }
                const char* srcPath = (const char*)Memory::GetPointer(inBufPtr);
                const char* dstPath = (const char*)Memory::GetPointer(inBufPtr + 0x40);
                const std::filesystem::path srcHost = TranslateNandPath(srcPath);
                const std::filesystem::path dstHost = TranslateNandPath(dstPath);
                
                if (NandRename(srcHost, dstHost)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETSTATS: {
                // Return filesystem stats (fake values)
                if (outBufPtr && outLen >= 0x1c) {
                    Memory::Write32(outBufPtr + 0x00, 0x200000);  // Total blocks
                    Memory::Write32(outBufPtr + 0x04, 0x100000);  // Free blocks
                    Memory::Write32(outBufPtr + 0x08, 0);         // Used blocks
                    Memory::Write32(outBufPtr + 0x0C, 0);         // Bad blocks
                    Memory::Write32(outBufPtr + 0x10, 0);         // Reserved blocks
                    Memory::Write32(outBufPtr + 0x14, 0x20);      // Block size
                    Memory::Write32(outBufPtr + 0x18, 0);         // Free inodes
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_SETATTR: {
                // Ignore attribute changes - we don't implement file permissions
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_GETUSAGE: {
                // Return usage info (fake values)
                if (outBufPtr && outLen >= 8) {
                    Memory::Write32(outBufPtr + 0, 100);   // Files
                    Memory::Write32(outBufPtr + 4, 10000); // Blocks used
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_READDIR: {
                // Read directory listing
                // This is complex - return empty for now
                if (outBufPtr && outLen >= 4) {
                    Memory::Write32(outBufPtr, 0); // 0 entries
                }
                return ISFS_OK;
            }
            
            default:
                LogNandWarning("IOS_Ioctl", "unknown ISFS cmd=%u", cmd);
                return ISFS_OK;
        }
    }
    
    // Handle file-specific commands
    auto* handle = GetHandle(fd);
    if (handle && handle->file) {
        if (cmd == ISFS_IOCTL_GETFILESTATS) {
            // Get file stats
            if (!outBufPtr || outLen < 8) {
                return ISFS_EINVAL;
            }
            
            const NandFileExtent extent = NandProbeFileExtent(handle->file);
            Memory::Write32(outBufPtr, static_cast<uint32_t>(extent.size));
            Memory::Write32(outBufPtr + 4, static_cast<uint32_t>(extent.position));
            
            return ISFS_OK;
        }
    }
    
    // Unknown command - return success to not block game
    return ISFS_OK;
}

extern "C" void NAND_IOS_Ioctl_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t inBufPtr = ctx->gpr[5];
    const uint32_t inLen = ctx->gpr[6];
    const uint32_t outBufPtr = ctx->gpr[7];
    const uint32_t outLen = ctx->gpr[8];

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctl_HLE(fd, cmd, inBufPtr, inLen, outBufPtr, outLen));
}
PPC_NATIVE_OVERRIDE_VOID(802B7FC0 /*mkw:80194290*/, NAND_IOS_Ioctl_Entry_HLE, (CpuContext* ctx), (ctx));

// ============================================================================
// ISFS_OpenLib - Initialize ISFS
// ============================================================================

// Global state for ISFS initialization
static bool g_isfsInitialized = false;

// The ISFS/IPC globals ISFS_OpenLib touches, as negative r13 (SDA1) offsets.
// These are address-exact: they name the SDK's own variables, so the numbers are
// load-bearing and must not be "tidied". Names come from the RVL IPC/ISFS
// sources; only the naming changed here, never a value.
namespace {
// The ISFS client's small-data globals.
const uint32_t kIsfsFdGlobal = MKW_GADDR(80688CC8 /*mkw:80385920*/);            // __ISFS_fd
const uint32_t kIsfsPathGlobal = MKW_GADDR(80688CD0 /*mkw:80385928*/);          // __ISFS_path ("/dev/fs")
const uint32_t kIpcBufferLoGlobal = MKW_GADDR(8068AD94 /*mkw:803867EC*/);       // IPC buffer window, low
const uint32_t kIpcBufferHiGlobal = MKW_GADDR(8068AD98 /*mkw:803867F0*/);       // IPC buffer window, high
const uint32_t kIpcArenaLoGlobal = MKW_GADDR(8068AD84 /*mkw:8038677C*/);        // __IPCArenaLo
const uint32_t kIpcArenaHiGlobal = MKW_GADDR(8068AD88 /*mkw:80386780*/);        // __IPCArenaHi
const uint32_t kIsfsHeapHandleGlobal = MKW_GADDR(8068AD8C /*mkw:80386784*/);    // ISFS heap handle
const uint32_t kIsfsHeapBaseGlobal = MKW_GADDR(8068AD7C /*mkw:80386774*/);      // ISFS heap base address
const uint32_t kIsfsHeapInitializedGlobal = MKW_GADDR(8068AD78 /*mkw:80386770*/);  // ISFS heap created flag
} // namespace

static void WriteGuestString(uint32_t address, const char* value) {
    if (!value) {
        return;
    }
    const size_t length = std::strlen(value) + 1;
    if (!Memory::Contains(address, length)) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(value[i]));
    }
}

int32_t ISFS_OpenLib_Initialize(CpuContext* ctx) {
    g_isfsInitialized = true;
    
    // Create the title data directory if it doesn't exist
    char titleId[32];
    std::snprintf(titleId, sizeof(titleId), "%08x", kNandTitleIdHi);
    char gameId[32];
    std::snprintf(gameId, sizeof(gameId), "%08x", CurrentMkwTitleIdLo());
    CreateDirectoryPath(GetNandBasePath() / "title" / titleId / gameId / "data");

    if (!ctx) {
        return ISFS_OK;
    }

    WriteGuestString(kIsfsPathGlobal, "/dev/fs");

    if (Memory::Contains(kIsfsFdGlobal, 4)) {
        Memory::Write32(kIsfsFdGlobal, static_cast<uint32_t>(ISFS_DEV_FD));
    }

    // The heap bring-up below reads and writes all seven IPC globals, so it only
    // runs when every one of them is inside guest memory.
    for (const uint32_t global : {kIpcBufferLoGlobal, kIpcBufferHiGlobal, kIpcArenaLoGlobal,
                                  kIpcArenaHiGlobal, kIsfsHeapHandleGlobal, kIsfsHeapBaseGlobal,
                                  kIsfsHeapInitializedGlobal}) {
        if (!Memory::Contains(global, 4)) {
            return ISFS_OK;
        }
    }

    uint32_t ipcLo = Memory::Read32(kIpcBufferLoGlobal);
    uint32_t ipcHi = Memory::Read32(kIpcBufferHiGlobal);
    if (ipcLo == 0 || ipcHi == 0 || ipcLo >= ipcHi) {
        return ISFS_OK;
    }

    if (Memory::Read32(kIsfsHeapInitializedGlobal) == 0) {
        Memory::Write32(kIpcArenaLoGlobal, ipcLo);
        Memory::Write32(kIpcArenaHiGlobal, ipcHi);

        const uint32_t heapBase = (ipcLo + 31u) & ~31u;
        const uint32_t heapSize = 5440u;
        if (heapBase + heapSize <= ipcHi) {
            Memory::Write32(kIsfsHeapBaseGlobal, heapBase);

            const uint32_t savedR3 = ctx->gpr[3];
            const uint32_t savedR4 = ctx->gpr[4];
            const uint32_t savedR5 = ctx->gpr[5];
            const uint32_t savedLr = ctx->lr;

            ctx->gpr[3] = heapBase;
            ctx->gpr[4] = heapSize;
            ctx->lr = MKW_GADDR(802B5684 /*mkw:80169BCC*/);
            InvokeIndirectCpu(MKW_GADDR(802B86E8 /*mkw:801949B8*/), ctx);
            const uint32_t heapHandle = ctx->gpr[3];

            ctx->gpr[3] = heapBase + heapSize;
            ctx->lr = MKW_GADDR(802B5684 /*mkw:80169BCC*/);
            InvokeIndirectCpu(MKW_GADDR(802B6D70 /*mkw:80193040*/), ctx);

            ctx->gpr[3] = savedR3;
            ctx->gpr[4] = savedR4;
            ctx->gpr[5] = savedR5;
            ctx->lr = savedLr;

            Memory::Write32(kIsfsHeapHandleGlobal, heapHandle);
            Memory::Write32(kIsfsHeapInitializedGlobal, 1u);
        }
    }

    return ISFS_OK;
}

extern "C" void ISFS_OpenLib_HLE_80169BCC(CpuContext* ctx) {
    ctx->gpr[3] = static_cast<uint32_t>(ISFS_OpenLib_Initialize(ctx));
}

REGISTER_NATIVE_FUNCTION_AS(0x802B5684 /*mkw:80169BCC*/, ISFS_OpenLib_HLE_80169BCC, "ISFS_OpenLib_HLE_80169BCC");

// ============================================================================
// IOS_Ioctlv HLE - Vector Ioctl for complex ISFS operations
// ============================================================================

static int32_t HandleIsfsReadDir(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    const bool countOnly = (numIn == 1 && numOut == 1);
    if (!countOnly && !(numIn == 2 && numOut == 2)) {
        LogNandWarning("IOS_Ioctlv", "READDIR unsupported vector shape numIn=%u numOut=%u",
                numIn, numOut);
        return ISFS_EINVAL;
    }

    const IosVector pathVec = ReadIosVector(vectorPtr, 0);
    const std::string wiiPath = ReadGuestCString(pathVec.address, 64);
    if (wiiPath.empty()) {
        return ISFS_EINVAL;
    }
    const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());
    if (!IsDirectory(hostPath)) {
        return PathExists(hostPath) ? ISFS_EINVAL : ISFS_ENOENT;
    }

    // NAND names are at most 12 characters; longer host names cannot exist on
    // a real NAND (this also hides *.nandsafe.tmp write shadows).
    constexpr size_t kMaxNandNameLength = 12;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(hostPath, ec)) {
        std::string name = HostPathText(entry.path().filename());
        if (name.empty() || name.size() > kMaxNandNameLength) {
            continue;
        }
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());

    if (countOnly) {
        const IosVector countOut = ReadIosVector(vectorPtr, 1);
        if (countOut.size < 4 || !Memory::Contains(countOut.address, 4)) {
            return ISFS_EINVAL;
        }
        Memory::Write32(countOut.address, static_cast<uint32_t>(names.size()));
        return ISFS_OK;
    }

    const IosVector maxVec = ReadIosVector(vectorPtr, 1);
    const IosVector namesOut = ReadIosVector(vectorPtr, 2);
    const IosVector countOut = ReadIosVector(vectorPtr, 3);
    if (maxVec.size < 4 || !Memory::Contains(maxVec.address, 4) ||
        countOut.size < 4 || !Memory::Contains(countOut.address, 4) ||
        !IsValidGuestRange(namesOut.address, namesOut.size)) {
        return ISFS_EINVAL;
    }
    const uint32_t maxCount = Memory::Read32(maxVec.address);

    constexpr uint32_t kEntryWindow = 13; // 12 chars + terminator
    uint32_t cursor = 0;
    uint32_t written = 0;
    for (const std::string& name : names) {
        if (written >= maxCount || cursor + kEntryWindow > namesOut.size) {
            break;
        }
        uint8_t* out = Memory::GetPointer(namesOut.address + cursor, kEntryWindow);
        std::memset(out, 0, kEntryWindow);
        std::memcpy(out, name.data(), name.size());
        cursor += static_cast<uint32_t>(name.size()) + 1;
        ++written;
    }
    Memory::Write32(countOut.address, written);
    return ISFS_OK;
}

static int32_t NAND_IOS_Ioctlv_Impl(
    uint32_t fd,
    uint32_t cmd,
    uint32_t numIn,
    uint32_t numOut,
    uint32_t vectorPtr)
{

    if (KD_HLE_IsFd(fd)) {
        return KD_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr);
    }
    if (Bluetooth_HLE_IsFd(fd)) {
        return Bluetooth_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr);
    }



    if (fd == ISFS_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }
        if (cmd == ISFS_IOCTL_READDIR) {
            return HandleIsfsReadDir(numIn, numOut, vectorPtr);
        }
        return ISFS_OK;
    }

    if (fd == ES_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }

        switch (cmd) {
            // Device identity and signing need the console's ECC keys. OPUC never asks for them (its ES library is
            // ESP_InitLib / CloseLib / GetDataDir / GetTitleId, answered in esp.cpp; vault entities/boot-flow), so
            // they are not emulated: a request is reported and refused.
            case ES_IOCTL_GETDEVICEID:
            case ES_IOCTL_GETDEVICECERT:
            case ES_IOCTL_SIGN:
                LogNandWarning("IOS_Ioctlv", "/dev/es cmd=0x%X (device identity / signing) is not emulated", cmd);
                return ES_EINVAL;

            case ES_IOCTL_GETTITLEID: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size < 8 || out.address == 0 || !Memory::Contains(out.address, 8)) {
                    return ISFS_EINVAL;
                }
                const uint64_t titleId = CurrentMkwTitleId();
                Memory::Write32(out.address, static_cast<uint32_t>(titleId >> 32));
                Memory::Write32(out.address + 4u, static_cast<uint32_t>(titleId));
                return ISFS_OK;
            }

            default:
                LogNandWarning("IOS_Ioctlv", "unsupported /dev/es cmd=%u", cmd);
                return ISFS_EINVAL;
        }
    }
    
    // Non-device ioctlv has no ISFS command we need to service.
    return ISFS_OK;
}

extern "C" void NAND_IOS_Ioctlv_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t numIn = ctx->gpr[5];
    const uint32_t numOut = ctx->gpr[6];
    const uint32_t vectorPtr = ctx->gpr[7];

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctlv_HLE(fd, cmd, numIn, numOut, vectorPtr));
}
PPC_NATIVE_OVERRIDE_VOID(802B8310 /*mkw:801945E0*/, NAND_IOS_Ioctlv_Entry_HLE, (CpuContext* ctx), (ctx));
