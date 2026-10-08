#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <limits>
#include <process.h>

namespace
{
constexpr std::uint32_t kPluginVersion = 4; // Release 1.0.1; OBSE uses a monotonic integer.
constexpr std::uint32_t kPluginInfoVersion = 3;
constexpr std::uint32_t kMinimumXObseVersion = 22;
constexpr std::uint32_t kOblivionVersion_1_2_416 = 0x010201A0;
constexpr ULONG kDesiredTimerResolution100ns = 5000; // 0.5 ms in 100 ns units.
constexpr DWORD kDefaultMaintenanceIntervalMs = 5 * 60 * 1000;
constexpr DWORD kMinimumMaintenanceIntervalMs = 30 * 1000;
constexpr DWORD kMaximumMaintenanceIntervalSeconds = (MAXDWORD - 1) / 1000;
constexpr ULONG kProcessIoPriorityClass = 33;
constexpr ULONG kIoPriorityNormal = 2;
constexpr ULONG kIoPriorityHigh = 3;
constexpr ULONG kProcessPowerThrottlingClass = 4;
constexpr ULONG kProcessPowerThrottlingCurrentVersion = 1;
constexpr ULONG kProcessPowerThrottlingExecutionSpeed = 0x1;
constexpr std::uint32_t kPluginHandleInvalid = 0xFFFFFFFF;

struct CommandInfo;
using PluginHandle = std::uint32_t;

enum OBSEInterfaceId : std::uint32_t
{
    kInterface_Console = 0,
    kInterface_Serialization = 1,
    kInterface_StringVar = 2,
    kInterface_IO = 3,
    kInterface_Messaging = 4,
    kInterface_ArrayVar = 5,
    kInterface_CommandTable = 6,
    kInterface_Script = 7,
    kInterface_Tasks = 8,
    kInterface_Input = 9,
    kInterface_EventManager = 10,
    kInterface_Tasks2 = 11,
};

enum OBSEMessageType : std::uint32_t
{
    kMessage_PostLoad = 0,
    kMessage_ExitGame = 1,
    kMessage_ExitToMainMenu = 2,
    kMessage_LoadGame = 3,
    kMessage_SaveGame = 4,
    kMessage_Precompile = 5,
    kMessage_PreLoadGame = 6,
    kMessage_ExitGame_Console = 7,
    kMessage_PostLoadGame = 8,
    kMessage_PostPostLoad = 9,
    kMessage_RuntimeScriptError = 10,
    kMessage_GameInitialized = 11,
};

struct OBSEInterface
{
    std::uint32_t obseVersion;
    std::uint32_t oblivionVersion;
    std::uint32_t editorVersion;
    std::uint32_t isEditor;
    bool (*RegisterCommand)(CommandInfo* info);
    void (*SetOpcodeBase)(std::uint32_t opcode);
    void* (*QueryInterface)(std::uint32_t id);
    PluginHandle (*GetPluginHandle)();
    bool (*RegisterTypedCommand)(CommandInfo* info, std::uint32_t returnType);
    const char* (*GetOblivionDirectory)();
    bool (*GetPluginLoaded)(const char* pluginName);
    std::uint32_t (*GetPluginVersion)(const char* pluginName);
};

struct PluginInfo
{
    std::uint32_t infoVersion;
    const char* name;
    std::uint32_t version;
};

struct OBSEMessagingInterface
{
    struct Message
    {
        const char* sender;
        std::uint32_t type;
        std::uint32_t dataLen;
        void* data;
    };

    using EventCallback = void (*)(Message* message);

    std::uint32_t version;
    bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
    bool (*Dispatch)(PluginHandle sender, std::uint32_t messageType, void* data, std::uint32_t dataLen, const char* receiver);
};

struct ProcessPowerThrottlingState
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

static_assert(offsetof(OBSEInterface, RegisterCommand) == 16, "OBSEInterface ABI drift before RegisterCommand");
static_assert(offsetof(OBSEInterface, QueryInterface) == 24, "OBSEInterface ABI drift before QueryInterface");
static_assert(offsetof(OBSEInterface, GetPluginHandle) == 28, "OBSEInterface ABI drift before GetPluginHandle");
static_assert(sizeof(PluginInfo) == 12, "PluginInfo ABI drift");

struct RuntimeOptions
{
    bool aboveNormalPriority = true;
    bool highPrecisionTimer = true;
    bool processAffinity = true;
    DWORD_PTR processAffinityMask = 0;
    bool mainThreadHighestPriority = true;
    bool mainThreadIdealProcessor = false;
    bool mainThreadHardPin = false;
    bool lowFragmentationHeap = true;
    bool workingSetPurge = false;
    bool ioPriorityBoost = true;
    bool powerThrottlingDisable = true;
    DWORD maintenanceIntervalMs = kDefaultMaintenanceIntervalMs;
};

using NtSetTimerResolutionFn = LONG (WINAPI*)(ULONG requestedResolution, BOOLEAN set, PULONG actualResolution);
using NtQueryTimerResolutionFn = LONG (WINAPI*)(PULONG maximumResolution, PULONG minimumResolution, PULONG currentResolution);
using NtSetInformationProcessFn = LONG (WINAPI*)(HANDLE process, ULONG processInformationClass, PVOID processInformation, ULONG processInformationLength);
using SetProcessInformationFn = BOOL (WINAPI*)(HANDLE process, ULONG processInformationClass, LPVOID processInformation, DWORD processInformationSize);
using GetProcessDEPPolicyFn = BOOL (WINAPI*)(HANDLE process, LPDWORD flags, PBOOL permanent);

HINSTANCE g_instance = nullptr;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
HANDLE g_worker = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_mainThread = nullptr;
DWORD g_mainThreadId = 0;
DWORD_PTR g_mainThreadPinMask = 0;
RuntimeOptions g_options;
PluginHandle g_pluginHandle = kPluginHandleInvalid;
OBSEMessagingInterface* g_messaging = nullptr;
std::atomic_bool g_initialized{false};
bool g_activated = false; // Guarded by g_runtimeLock.
std::atomic_bool g_running{false};
UINT g_timePeriod = 0;
bool g_timerResolutionSet = false;
ULONG g_timerResolutionRequest100ns = kDesiredTimerResolution100ns;
NtSetTimerResolutionFn g_ntSetTimerResolution = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;
SRWLOCK g_runtimeLock = SRWLOCK_INIT;
SRWLOCK g_threadLock = SRWLOCK_INIT;

struct ExclusiveLock
{
    explicit ExclusiveLock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    SRWLOCK& lock_;
};

void ReapplyMainThreadHints();
void ApplyMainThreadHints();
void StartMaintenance();
void ActivateRuntimeTuning();
void ShutdownRuntime();

bool NtSuccess(LONG status)
{
    return status >= 0;
}

bool BuildPluginSiblingPath(const wchar_t* fileName, wchar_t* output, DWORD outputCount)
{
    if (!output || outputCount == 0)
    {
        return false;
    }

    output[0] = L'\0';
    if (!fileName)
    {
        return false;
    }
    const DWORD length = GetModuleFileNameW(g_instance, output, outputCount);
    if (!length || length >= outputCount)
    {
        output[0] = L'\0';
        return false;
    }

    wchar_t* slash = std::wcsrchr(output, L'\\');
    if (!slash)
    {
        output[0] = L'\0';
        return false;
    }

    const std::size_t directoryLength = static_cast<std::size_t>(slash + 1 - output);
    const std::size_t nameLength = std::wcslen(fileName);
    if (nameLength >= outputCount - directoryLength)
    {
        output[0] = L'\0';
        return false;
    }
    std::wmemcpy(output + directoryLength, fileName, nameLength + 1);
    return true;
}

void Log(const char* format, ...)
{
    char message[1024]{};
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);

    char line[1280]{};
    SYSTEMTIME now{};
    GetLocalTime(&now);
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%02u:%02u:%02u.%03u] %s\r\n",
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        message);

    OutputDebugStringA("[OblivionPerformanceRuntime] ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");

    ExclusiveLock lock(g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(g_logFile, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        FlushFileBuffers(g_logFile);
    }
}

void OpenLog()
{
    {
        ExclusiveLock lock(g_logLock);
        if (g_logFile != INVALID_HANDLE_VALUE)
        {
            return;
        }
        wchar_t path[MAX_PATH]{};
        if (!BuildPluginSiblingPath(L"OblivionPerformanceRuntime.log", path, MAX_PATH))
        {
            return;
        }
        g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    Log("log opened (plugin version %u)", kPluginVersion);
}

void CloseLog(bool writeCloseMessage)
{
    if (writeCloseMessage)
    {
        Log("log closed");
    }
    ExclusiveLock lock(g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

int ReadIniInt(const wchar_t* iniPath, const wchar_t* key, int defaultValue)
{
    return GetPrivateProfileIntW(L"Tuning", key, defaultValue, iniPath);
}

bool ReadIniBool(const wchar_t* iniPath, const wchar_t* key, bool defaultValue)
{
    return ReadIniInt(iniPath, key, defaultValue ? 1 : 0) != 0;
}

bool ParseUnsigned(const wchar_t* raw, unsigned long long maximum, unsigned long long& value, bool allowHex)
{
    while (std::iswspace(*raw))
    {
        ++raw;
    }
    if (*raw < L'0' || *raw > L'9')
    {
        return false;
    }
    const int base = allowHex && raw[0] == L'0' && (raw[1] == L'x' || raw[1] == L'X') ? 16 : 10;
    errno = 0;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoull(raw, &end, base);
    if (end == raw || errno == ERANGE || parsed > maximum)
    {
        return false;
    }
    while (std::iswspace(*end))
    {
        ++end;
    }
    if (*end != L'\0')
    {
        return false;
    }
    value = parsed;
    return true;
}

DWORD ParseMaintenanceInterval(const wchar_t* raw)
{
    unsigned long long seconds = 0;
    if (!ParseUnsigned(raw, (std::numeric_limits<unsigned long long>::max)(), seconds, false))
    {
        Log("invalid MaintenanceIntervalSeconds; using 300 seconds");
        return kDefaultMaintenanceIntervalMs;
    }
    seconds = std::clamp(seconds, static_cast<unsigned long long>(kMinimumMaintenanceIntervalMs / 1000),
        static_cast<unsigned long long>(kMaximumMaintenanceIntervalSeconds));
    return static_cast<DWORD>(seconds * 1000ULL);
}

DWORD_PTR ReadIniAffinityMask(const wchar_t* iniPath)
{
    wchar_t raw[64]{};
    const DWORD length = GetPrivateProfileStringW(L"Tuning", L"ProcessAffinityMask", L"0", raw, static_cast<DWORD>(std::size(raw)), iniPath);
    unsigned long long value = 0;
    if (length >= std::size(raw) - 1 || !ParseUnsigned(raw, (std::numeric_limits<DWORD_PTR>::max)(), value, true))
    {
        Log("invalid ProcessAffinityMask value; using current process mask");
        return 0;
    }

    return static_cast<DWORD_PTR>(value);
}

RuntimeOptions LoadOptions()
{
    RuntimeOptions options{};
    wchar_t iniPath[MAX_PATH]{};
    if (!BuildPluginSiblingPath(L"OblivionPerformanceRuntime.ini", iniPath, MAX_PATH))
    {
        Log("unable to resolve INI path; using defaults");
        return options;
    }

    if (GetFileAttributesW(iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        Log("INI not found; using built-in defaults");
    }

    options.aboveNormalPriority = ReadIniBool(iniPath, L"AboveNormalPriority", options.aboveNormalPriority);
    options.highPrecisionTimer = ReadIniBool(iniPath, L"HighPrecisionTimer", options.highPrecisionTimer);
    options.processAffinity = ReadIniBool(iniPath, L"ProcessAffinity", options.processAffinity);
    options.processAffinityMask = ReadIniAffinityMask(iniPath);
    options.mainThreadHighestPriority = ReadIniBool(iniPath, L"MainThreadHighestPriority", options.mainThreadHighestPriority);
    options.mainThreadIdealProcessor = ReadIniBool(iniPath, L"MainThreadIdealProcessor", options.mainThreadIdealProcessor);
    options.mainThreadHardPin = ReadIniBool(iniPath, L"MainThreadHardPin", options.mainThreadHardPin);
    options.lowFragmentationHeap = ReadIniBool(iniPath, L"LowFragmentationHeap", options.lowFragmentationHeap);
    options.workingSetPurge = ReadIniBool(iniPath, L"WorkingSetPurge", options.workingSetPurge);
    options.ioPriorityBoost = ReadIniBool(iniPath, L"IoPriorityBoost", options.ioPriorityBoost);
    options.powerThrottlingDisable = ReadIniBool(iniPath, L"PowerThrottlingDisable", options.powerThrottlingDisable);

    wchar_t interval[64]{};
    const DWORD intervalLength = GetPrivateProfileStringW(L"Tuning", L"MaintenanceIntervalSeconds", L"300", interval,
        static_cast<DWORD>(std::size(interval)), iniPath);
    options.maintenanceIntervalMs = ParseMaintenanceInterval(intervalLength >= std::size(interval) - 1 ? L"" : interval);

    Log(
        "options: priority=%d timer=%d affinity=%d affinityMask=0x%Ix mainThreadPriority=%d idealCpu=%d hardPin=%d lfh=%d purge=%d io=%d power=%d intervalMs=%lu",
        options.aboveNormalPriority ? 1 : 0,
        options.highPrecisionTimer ? 1 : 0,
        options.processAffinity ? 1 : 0,
        static_cast<std::size_t>(options.processAffinityMask),
        options.mainThreadHighestPriority ? 1 : 0,
        options.mainThreadIdealProcessor ? 1 : 0,
        options.mainThreadHardPin ? 1 : 0,
        options.lowFragmentationHeap ? 1 : 0,
        options.workingSetPurge ? 1 : 0,
        options.ioPriorityBoost ? 1 : 0,
        options.powerThrottlingDisable ? 1 : 0,
        options.maintenanceIntervalMs);

    return options;
}

bool IsWow64ProcessCompat(HANDLE process)
{
    BOOL wow64 = FALSE;
    using IsWow64Process2Fn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto* kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(kernel32, "IsWow64Process2"));
    if (isWow64Process2)
    {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (isWow64Process2(process, &processMachine, &nativeMachine))
        {
            return processMachine != IMAGE_FILE_MACHINE_UNKNOWN;
        }
    }

    return IsWow64Process(process, &wow64) && wow64;
}

enum class AddressAwareness { Unknown, Disabled, Enabled };

AddressAwareness ReadImageAddressAwareness(const void* image, std::size_t available)
{
    if (!image || available < sizeof(IMAGE_DOS_HEADER))
    {
        return AddressAwareness::Unknown;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)))
    {
        return AddressAwareness::Unknown;
    }
    const auto offset = static_cast<std::size_t>(dos.e_lfanew);
    if (offset > available || available - offset < sizeof(IMAGE_NT_HEADERS32))
    {
        return AddressAwareness::Unknown;
    }
    IMAGE_NT_HEADERS32 nt{};
    std::memcpy(&nt, static_cast<const std::uint8_t*>(image) + offset, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386
        || nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32)
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        return AddressAwareness::Unknown;
    }
    return (nt.FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)
        ? AddressAwareness::Enabled : AddressAwareness::Disabled;
}

AddressAwareness CurrentExeAddressAwareness()
{
    // Inspect the running image, independent of Unicode paths, sharing permissions,
    // or replacement of the executable on disk. Restrict parsing to readable headers.
    HMODULE image = GetModuleHandleW(nullptr);
    MEMORY_BASIC_INFORMATION region{};
    if (!image || !VirtualQuery(image, &region, sizeof(region)) || region.State != MEM_COMMIT
        || region.BaseAddress != image || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        || !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
            | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    {
        return AddressAwareness::Unknown;
    }
    return ReadImageAddressAwareness(image, region.RegionSize);
}

DWORD CountAffinityBits(DWORD_PTR mask)
{
    DWORD count = 0;
    for (DWORD bit = 0; bit < sizeof(DWORD_PTR) * CHAR_BIT; ++bit)
    {
        if ((mask & (static_cast<DWORD_PTR>(1) << bit)) != 0)
        {
            ++count;
        }
    }
    return count;
}

DWORD SelectStableProcessor(DWORD_PTR mask)
{
    DWORD selected = MAXDWORD;
    for (DWORD bit = 0; bit < sizeof(DWORD_PTR) * CHAR_BIT; ++bit)
    {
        if ((mask & (static_cast<DWORD_PTR>(1) << bit)) != 0)
        {
            selected = bit;
        }
    }
    return selected;
}

void CaptureMainThreadHandle()
{
    g_mainThreadId = GetCurrentThreadId();
    HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process, GetCurrentThread(), process, &g_mainThread, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, 0))
    {
        g_mainThread = nullptr;
        Log("DuplicateHandle for main thread failed: %lu", GetLastError());
    }
}

void HandleOBSEMessage(OBSEMessagingInterface::Message* message)
{
    if (!message || !message->sender || std::strcmp(message->sender, "OBSE") != 0)
    {
        return;
    }

    switch (message->type)
    {
        case kMessage_GameInitialized:
        {
            ExclusiveLock lock(g_runtimeLock);
            if (!g_initialized.load() || g_activated)
            {
                break;
            }
            Log("received OBSE game initialized message");
            ActivateRuntimeTuning();
            break;
        }

        case kMessage_ExitGame:
        case kMessage_ExitGame_Console:
            Log("received OBSE exit message; shutting down runtime tuning");
            ShutdownRuntime();
            break;

        default:
            break;
    }
}

bool RegisterOBSEMessaging(const OBSEInterface* obse)
{
    if (!obse || !obse->GetPluginHandle || !obse->QueryInterface)
    {
        Log("required xOBSE interface functions are missing");
        return false;
    }

    Log("OBSE version=%lu Oblivion version=0x%08lX", obse->obseVersion, obse->oblivionVersion);
    g_pluginHandle = obse->GetPluginHandle();
    Log("plugin handle=%lu", g_pluginHandle);
    if (g_pluginHandle == kPluginHandleInvalid)
    {
        Log("xOBSE returned an invalid plugin handle");
        return false;
    }

    g_messaging = static_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));
    if (!g_messaging || g_messaging->version < 1 || !g_messaging->RegisterListener)
    {
        Log("required xOBSE messaging interface is missing or unsupported");
        return false;
    }

    if (g_messaging->RegisterListener(g_pluginHandle, "OBSE", HandleOBSEMessage))
    {
        Log("registered OBSE messaging listener");
        return true;
    }
    else
    {
        Log("failed to register OBSE messaging listener");
        return false;
    }
}

void ApplyProcessPriorityAndAffinity()
{
    HANDLE process = GetCurrentProcess();

    if (g_options.aboveNormalPriority)
    {
        if (SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS))
        {
            Log("process priority set to ABOVE_NORMAL_PRIORITY_CLASS");
        }
        else
        {
            Log("SetPriorityClass failed: %lu", GetLastError());
        }
    }

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    DWORD_PTR effectiveMask = 0;
    if (GetProcessAffinityMask(process, &processMask, &systemMask))
    {
        effectiveMask = processMask;
        // A zero mask preserves the existing policy without issuing a redundant setter.
        if (g_options.processAffinity && g_options.processAffinityMask)
        {
            const DWORD_PTR requested = g_options.processAffinityMask;
            if ((requested & systemMask) != requested)
            {
                Log("ProcessAffinityMask contains unavailable CPUs; preserving current mask");
            }
            else if (!SetProcessAffinityMask(process, requested))
            {
                Log("SetProcessAffinityMask failed: %lu; using current mask", GetLastError());
            }
            // Re-read after either success or failure; never select a CPU from an unapplied mask.
            if (!GetProcessAffinityMask(process, &effectiveMask, &systemMask))
            {
                effectiveMask = 0;
                Log("unable to refresh process affinity: %lu; thread hints will query again at game initialization", GetLastError());
            }
            else
            {
                Log("current process affinity mask: 0x%Ix", static_cast<std::size_t>(effectiveMask));
            }
        }
    }
    else
    {
        Log("GetProcessAffinityMask failed: %lu; process affinity unchanged", GetLastError());
    }

}

// Caller owns g_threadLock. Only target the thread captured by GameInitialized.
void ApplyMainThreadHints()
{
    if (!g_mainThread)
    {
        return;
    }
    HANDLE mainThread = g_mainThread;
    if (g_options.mainThreadHighestPriority)
    {
        if (SetThreadPriority(mainThread, THREAD_PRIORITY_HIGHEST))
        {
            Log("game main thread %lu priority set to THREAD_PRIORITY_HIGHEST", g_mainThreadId);
        }
        else
        {
            Log("SetThreadPriority for main thread failed: %lu", GetLastError());
        }
    }

    if (!g_options.mainThreadIdealProcessor && !g_options.mainThreadHardPin)
    {
        return;
    }
    DWORD_PTR effectiveMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &effectiveMask, &systemMask) || !effectiveMask)
    {
        Log("unable to read current process affinity; skipping thread CPU hints");
        return;
    }

    const DWORD selectedProcessor = SelectStableProcessor(effectiveMask);
    if (selectedProcessor == MAXDWORD)
    {
        return;
    }

    if (g_options.mainThreadIdealProcessor)
    {
        const DWORD previous = SetThreadIdealProcessor(mainThread, selectedProcessor);
        if (previous != MAXDWORD)
        {
            Log("main thread ideal processor set to CPU %lu", selectedProcessor);
        }
        else
        {
            Log("SetThreadIdealProcessor failed: %lu", GetLastError());
        }
    }

    if (g_options.mainThreadHardPin && CountAffinityBits(effectiveMask) > 1)
    {
        g_mainThreadPinMask = static_cast<DWORD_PTR>(1) << selectedProcessor;
        if (SetThreadAffinityMask(mainThread, g_mainThreadPinMask))
        {
            Log("main thread hard-pinned to CPU %lu", selectedProcessor);
        }
        else
        {
            Log("SetThreadAffinityMask failed: %lu", GetLastError());
            g_mainThreadPinMask = 0;
        }
    }
}

void ApplyTimerResolution()
{
    if (!g_options.highPrecisionTimer)
    {
        return;
    }

    if (timeBeginPeriod(1) == TIMERR_NOERROR)
    {
        g_timePeriod = 1;
        Log("timeBeginPeriod(1) succeeded");
    }
    else
    {
        Log("timeBeginPeriod(1) failed");
    }

    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ntSetTimerResolution = reinterpret_cast<NtSetTimerResolutionFn>(GetProcAddress(ntdll, "NtSetTimerResolution"));
    auto ntQueryTimerResolution = reinterpret_cast<NtQueryTimerResolutionFn>(GetProcAddress(ntdll, "NtQueryTimerResolution"));
    if (!g_ntSetTimerResolution)
    {
        Log("NtSetTimerResolution unavailable");
        return;
    }

    ULONG requested = kDesiredTimerResolution100ns;
    ULONG actual = 0;
    if (ntQueryTimerResolution)
    {
        ULONG maximumInterval = 0;
        ULONG minimumInterval = 0;
        ULONG currentInterval = 0;
        const LONG queryStatus = ntQueryTimerResolution(&maximumInterval, &minimumInterval, &currentInterval);
        if (NtSuccess(queryStatus) && minimumInterval && maximumInterval)
        {
            const ULONG bestInterval = std::min(minimumInterval, maximumInterval);
            const ULONG worstInterval = std::max(minimumInterval, maximumInterval);
            requested = std::clamp(kDesiredTimerResolution100ns, bestInterval, worstInterval);
            Log(
                "timer resolution range: best %.3fms, default %.3fms, current %.3fms",
                bestInterval / 10000.0,
                worstInterval / 10000.0,
                currentInterval / 10000.0);
        }
    }

    const LONG setStatus = g_ntSetTimerResolution(requested, TRUE, &actual);
    if (NtSuccess(setStatus))
    {
        g_timerResolutionSet = true;
        g_timerResolutionRequest100ns = requested;
        Log("requested NT timer resolution %.3fms; actual %.3fms", requested / 10000.0, actual / 10000.0);
    }
    else
    {
        Log("NtSetTimerResolution failed: 0x%08lX", static_cast<unsigned long>(setStatus));
    }
}

void ReleaseTimerResolution()
{
    if (g_timerResolutionSet && g_ntSetTimerResolution)
    {
        ULONG actual = 0;
        g_ntSetTimerResolution(g_timerResolutionRequest100ns, FALSE, &actual);
        g_timerResolutionSet = false;
    }

    if (g_timePeriod)
    {
        timeEndPeriod(g_timePeriod);
        g_timePeriod = 0;
    }
}

void ApplyHeapLowFragmentationMode()
{
    if (!g_options.lowFragmentationHeap)
    {
        return;
    }

    // Oblivion's FormHeap/MemoryPool allocator is separate from the Windows default heap.
    // Other DLLs own private heaps whose lifetime we cannot synchronize with.
    HANDLE heap = GetProcessHeap();
    ULONG lfh = 2;
    if (heap && HeapSetInformation(heap, HeapCompatibilityInformation, &lfh, sizeof(lfh)))
    {
        Log("LFH requested on Windows default process heap only");
    }
    else
    {
        Log("default process heap LFH request failed: %lu", GetLastError());
    }
}

void ApplyIoPriority()
{
    if (!g_options.ioPriorityBoost)
    {
        return;
    }

    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    auto ntSetInformationProcess = reinterpret_cast<NtSetInformationProcessFn>(GetProcAddress(ntdll, "NtSetInformationProcess"));
    if (!ntSetInformationProcess)
    {
        Log("NtSetInformationProcess unavailable for I/O priority");
        return;
    }

    ULONG ioPriority = kIoPriorityHigh;
    LONG status = ntSetInformationProcess(GetCurrentProcess(), kProcessIoPriorityClass, &ioPriority, sizeof(ioPriority));
    if (NtSuccess(status))
    {
        Log("process I/O priority hint set to high");
        return;
    }

    ioPriority = kIoPriorityNormal;
    status = ntSetInformationProcess(GetCurrentProcess(), kProcessIoPriorityClass, &ioPriority, sizeof(ioPriority));
    if (NtSuccess(status))
    {
        Log("high I/O priority was rejected; process I/O priority hint set to normal");
    }
    else
    {
        Log("process I/O priority request failed: 0x%08lX", static_cast<unsigned long>(status));
    }
}

void DisablePowerThrottling()
{
    if (!g_options.powerThrottlingDisable)
    {
        return;
    }

    auto* kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto setProcessInformation = reinterpret_cast<SetProcessInformationFn>(GetProcAddress(kernel32, "SetProcessInformation"));
    if (!setProcessInformation)
    {
        Log("SetProcessInformation unavailable; power throttling disable is not supported on this Windows version");
        return;
    }

    ProcessPowerThrottlingState state{};
    state.Version = kProcessPowerThrottlingCurrentVersion;
    state.ControlMask = kProcessPowerThrottlingExecutionSpeed;
    state.StateMask = 0;
    if (setProcessInformation(GetCurrentProcess(), kProcessPowerThrottlingClass, &state, sizeof(state)))
    {
        Log("process power throttling disabled");
    }
    else
    {
        Log("power throttling request failed: %lu", GetLastError());
    }
}

void ReportDepPolicy()
{
    auto* kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto getProcessDEPPolicy = reinterpret_cast<GetProcessDEPPolicyFn>(GetProcAddress(kernel32, "GetProcessDEPPolicy"));
    if (!getProcessDEPPolicy)
    {
        return;
    }

    DWORD flags = 0;
    BOOL permanent = FALSE;
    if (getProcessDEPPolicy(GetCurrentProcess(), &flags, &permanent))
    {
        Log(
            "DEP policy: enabled=%s, atlThunkEmulationDisabled=%s, permanent=%s",
            (flags & PROCESS_DEP_ENABLE) ? "yes" : "no",
            (flags & PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION) ? "yes" : "no",
            permanent ? "yes" : "no");
    }
}

void TrimWorkingSet()
{
    if (!g_options.workingSetPurge)
    {
        return;
    }

    if (EmptyWorkingSet(GetCurrentProcess()))
    {
        Log("working set purged");
    }
    else
    {
        Log("EmptyWorkingSet failed: %lu", GetLastError());
    }
}

void ReapplyMainThreadHints()
{
    ExclusiveLock lock(g_threadLock);
    if (!g_mainThread)
    {
        return;
    }

    if (g_options.mainThreadHighestPriority)
    {
        SetThreadPriority(g_mainThread, THREAD_PRIORITY_HIGHEST);
    }

    if (g_mainThreadPinMask)
    {
        SetThreadAffinityMask(g_mainThread, g_mainThreadPinMask);
    }
}

unsigned __stdcall MaintenanceThread(void*)
{
    while (g_running.load(std::memory_order_acquire))
    {
        const DWORD waitResult = WaitForSingleObject(g_stopEvent, g_options.maintenanceIntervalMs);
        if (waitResult != WAIT_TIMEOUT || !g_running.load(std::memory_order_acquire))
        {
            break;
        }

        ReapplyMainThreadHints();
        TrimWorkingSet();
    }
    return 0;
}

void StartMaintenance()
{
    if (!g_options.workingSetPurge && !(g_mainThread && (g_options.mainThreadHighestPriority || g_mainThreadPinMask)))
    {
        return;
    }
    if (g_running.exchange(true))
    {
        return;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent)
    {
        g_running.store(false);
        Log("CreateEvent for maintenance thread failed: %lu", GetLastError());
        return;
    }

    g_worker = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, MaintenanceThread, nullptr, 0, nullptr));
    if (!g_worker)
    {
        const int error = errno;
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        g_running.store(false);
        Log("_beginthreadex for maintenance failed: errno=%d", error);
        return;
    }

    SetThreadPriority(g_worker, THREAD_PRIORITY_BELOW_NORMAL);
    Log("maintenance thread started; interval=%lums", g_options.maintenanceIntervalMs);
}

bool StopMaintenance()
{
    g_running.store(false, std::memory_order_release);

    if (g_stopEvent && !SetEvent(g_stopEvent))
    {
        Log("unable to signal maintenance shutdown: %lu; retaining resources", GetLastError());
        return false;
    }

    if (g_worker)
    {
        // Only called outside DllMain. A timeout must never authorize closing live resources.
        if (WaitForSingleObject(g_worker, INFINITE) != WAIT_OBJECT_0)
        {
            Log("unable to join maintenance thread: %lu; retaining resources", GetLastError());
            return false;
        }
        CloseHandle(g_worker);
        g_worker = nullptr;
    }

    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    return true;
}

void CloseMainThreadHandle()
{
    ExclusiveLock lock(g_threadLock);
    if (g_mainThread)
    {
        CloseHandle(g_mainThread);
        g_mainThread = nullptr;
    }
}

// Called under g_runtimeLock by the required xOBSE main-loop notification.
void ActivateRuntimeTuning()
{
    ApplyProcessPriorityAndAffinity();
    ApplyTimerResolution();
    ApplyHeapLowFragmentationMode();
    ApplyIoPriority();
    DisablePowerThrottling();
    {
        ExclusiveLock threadLock(g_threadLock);
        CaptureMainThreadHandle();
        ApplyMainThreadHints();
    }
    g_activated = true;
    StartMaintenance();
}

bool InitializeRuntime(const OBSEInterface* obse)
{
    ExclusiveLock lock(g_runtimeLock);
    if (g_initialized.exchange(true))
    {
        Log("runtime tuning already initialized");
        return true;
    }

    OpenLog();
    g_options = LoadOptions();
    if (!RegisterOBSEMessaging(obse))
    {
        Log("runtime tuning rejected: xOBSE messaging is required");
        CloseLog(true);
        g_initialized.store(false);
        return false;
    }
    Log("all tuning awaits GameInitialized (requires xOBSE 22.10 or newer)");

    const bool wow64 = IsWow64ProcessCompat(GetCurrentProcess());
    const auto largeAddressAware = CurrentExeAddressAwareness();
    Log("64-bit OS/WOW64: %s; Large Address Aware: %s", wow64 ? "yes" : "no",
        largeAddressAware == AddressAwareness::Unknown ? "unknown" :
        (largeAddressAware == AddressAwareness::Enabled ? "yes" : "no"));
    if (largeAddressAware == AddressAwareness::Disabled)
    {
        Log("Oblivion.exe is not Large Address Aware. Apply a 4GB patch before expecting >2GB address space.");
    }
    ReportDepPolicy();

    return true;
}

void ShutdownRuntime()
{
    ExclusiveLock lock(g_runtimeLock);
    if (!g_initialized.load())
    {
        return;
    }

    if (!StopMaintenance())
    {
        return;
    }
    ReleaseTimerResolution();
    CloseMainThreadHandle();
    g_mainThreadPinMask = 0;
    g_activated = false;
    CloseLog(true);
    g_initialized.store(false);
}

bool IsSupportedRuntime(const OBSEInterface* obse)
{
    // The public interface exposes the major version only. GameInitialized,
    // introduced in xOBSE 22.10, is required to activate any tuning.
    return obse && !obse->isEditor && obse->obseVersion >= kMinimumXObseVersion
        && obse->oblivionVersion == kOblivionVersion_1_2_416
        && obse->QueryInterface && obse->GetPluginHandle;
}
}

extern "C" __declspec(dllexport) bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
{
    if (info)
    {
        info->infoVersion = kPluginInfoVersion;
        info->name = "OblivionPerformanceRuntime";
        info->version = kPluginVersion;
    }

    return info && IsSupportedRuntime(obse);
}

extern "C" __declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse)
{
    if (!IsSupportedRuntime(obse))
    {
        return false;
    }

    // OBSE callbacks cannot be unregistered. Keep their code and any worker mapped for
    // the process lifetime, including if an external caller attempts FreeLibrary.
    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&g_instance), &pinnedModule))
    {
        return false;
    }
    return InitializeRuntime(obse);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_instance = instance;
    }

    // Keep CRT thread notifications. Normal cleanup runs through OBSE messaging;
    // abrupt process termination is left to Windows, with no loader-lock waits.

    return TRUE;
}
