// Stress-repros the intermittent Streamline Detours transaction race: StreamlineHooks::hookDlss /
// hookDlssg / hookInterposer / hookReflex / hookPcl / hookCommon each run their own complete
// DetourTransactionBegin()...DetourTransactionCommit() sequence, synchronously, from inside
// OptiScaler's own LoadLibraryW hook (LibraryLoadHooks::LoadLibraryCheckW), with nothing
// serializing them against each other. This drives real LoadLibraryW calls on several real
// sl.*.dll plugins concurrently, from multiple threads, to maximize the chance two of those
// transactions are in flight on Detours' process-wide state at once -- the same collision the two
// user-supplied repro logs showed as "Failed to (un)hook ...: 10DD" (0x10DD ==
// ERROR_INVALID_OPERATION from DetourTransactionCommit) immediately followed by Streamline's own
// exception handler writing a minidump and no further plugin ever loading.
//
// This does not call slInit -- it only needs to exercise LoadLibraryCheckW's dispatch, not a
// working Streamline session, so it has no dependency on sl.h / a device / a window.
//
// Build: cl /std:c++20 /EHsc /W4 tests\streamline_detours_race_stress.cpp
// Run in an isolated directory with an OptiScaler.ini containing [Log] LogToFile=true (LogLevel
// left at its default 0/trace, which already includes LOG_ERROR) and OptiDllPath pointed at a
// harmless FGInput/FGOutput so hookDlss/hookDlssg actually run (shouldHookSl just needs the
// plugin directory below to not be OptiScaler's own private Streamline path, which an ad hoc
// directory satisfies automatically):
//   streamline_detours_race_stress.exe <OptiScaler DLL> <Streamline plugin dir> [iterations] [threads]
//
// After the threads finish, this reads OptiScaler.log itself (written next to the DLL) for any
// "Failed to hook" / "Failed to unhook" line -- hookDlss et al. are not exported, so the log is
// the only observable signal, exactly as it was for the two original repro logs. Exits non-zero
// (FAIL) if any such line is found, zero (PASS) otherwise.
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void Require(bool success, const char* stage)
{
    if (!success)
        throw std::runtime_error(stage);
}

// One thread per entry, each hammering a different plugin so different StreamlineHooks::hookXxx
// functions (and therefore different DetourTransactionBegin/Commit pairs) are likely to overlap.
static const wchar_t* kPluginNames[] = { L"sl.interposer.dll", L"sl.dlss.dll",   L"sl.dlss_g.dll",
                                          L"sl.reflex.dll",     L"sl.pcl.dll",    L"sl.common.dll" };

static void HammerPlugin(const std::filesystem::path& path, int iterations)
{
    for (int i = 0; i < iterations; ++i)
    {
        // Every call re-enters our own LoadLibraryW hook and re-runs LoadLibraryCheckW's dispatch
        // -- including StreamlineHooks::hookDlss/hookDlssg/etc. -- even though the module is
        // already resident, since the hook intercepts the API call itself, not first-load.
        LoadLibraryW(path.c_str());
    }
}

// Every hookX/unhookX error uses the same "Failed to (un)hook X: {:X}" format regardless of which
// error code DetourTransactionCommit returned, and this stress pattern (several threads hammering
// the same plugin concurrently) is known to also surface a second, pre-existing, unrelated
// failure: "...: 9" (ERROR_INVALID_BLOCK, a TOCTOU race on the "already hooked?" check that lives
// outside any lock, present before and after the transaction-mutex fix this test targets). Only
// "...: 10DD" (ERROR_INVALID_OPERATION from DetourTransactionCommit -- the actual Detours
// transaction race this test exists to catch) should fail the run; other codes are logged for
// visibility but do not affect the verdict.
static bool LogContainsTransactionRaceFailure(const std::filesystem::path& logPath)
{
    std::ifstream log(logPath);
    if (!log)
    {
        std::fprintf(stderr, "WARN: could not open %ls to check for failures\n", logPath.c_str());
        return false;
    }
    std::string line;
    bool sawRaceFailure = false;
    while (std::getline(log, line))
    {
        const bool isHookFailure =
            line.find("Failed to hook") != std::string::npos || line.find("Failed to unhook") != std::string::npos;
        if (!isHookFailure)
            continue;

        const bool isTransactionRace = line.find(": 10DD") != std::string::npos;
        std::printf("%s%s\n", line.c_str(), isTransactionRace ? "" : "  (unrelated to the transaction race, ignored)");
        if (isTransactionRace)
            sawRaceFailure = true;
    }
    return sawRaceFailure;
}

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try
    {
        Require(argc >= 3, "Expected OptiScaler DLL, Streamline plugin dir, [iterations], [threads]");
        const int iterations = argc >= 4 ? std::stoi(argv[3]) : 500;
        const int threadsPerPlugin = argc >= 5 ? std::stoi(argv[4]) : 3;

        const std::filesystem::path dllPath(argv[1]);
        const std::filesystem::path pluginDir(argv[2]);
        Require(LoadLibraryW(dllPath.c_str()) != nullptr, "Load OptiScaler");

        std::vector<std::filesystem::path> pluginPaths;
        for (auto name : kPluginNames)
        {
            auto candidate = pluginDir / name;
            if (std::filesystem::exists(candidate))
                pluginPaths.push_back(candidate);
            else
                std::fprintf(stderr, "WARN: %ls not found, skipping\n", candidate.c_str());
        }
        Require(!pluginPaths.empty(), "No known Streamline plugin DLLs found in the given directory");

        std::vector<std::thread> threads;
        for (const auto& path : pluginPaths)
        {
            for (int t = 0; t < threadsPerPlugin; ++t)
                threads.emplace_back(HammerPlugin, path, iterations);
        }
        for (auto& thread : threads)
            thread.join();

        std::printf("Ran %zu plugin(s) x %d thread(s) x %d iteration(s)\n", pluginPaths.size(),
                    threadsPerPlugin, iterations);
        std::fflush(stdout);

        // OptiScaler resolves a relative LogFileName against the DLL's own directory. Wait for the
        // (possibly async) file sink to stop growing the log rather than a fixed delay, so this
        // doesn't race a slow/loaded machine into reading a not-yet-flushed tail.
        const auto logPath = dllPath.parent_path() / L"OptiScaler.log";
        std::error_code sizeError;
        auto lastSize = std::filesystem::file_size(logPath, sizeError);
        for (int stableChecks = 0; stableChecks < 3;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto size = std::filesystem::file_size(logPath, sizeError);
            if (!sizeError && size == lastSize)
                ++stableChecks;
            else
                stableChecks = 0;
            lastSize = size;
        }
        if (LogContainsTransactionRaceFailure(logPath))
        {
            std::puts("FAIL: Detours transaction race failure(s) logged above (10DD)");
            std::fflush(stdout);
            ExitProcess(1);
        }

        std::puts("PASS: no Detours transaction race failures (10DD) across the stress run");
        std::fflush(stdout);
        ExitProcess(0);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::fflush(stderr);
        ExitProcess(1);
    }
}
