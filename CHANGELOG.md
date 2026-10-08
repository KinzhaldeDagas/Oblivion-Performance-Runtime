# Changelog

## 1.0.1

OBSE plugin version integer: 4 (previous release reported 3).

- Restrict LFH requests to the Windows default heap at startup; remove private-heap enumeration.
- Join maintenance fully before releasing its resources, and retain resources on shutdown API failure.
- Keep the DLL resident for registered callbacks; remove blocking teardown from DllMain and retain static CRT thread notifications.
- Use `_beginthreadex` for the CRT-using worker and serialize log-file access.
- Reject invalid affinity masks and safely clamp maintenance intervals before multiplication.
- Apply thread affinity options independently of process-affinity changes; re-query the effective mask after requests.
- Default ideal-processor hints and working-set purging to off in code; skip workers with no periodic tasks. The bundled INI is unchanged and explicitly overrides those defaults; set both options to 0 to adopt them.
- Verify with local regression checks and read-only inspection of the open Oblivion IDA database. The development test harness is not included in this update.
- Require xOBSE 22.10+ on Oblivion 1.2.0416. Reject classic OBSE, other game versions and missing messaging; defer all tuning until GameInitialized instead of targeting a temporary loading thread.
- Synchronize callback thread capture with maintenance, and ignore non-OBSE messages.
- Guard sibling-path capacity before copying and inspect LAA on the running image with an explicit unknown state.
- Verify loader/main-thread separation, return-to-menu behavior, unsupported-loader rejection, and path/header edge cases.

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
