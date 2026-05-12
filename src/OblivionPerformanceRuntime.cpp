#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <vector>

namespace
{
constexpr std::uint32_t kPluginVersion = 3;
constexpr std::uint32_t kPluginInfoVersion = 3;
constexpr std::uint32_t kOblivionVersion_1_2_416 = 0x010201A0;
constexpr ULONG kDesiredTimerResolution100ns = 5000; // 0.5 ms in 100 ns units.
constexpr DWORD kDefaultMaintenanceIntervalMs = 5 * 60 * 1000;
constexpr DWORD kMinimumMaintenanceIntervalMs = 30 * 1000;
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
    bool mainThreadIdealProcessor = true;
    bool mainThreadHardPin = false;
    bool lowFragmentationHeap = true;
    bool workingSetPurge = true;
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
std::atomic_bool g_running{false};
UINT g_timePeriod = 0;
bool g_timerResolutionSet = false;
ULONG g_timerResolutionRequest100ns = kDesiredTimerResolution100ns;
NtSetTimerResolutionFn g_ntSetTimerResolution = nullptr;

void ReapplyMainThreadHints();
void ShutdownRuntime(bool waitForWorker, bool writeCloseMessage);

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
    if (!GetModuleFileNameW(g_instance, output, outputCount))
    {
        return false;
    }

    wchar_t* slash = std::wcsrchr(output, L'\\');
    if (!slash)
    {
        return false;
    }

    *(slash + 1) = L'\0';
    return wcscat_s(output, outputCount, fileName) == 0;
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

    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(g_logFile, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        FlushFileBuffers(g_logFile);
    }
}

void OpenLog()
{
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }

    wchar_t path[MAX_PATH]{};
    if (!BuildPluginSiblingPath(L"OblivionPerformanceRuntime.log", path, MAX_PATH))
    {
        return;
    }

    g_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        Log("log opened");
    }
}

void CloseLog(bool writeCloseMessage)
{
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        if (writeCloseMessage)
        {
            Log("log closed");
        }
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

DWORD_PTR ReadIniAffinityMask(const wchar_t* iniPath)
{
    wchar_t raw[64]{};
    GetPrivateProfileStringW(L"Tuning", L"ProcessAffinityMask", L"0", raw, static_cast<DWORD>(std::size(raw)), iniPath);

    wchar_t* end = nullptr;
    const unsigned long long value = std::wcstoull(raw, &end, 0);
    if (end == raw)
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

    const int intervalSeconds = ReadIniInt(iniPath, L"MaintenanceIntervalSeconds", static_cast<int>(kDefaultMaintenanceIntervalMs / 1000));
    const DWORD intervalMs = static_cast<DWORD>(std::max(intervalSeconds, 1) * 1000);
    options.maintenanceIntervalMs = std::max(intervalMs, kMinimumMaintenanceIntervalMs);

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

bool CurrentExeIsLargeAddressAware()
{
    char path[MAX_PATH]{};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
    {
        return false;
    }

    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize))
    {
        CloseHandle(file);
        return false;
    }

    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping)
    {
        CloseHandle(file);
        return false;
    }

    auto* base = static_cast<const std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    bool aware = false;
    if (base && fileSize.QuadPart >= static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER)))
    {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const LONGLONG ntOffset = dos->e_lfanew;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
            ntOffset > 0 &&
            ntOffset + static_cast<LONGLONG>(sizeof(IMAGE_NT_HEADERS32)) <= fileSize.QuadPart)
        {
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + ntOffset);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                aware = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
            }
        }
    }

    if (base)
    {
        UnmapViewOfFile(base);
    }

    CloseHandle(mapping);
    CloseHandle(file);
    return aware;
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

HANDLE MainThreadHandleForCurrentCall()
{
    return g_mainThread ? g_mainThread : GetCurrentThread();
}

void HandleOBSEMessage(OBSEMessagingInterface::Message* message)
{
    if (!message)
    {
        return;
    }

    switch (message->type)
    {
        case kMessage_GameInitialized:
            Log("received OBSE game initialized message");
            ReapplyMainThreadHints();
            break;

        case kMessage_ExitGame:
        case kMessage_ExitGame_Console:
            Log("received OBSE exit message; shutting down runtime tuning");
            ShutdownRuntime(true, true);
            break;

        default:
            break;
    }
}

void RegisterOBSEMessaging(const OBSEInterface* obse)
{
    if (!obse)
    {
        Log("OBSE interface is null; messaging unavailable");
        return;
    }

    Log("OBSE version=%lu Oblivion version=0x%08lX", obse->obseVersion, obse->oblivionVersion);
    if (obse->oblivionVersion && obse->oblivionVersion != kOblivionVersion_1_2_416)
    {
        Log("unexpected Oblivion runtime version; process tuning will continue without address hooks");
    }

    if (obse->obseVersion >= 15 && obse->GetPluginHandle)
    {
        g_pluginHandle = obse->GetPluginHandle();
        Log("plugin handle=%lu", g_pluginHandle);
    }

    if (obse->obseVersion < 17 || !obse->QueryInterface || g_pluginHandle == kPluginHandleInvalid)
    {
        Log("OBSE messaging unavailable; DllMain will provide fallback cleanup");
        return;
    }

    g_messaging = static_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));
    if (!g_messaging || g_messaging->version < 1 || !g_messaging->RegisterListener)
    {
        Log("OBSE messaging interface missing or unsupported");
        return;
    }

    if (g_messaging->RegisterListener(g_pluginHandle, "OBSE", HandleOBSEMessage))
    {
        Log("registered OBSE messaging listener");
    }
    else
    {
        Log("failed to register OBSE messaging listener");
    }
}

void ApplyPriorityAndAffinity()
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
    if (g_options.processAffinity && GetProcessAffinityMask(process, &processMask, &systemMask))
    {
        effectiveMask = g_options.processAffinityMask ? (g_options.processAffinityMask & systemMask) : processMask;
        if (!effectiveMask)
        {
            effectiveMask = systemMask;
        }

        if (effectiveMask && SetProcessAffinityMask(process, effectiveMask))
        {
            Log(
                "process affinity mask applied: requested=0x%Ix system=0x%Ix effective=0x%Ix",
                static_cast<std::size_t>(g_options.processAffinityMask),
                static_cast<std::size_t>(systemMask),
                static_cast<std::size_t>(effectiveMask));
        }
        else if (effectiveMask)
        {
            Log("SetProcessAffinityMask failed: %lu", GetLastError());
        }
    }

    HANDLE mainThread = MainThreadHandleForCurrentCall();
    if (g_options.mainThreadHighestPriority)
    {
        if (SetThreadPriority(mainThread, THREAD_PRIORITY_HIGHEST))
        {
            Log("main thread %lu priority set to THREAD_PRIORITY_HIGHEST", g_mainThreadId);
        }
        else
        {
            Log("SetThreadPriority for main thread failed: %lu", GetLastError());
        }
    }

    if (!effectiveMask)
    {
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

    DWORD heapCount = GetProcessHeaps(0, nullptr);
    if (!heapCount)
    {
        return;
    }

    std::vector<HANDLE> heaps(heapCount);
    heapCount = GetProcessHeaps(heapCount, heaps.data());
    ULONG lfh = 2;
    DWORD enabled = 0;
    for (DWORD i = 0; i < heapCount; ++i)
    {
        if (HeapSetInformation(heaps[i], HeapCompatibilityInformation, &lfh, sizeof(lfh)))
        {
            ++enabled;
        }
    }

    Log("LFH requested on %lu/%lu process heaps", enabled, heapCount);
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

DWORD WINAPI MaintenanceThread(void*)
{
    while (g_running.load(std::memory_order_acquire))
    {
        const DWORD waitResult = WaitForSingleObject(g_stopEvent, g_options.maintenanceIntervalMs);
        if (waitResult != WAIT_TIMEOUT)
        {
            break;
        }

        ReapplyMainThreadHints();
        ApplyHeapLowFragmentationMode();
        TrimWorkingSet();
    }
    return 0;
}

void StartMaintenance()
{
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

    g_worker = CreateThread(nullptr, 0, MaintenanceThread, nullptr, 0, nullptr);
    if (!g_worker)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        g_running.store(false);
        Log("CreateThread for maintenance failed: %lu", GetLastError());
        return;
    }

    SetThreadPriority(g_worker, THREAD_PRIORITY_BELOW_NORMAL);
    Log("maintenance thread started; interval=%lums", g_options.maintenanceIntervalMs);
}

void StopMaintenance(bool waitForWorker)
{
    if (!g_running.exchange(false))
    {
        return;
    }

    if (g_stopEvent)
    {
        SetEvent(g_stopEvent);
    }

    if (g_worker)
    {
        if (waitForWorker)
        {
            WaitForSingleObject(g_worker, 2000);
        }
        CloseHandle(g_worker);
        g_worker = nullptr;
    }

    if (g_stopEvent)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

void CloseMainThreadHandle()
{
    if (g_mainThread)
    {
        CloseHandle(g_mainThread);
        g_mainThread = nullptr;
    }
}

void ApplyRuntimeTuning(const OBSEInterface* obse)
{
    if (g_initialized.exchange(true))
    {
        Log("runtime tuning already initialized");
        return;
    }

    OpenLog();
    g_options = LoadOptions();
    RegisterOBSEMessaging(obse);
    CaptureMainThreadHandle();

    const bool wow64 = IsWow64ProcessCompat(GetCurrentProcess());
    const bool largeAddressAware = CurrentExeIsLargeAddressAware();
    Log("64-bit OS/WOW64: %s; Large Address Aware: %s", wow64 ? "yes" : "no", largeAddressAware ? "yes" : "no");
    if (!largeAddressAware)
    {
        Log("Oblivion.exe is not Large Address Aware. Apply a 4GB patch before expecting >2GB address space.");
    }
    ReportDepPolicy();

    ApplyPriorityAndAffinity();
    ApplyTimerResolution();
    ApplyHeapLowFragmentationMode();
    ApplyIoPriority();
    DisablePowerThrottling();
    StartMaintenance();
}

void ShutdownRuntime(bool waitForWorker, bool writeCloseMessage)
{
    if (!g_initialized.exchange(false))
    {
        return;
    }

    StopMaintenance(waitForWorker);
    ReleaseTimerResolution();
    CloseMainThreadHandle();
    CloseLog(writeCloseMessage);
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

    if (!obse)
    {
        return false;
    }

    if (obse && obse->isEditor)
    {
        return false;
    }

    return true;
}

extern "C" __declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse)
{
    if (!obse || obse->isEditor)
    {
        return false;
    }

    ApplyRuntimeTuning(obse);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        ShutdownRuntime(reserved == nullptr, false);
    }

    return TRUE;
}
