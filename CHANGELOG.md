# Changelog

## 1.0.0

Initial release.

### Added

- OBSE plugin exports: `OBSEPlugin_Query` and `OBSEPlugin_Load`.
- Above-normal process priority for `Oblivion.exe`.
- Highest-priority hint for the main OBSE loading thread.
- High precision timer request using NT timer resolution plus `timeBeginPeriod(1)`.
- Process affinity handling with configurable `ProcessAffinityMask`.
- Main-thread ideal processor hint.
- Optional main-thread hard pinning through `MainThreadHardPin=1`.
- Low Fragmentation Heap enablement for process heaps.
- Periodic working set purge for long play sessions.
- Process I/O priority hint with fallback when Windows rejects high priority.
- Windows 10/11 power throttling disable when supported.
- Large Address Aware, WOW64, and DEP diagnostic logging.
- OBSE messaging listener for graceful shutdown on game exit.
- Windows 7-compatible build target and static MSVC runtime.
- Player-editable INI configuration.
- Log file written beside the plugin DLL.

### Notes

- The plugin does not patch `Oblivion.exe`.
- The plugin does not make the executable Large Address Aware. A separate 4GB/LAA patch is still required for expanded address space.
- Power throttling disable is skipped on Windows 7 because that API does not exist there.
