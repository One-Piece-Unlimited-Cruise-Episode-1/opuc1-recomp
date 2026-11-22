// NAND/ISFS HLE: redirects Wii NAND paths (e.g. /title/00010004/524d4350/data/rksys.dat) to
// <nand_root>\title\00010004\524d4350\data\rksys.dat on the host.

#include "nand_internal.h"

#include "isa/big_endian.h"
#include "hle/storage/riivolution.h"
#include "nand_guest_path.h"
#include "host_file.h"
#include "runtime_log.h"

// ============================================================================
// Configuration
// ============================================================================

// Base path for the host Wii NAND directory (resolved at runtime).
static std::filesystem::path g_dolphinWiiBase;
static std::once_flag g_dolphinWiiBaseOnce;

// ============================================================================
// Logging
// ============================================================================

static void EmitNandLog(const char* func, const char* fmt, va_list args) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    RT_LOGF(RT_TAG_NAND, "%s: %s\n", func, buf);
}

void LogNandError(const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EmitNandLog(func, fmt, args);
    va_end(args);
}

void LogNandWarning(const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EmitNandLog(func, fmt, args);
    va_end(args);
}

// ============================================================================
// File Descriptor Management
// ============================================================================

std::map<int32_t, FileHandle> g_fileHandles;
static int32_t g_nextFd = 100; // Start at 100 to avoid confusion with stdio fds
std::mutex g_fdMutex;

int32_t AllocateFd(const std::filesystem::path& path, FILE* file, int32_t mode) {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    int32_t fd = g_nextFd++;
    g_fileHandles[fd] = {file, path, mode, 0};
    return fd;
}

FileHandle* GetHandle(int32_t fd) {
    auto it = g_fileHandles.find(fd);
    if (it == g_fileHandles.end()) {
        return nullptr;
    }
    return &it->second;
}

void CloseFd(int32_t fd) {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    auto it = g_fileHandles.find(fd);
    if (it != g_fileHandles.end()) {
        if (it->second.file) {
            std::fclose(it->second.file);
        }
        g_fileHandles.erase(it);
    }
}

// ============================================================================
// Path Translation
// ============================================================================

uint32_t CurrentMkwTitleIdLo() {
    return RuntimeHle::CurrentGameCode(NandTitleIdLo());
}

std::string CurrentNandDataDir() {
    char path[64];
    std::snprintf(path, sizeof(path), "/title/%08x/%08x/data",
                  kNandTitleIdHi, CurrentMkwTitleIdLo());
    return path;
}

// OPUC: the SDK's working directory NAND_currentDir (0x805B7760, nandGenerateAbsPath 802bb4a4 / nandChangeDir
// 802bb814) lives in guest memory and is changed by OPUC's translated NANDChangeDirAsync (SoundSe::NandWriteTmpFile
// switches to /tmp, SaveNandWorker::EnsureDataDir back to the home dir). Native NAND calls must resolve relative
// paths against it too, or the save lands in /tmp.
constexpr uint32_t kOpucNandCurrentDirAddr = 0x805B7760u;
std::string CurrentNandWorkingDir() {
    std::string dir;
    try {
        for (uint32_t i = 0; i < 64; ++i) {
            const uint8_t c = Memory::Read8(kOpucNandCurrentDirAddr + i);
            if (c == 0) break;
            dir.push_back(static_cast<char>(c));
        }
    } catch (...) {
        dir.clear();
    }
    if (dir.empty() || dir[0] != '/') return CurrentNandDataDir();
    return dir;
}

const std::filesystem::path& GetNandBasePath() {
    std::call_once(g_dolphinWiiBaseOnce, []() {
        g_dolphinWiiBase = RuntimeNandPath::DiscoverNandRootPath();
    });

    return g_dolphinWiiBase;
}

std::string HostPathText(const std::filesystem::path& path) {
    return RuntimeConfigFile::PathToUtf8(path);
}

FILE* NandFopen(const std::filesystem::path& path, const char* mode) {
    return RuntimeHostFile::Open(path, mode);
}

bool NandRemove(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::remove(path, ec) && !ec;
}

bool NandRename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
}

// Guest paths are absolute and already lexically resolved against the NAND root, so
// they are appended as relative components instead of replacing the root. Empty when a
// component could leave the root (see nand_guest_path.h).
static std::filesystem::path BuildHostNandPath(const std::string& wiiPathStr) {
    auto hostPath = RuntimeNandGuestPath::AppendBelowRoot(GetNandBasePath(), wiiPathStr);
    return hostPath ? std::move(*hostPath) : std::filesystem::path{};
}

// Collapses "." and ".." components against the NAND root. Without this, guest paths like
// "/title/../../../../Users/..." escape the NAND root into the real filesystem under the
// current user's permissions. ".." at the root clamps to the root, matching IOS.
static std::string LexicallyResolveWiiPath(const std::string& path) {
    std::vector<std::string> components;
    std::string component;
    // path is always absolute here, so a leading empty component is implied.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            component.push_back(path[i]);
            continue;
        }
        if (component == "..") {
            if (!components.empty()) {
                components.pop_back();
            }
        } else if (!component.empty() && component != ".") {
            components.push_back(component);
        }
        component.clear();
    }

    std::string resolved;
    for (const std::string& part : components) {
        resolved.push_back('/');
        resolved += part;
    }
    return resolved.empty() ? "/" : resolved;
}

static std::string NormalizeAbsoluteWiiPath(const char* wiiPath) {
    if (!wiiPath || wiiPath[0] == '\0') {
        return {};
    }

    std::string path = wiiPath;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path[0] != '/') {
        std::string cwd = CurrentNandWorkingDir();
        if (!cwd.empty() && cwd.back() != '/') {
            cwd.push_back('/');
        }
        path = cwd + path;
    }
    path = LexicallyResolveWiiPath(path);
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

struct RiivolutionSaveRedirect {
    bool enabled = false;
    bool clone = false;
    std::filesystem::path hostDirectory;
};

static std::once_flag g_riivolutionSaveRedirectOnce;
static RiivolutionSaveRedirect g_riivolutionSaveRedirect;

static const RiivolutionSaveRedirect& GetRiivolutionSaveRedirect() {
    // The active pack's <savegame> patch, resolved against the virtual SD root
    // exactly like Dolphin resolves it for Riivolution launches. This keeps
    // the recomp's save in the same folder a Dolphin/WheelWizard launch of the
    // pack uses (e.g. .../riivolution/save/<pack>/<game id>).
    std::call_once(g_riivolutionSaveRedirectOnce, []() {
        const auto& redirect = RuntimeRiivolution::GetSaveRedirect();
        if (!redirect) {
            return;
        }
        g_riivolutionSaveRedirect.enabled = true;
        g_riivolutionSaveRedirect.clone = redirect->clone;
        g_riivolutionSaveRedirect.hostDirectory = redirect->hostDirectory;
        // Riivolution creates the redirect folder if it does not exist.
        CreateDirectoryPath(g_riivolutionSaveRedirect.hostDirectory);
    });

    return g_riivolutionSaveRedirect;
}

static void CloneRiivolutionSaveIfNeeded(const std::filesystem::path& sourceHostPath,
                                         const std::filesystem::path& redirectedHostPath,
                                         const RiivolutionSaveRedirect& redirect) {
    if (!redirect.clone || PathExists(redirectedHostPath) || !PathExists(sourceHostPath)) {
        return;
    }

    CreateParentDirectories(redirectedHostPath);

    std::error_code ec;
    std::filesystem::copy_file(sourceHostPath, redirectedHostPath,
                               std::filesystem::copy_options::skip_existing, ec);
    if (ec) {
        LogNandWarning("RiivolutionSave", "WARNING: failed to clone '%s' -> '%s': %s",
                       HostPathText(sourceHostPath).c_str(),
                       HostPathText(redirectedHostPath).c_str(), ec.message().c_str());
    }
}

static bool ResolveRiivolutionSaveHostPath(const std::string& absoluteWiiPath,
                                           std::filesystem::path& outHostPath) {
    const RiivolutionSaveRedirect& redirect = GetRiivolutionSaveRedirect();
    if (!redirect.enabled) {
        return false;
    }

    const std::string dataDir = CurrentNandDataDir();
    if (absoluteWiiPath != dataDir &&
        (absoluteWiiPath.size() <= dataDir.size() ||
         absoluteWiiPath.compare(0, dataDir.size(), dataDir) != 0 ||
         absoluteWiiPath[dataDir.size()] != '/')) {
        return false;
    }

    std::string relative;
    if (absoluteWiiPath.size() > dataDir.size()) {
        relative = absoluteWiiPath.substr(dataDir.size() + 1);
    }

    // The path belongs to the redirect either way; an unsafe one resolves to nothing
    // rather than falling back to the NAND root.
    auto redirected = RuntimeNandGuestPath::AppendBelowRoot(redirect.hostDirectory, relative);
    if (!redirected) {
        outHostPath.clear();
        return true;
    }

    outHostPath = std::move(*redirected);
    CloneRiivolutionSaveIfNeeded(BuildHostNandPath(absoluteWiiPath), outHostPath, redirect);
    return true;
}

std::filesystem::path TranslateNandPath(const char* wiiPath) {
    std::string wiiPathStr = NormalizeAbsoluteWiiPath(wiiPath);
    if (wiiPathStr.empty()) {
        return {};
    }

    std::filesystem::path redirectedHostPath;
    if (ResolveRiivolutionSaveHostPath(wiiPathStr, redirectedHostPath)) {
        return redirectedHostPath;
    }

    return BuildHostNandPath(wiiPathStr);
}

std::optional<int32_t> NandCheckSystemSaveRead(const char* who,
    const std::filesystem::path& hostPath, int mode, bool ios) {
    const auto action = RuntimeNandSave::CheckRead(hostPath, mode);
    if (action == RuntimeNandSave::ReadAction::Proceed) return std::nullopt;
    if (action == RuntimeNandSave::ReadAction::Missing) {
        LogNandWarning(who, "treating empty or zero-filled system save '%s' as missing",
                       HostPathText(hostPath).c_str());
        return ios ? ISFS_ENOENT : NAND_RESULT_NOEXISTS;
    }
    if (action == RuntimeNandSave::ReadAction::RecoveryNeeded) {
        LogNandError(who, "system save '%s' is missing or blank but its .nandsafe.tmp contains data; "
                         "back up both files before attempting recovery",
                     HostPathText(hostPath).c_str());
    } else {
        LogNandError(who, "could not inspect system save '%s' or its write shadow; leaving data untouched",
                     HostPathText(hostPath).c_str());
    }
    return ios ? ISFS_EIO : NAND_RESULT_UNKNOWN;
}

// Create directories recursively
bool CreateDirectoryPath(const std::filesystem::path& path) {
    if (path.empty()) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec || std::filesystem::is_directory(path, ec);
}

// Check if a path exists
bool PathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

// Check if path is a directory
bool IsDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool CreateParentDirectories(const std::filesystem::path& path) {
    if (!path.has_parent_path()) {
        return false;
    }
    CreateDirectoryPath(path.parent_path());
    return true;
}

// ============================================================================
// stdio helpers shared by the NAND* library and the IOS_* device layer
// ============================================================================

int NandSeekOrigin(int32_t whence) {
    if (whence == 1) return SEEK_CUR;
    if (whence == 2) return SEEK_END;
    return SEEK_SET;
}

NandFileExtent NandProbeFileExtent(FILE* file) {
    NandFileExtent extent;
    extent.position = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    extent.size = std::ftell(file);
    std::fseek(file, extent.position, SEEK_SET);
    return extent;
}
