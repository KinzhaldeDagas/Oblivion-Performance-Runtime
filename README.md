# Oblivion Performance Runtime

An xOBSE plugin providing configurable Windows runtime tuning for Oblivion. Performance effects depend on the system and mod setup; frame-time improvements have not been established by this repository.

This plugin does not change gameplay records, scripts, leveled lists, game INI settings, saves, or Oblivion's executable code. It loads with xOBSE, waits for game initialization to apply the enabled Windows settings, and starts maintenance only when periodic work is enabled.

## What This Mod Does

The plugin exposes Windows scheduling, timer, and memory-policy controls. These controls are experiments to measure on your own machine, rather than guaranteed engine optimizations.

It can:

- Raise `Oblivion.exe` to above-normal process priority.
- Request a finer timer resolution, which can increase power use.
- Apply an explicit process CPU affinity mask.
- Optionally set a preferred processor for the game main thread.
- Optionally hard-pin the main thread for anti-core-hop testing.
- Raise the game main thread to highest thread priority after xOBSE identifies it.
- Request Low Fragmentation Heap mode on the Windows default process heap once at startup.
- Optionally trim the working set during long sessions (disabled by default).
- Request better foreground I/O priority for loading.
- Disable Windows 10/11 power throttling when the OS supports it.
- Report Large Address Aware, WOW64, and DEP state in the log.

## Requirements

- Oblivion 1.2.0416.
- xOBSE 22.10 or newer. Classic OBSE is unsupported.
- Windows 7 or newer.
- A 32-bit compatible Oblivion install.

For 4GB memory support, you still need a Large Address Aware patched `Oblivion.exe`. This plugin manages runtime behavior, but it cannot make an already-running executable see more address space.

## Installation

Copy these files into:

```text
Data\OBSE\Plugins\
```

Files:

```text
OblivionPerformanceRuntime.dll
OblivionPerformanceRuntime.ini
```

Start the game through xOBSE as usual.

The plugin writes its log here:

```text
Data\OBSE\Plugins\OblivionPerformanceRuntime.log
```

## Configuration

Built-in defaults and recommended settings (explicit INI values override these):

```ini
[Tuning]
AboveNormalPriority=1
HighPrecisionTimer=1
ProcessAffinity=1
ProcessAffinityMask=0
MainThreadHighestPriority=1
MainThreadIdealProcessor=0
MainThreadHardPin=0
LowFragmentationHeap=1
WorkingSetPurge=0
MaintenanceIntervalSeconds=300
IoPriorityBoost=1
PowerThrottlingDisable=1
```

`ProcessAffinityMask=0` preserves the current process mask without calling the affinity setter. You can set a decimal or hex mask such as `0xFE` to exclude CPU 0. Decimal numbers with leading zeroes remain decimal. Negative values, trailing garbage, truncated values, and masks exceeding 32 bits are rejected. A mask containing unavailable CPUs leaves the process mask unchanged. Explicit masks can override an existing process affinity policy; avoid conflicting settings in other tools.

`ProcessAffinity=0` disables only process-affinity changes. The thread CPU options still work using the current process mask. If applying a requested mask fails, thread options use the mask reported by Windows, never the rejected request.

`MainThreadIdealProcessor=0` is the default. Both thread CPU options select the highest numbered allowed logical processor; this is not a topology-aware choice of the fastest core. The `MainThread*` keys are applied when xOBSE sends `GameInitialized` from the main game loop. They never target the thread calling `OBSEPlugin_Load`, which can be a temporary loader thread. The plugin installs no game-address hooks.

`MainThreadHardPin=0` is the recommended default. Set it to `1` only if you want to test a strict anti-core-hop setup. Hard pinning can help some systems and hurt others.

`WorkingSetPurge=0` is the default. Enabling it removes resident working-set pages and can cause additional page faults and stutter. It does not free leaked allocations, defragment the game's custom allocator, or expand its address space.

`MaintenanceIntervalSeconds=300` sets the maintenance interval. Decimal values are clamped to 30 through 4,294,967 seconds before conversion to milliseconds. Malformed or negative values use 300 seconds. Maintenance reapplies enabled thread-priority/hard-pin settings and performs purging only when requested. It does not enumerate or modify private heaps. With purging, repeated priority, and hard pinning disabled, no worker is created.

`LowFragmentationHeap=1` affects only the Windows default heap. Oblivion's FormHeap/MemoryPool and private CRT heaps are distinct; this option does not tune those allocators.

`HighPrecisionTimer=1` requests finer Windows timer intervals; it does not improve the precision of QueryPerformanceCounter or change Oblivion's QPC-based I/O budget. Its frame-time effect still requires measurement.

The LAA diagnostic reads the running executable's PE header, independent of its filename or Unicode installation path. Unreadable or unrecognized headers are reported as unknown. Overlong plugin sibling paths fail safely instead of invoking the CRT invalid-parameter handler; normal log/INI paths must still fit within `MAX_PATH`.

The repository's bundled INI still explicitly enables `WorkingSetPurge` and `MainThreadIdealProcessor`. Set both to `0` to adopt the safer built-in defaults shown above. Existing INI values remain authoritative when upgrading.

## Windows 7 Notes

The DLL is built for Windows 7 compatibility with a Windows 7 subsystem target and static MSVC runtime.

Windows 10/11-only features are loaded dynamically. On Windows 7, power throttling disable is simply skipped and logged as unavailable.

## Compatibility

This plugin does not register script commands, edit saves, patch Oblivion instructions, or require an opcode range.

Only xOBSE on Oblivion 1.2.0416 is supported. Query and Load reject loader major versions below 22, other game versions, the editor, and missing required interface callbacks. Load also rejects missing or failed messaging registration before changing runtime settings. The public interface exposes no minor version, so all tuning waits for the GameInitialized callback introduced in xOBSE 22.10; earlier xOBSE 22 builds cannot activate tuning. Mods that also alter process priority, affinity, timer resolution, working set trimming, or power policy may overlap with this plugin.

The DLL stays loaded for the process lifetime because OBSE retains callback pointers. Hot unloading is unsupported. Normal OBSE exit messages signal and fully join the worker before closing resources and releasing timer requests. Abrupt termination, or an unavailable exit notification, leaves reclamation to Windows. `DllMain` performs no waiting or teardown, and static CRT thread notifications remain enabled.

## Building

Use Visual Studio 2022 with Desktop development with C++, a Windows SDK, and CMake 3.20 or newer:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

The DLL and distributable configuration are produced in `build/Release`. Review the bundled INI settings described above before installation.

## Verification of the 1.0.1 correction

The Win32 Release DLL built successfully. Three local regression checks passed, covering configuration and path boundaries, PE-header diagnostics, affinity failures, loader/main-thread separation, unsupported-loader rejection, worker lifetime, and DLL callback lifetime. The development test harness is not included in this source/documentation update. The DLL exports `OBSEPlugin_Load` and `OBSEPlugin_Query` and has an x86 PE32 header with subsystem version 6.01.

Read-only inspection of the open Oblivion IDA database confirmed the custom FormHeap allocator at `0x401AA0`, pool cleanup at `0x402740`, and the main-loop hook site at `0x40F19D`. Comparing the latter with xOBSE's loader and callback source established why thread tuning must wait for `GameInitialized`. Main-menu, in-game-menu, and console exit hook instructions also matched at `0x5B5A0D`, `0x5BDE60`, and `0x5077F2`. These addresses are verification references, not hooks installed by this plugin.

The IDA database's recorded input hash differed from the executable currently on disk, so these findings describe the open database rather than establishing byte-for-byte identity with that disk file. In-game performance, mod compatibility, and Windows 7 execution remain unverified.

## Uninstallation

Delete:

```text
Data\OBSE\Plugins\OblivionPerformanceRuntime.dll
Data\OBSE\Plugins\OblivionPerformanceRuntime.ini
Data\OBSE\Plugins\OblivionPerformanceRuntime.log
```

No save cleanup is required.

## Credits

Built as a lightweight OBSE runtime tuning plugin for Oblivion. Thanks to the OBSE/xOBSE projects for keeping native plugin support alive.
