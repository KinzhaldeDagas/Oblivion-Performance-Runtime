# Oblivion Performance Runtime

An OBSE plugin for smoothing out Oblivion by giving the game process better treatment from Windows.

This plugin does not change gameplay records, scripts, leveled lists, INI settings, saves, or Oblivion's executable code. It loads with OBSE, applies a set of process-level runtime tweaks, writes a small log, and then gets out of the way.

## What This Mod Does

Oblivion is old enough that modern Windows can sometimes treat it like a background-era 32-bit app instead of a game that needs steady frame pacing. This plugin tries to reduce that problem by applying Windows-side tuning while the game is running.

It can:

- Raise `Oblivion.exe` to above-normal process priority.
- Request a high precision system timer for smoother frame pacing and lower input latency.
- Keep the game on a stable CPU affinity mask.
- Set a preferred processor for the main game thread.
- Optionally hard-pin the main thread for anti-core-hop testing.
- Raise the main OBSE loading thread to highest thread priority.
- Enable the Low Fragmentation Heap on process heaps.
- Periodically trim the working set during long sessions.
- Request better foreground I/O priority for loading.
- Disable Windows 10/11 power throttling when the OS supports it.
- Report Large Address Aware, WOW64, and DEP state in the log.

## Requirements

- Oblivion 1.2.0416.
- OBSE or xOBSE.
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

Start the game through OBSE/xOBSE as usual.

The plugin writes its log here:

```text
Data\OBSE\Plugins\OblivionPerformanceRuntime.log
```

## Configuration

Default settings:

```ini
[Tuning]
AboveNormalPriority=1
HighPrecisionTimer=1
ProcessAffinity=1
ProcessAffinityMask=0
MainThreadHighestPriority=1
MainThreadIdealProcessor=1
MainThreadHardPin=0
LowFragmentationHeap=1
WorkingSetPurge=1
MaintenanceIntervalSeconds=300
IoPriorityBoost=1
PowerThrottlingDisable=1
```

`ProcessAffinityMask=0` means the plugin uses the current CPU mask allowed by Windows. You can set a decimal or hex mask such as `0xFE` if you want to exclude CPU 0.

`MainThreadHardPin=0` is the recommended default. Set it to `1` only if you want to test a strict anti-core-hop setup. Hard pinning can help some systems and hurt others.

`MaintenanceIntervalSeconds=300` trims the working set every five minutes. Lower values are clamped to a minimum of 30 seconds.

## Windows 7 Notes

The DLL is built for Windows 7 compatibility with a Windows 7 subsystem target and static MSVC runtime.

Windows 10/11-only features are loaded dynamically. On Windows 7, power throttling disable is simply skipped and logged as unavailable.

## Compatibility

This plugin does not register script commands, edit saves, patch Oblivion instructions, or require an opcode range.

It should be compatible with normal OBSE plugin setups. Mods that also alter process priority, affinity, timer resolution, working set trimming, or power policy may overlap with this plugin.

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
