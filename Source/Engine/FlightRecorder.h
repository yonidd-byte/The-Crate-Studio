#pragma once

// Step 74 (The Flight Recorder) directive: a production-grade, always-on
// "black box" — a fixed-capacity, pre-allocated ring buffer that both
// processes (Host and every CrateSandbox.exe) write high-frequency
// diagnostic breadcrumbs into continuously, at negligible cost, and which
// is only ever READ (flushed to a physical .crashlog file) at one of
// three moments: an unhandled exception, a Watchdog-forced kill, or
// normal graceful shutdown. This exists so the NEXT deadlock/crash
// doesn't require walking the user through Task Manager dump captures —
// the log already has the exact sequence of HWND reparenting events, IPC
// traffic, and Watchdog pings leading up to the moment things went wrong,
// written by whichever thread was actually doing something at the time.
//
// Hard requirements this class exists to satisfy, and how:
//   - Zero allocation during normal operation: the ONE heap allocation
//     (the record array itself) happens once, at first getInstance() call
//     (which happens well before any plugin is ever loaded — see the
//     installation call sites in both Main.cpp files) — log()/logf() only
//     ever write into already-allocated slots.
//   - Zero locks: writeIndex is a single std::atomic<uint64_t>, claimed
//     via fetch_add (relaxed — ordering between independent log calls
//     doesn't matter, only that no two threads ever claim the SAME slot,
//     which fetch_add already guarantees). No mutex anywhere in the write
//     path, so this can never itself become a source of contention or
//     deadlock on the very threads it's trying to help diagnose.
//   - Safe to flush from an unhandled-exception filter: flushToDisk()
//     uses ONLY raw Win32 file I/O (CreateFileW/WriteFile/CloseHandle)
//     and stack-local snprintf formatting — no JUCE, no C++ exceptions,
//     nothing that assumes the CRT/heap is in a fully healthy state,
//     since by the time this runs that may no longer be true.
//
// Deliberately NOT thread-safe against a flush happening WHILE writers are
// still actively racing it — by design, a flush only ever happens at a
// moment things are already ending (a crash, a kill, a shutdown), so a
// handful of the very last entries being slightly stale or overwritten
// concurrently is an acceptable, bounded imprecision for a diagnostic
// tool, not a correctness bug worth paying a lock for on every single
// write.

#include <windows.h>
#include <atomic>
#include <memory>
#include <cstdio>
#include <cstdarg>
#include <cstring>

class FlightRecorder
{
public:
    static FlightRecorder& getInstance()
    {
        static FlightRecorder instance;
        return instance;
    }

    // Zero-allocation, lock-free. Safe from any thread, any context,
    // including inside an unhandled-exception filter. category is a short
    // fixed label ("AIRLOCK", "IPC", "WATCHDOG", ...); message is
    // truncated to fit, never allocated.
    void log (const char* category, const char* message) noexcept
    {
        const uint64_t slot = writeIndex.fetch_add (1, std::memory_order_relaxed) & indexMask;
        FlightRecord& rec = records[slot];

        LARGE_INTEGER qpc;
        QueryPerformanceCounter (&qpc);
        rec.qpcTimestamp = qpc.QuadPart;
        rec.threadId = GetCurrentThreadId();

        strncpy_s (rec.category, category, _TRUNCATE);
        strncpy_s (rec.message, message, _TRUNCATE);
    }

    // Convenience formatter — vsnprintf writes into a fixed STACK buffer
    // only, never the heap, so this stays as zero-allocation as log()
    // itself.
    void logf (const char* category, const char* fmt, ...) noexcept
    {
        char buffer[sizeof (FlightRecord::message)];

        va_list args;
        va_start (args, fmt);
        vsnprintf (buffer, sizeof (buffer), fmt, args);
        va_end (args);

        log (category, buffer);
    }

    // Called from exactly three places per process — see each call site's
    // own doc comment: the unhandled-exception filter installed by
    // installCrashHandler() below, a Watchdog-forced kill (CrateSandboxBridge's
    // four restart/guillotine sites), and normal graceful shutdown
    // (TheCrateStudioApplication::shutdown() / CrateSandboxApplication::shutdown()).
    // Raw Win32 file I/O only — see this class's own doc comment for why.
    void flushToDisk (const char* reasonForFlush) noexcept
    {
        wchar_t path[MAX_PATH];
        const DWORD len = GetTempPathW (MAX_PATH, path);

        if (len == 0 || len >= MAX_PATH)
            return;

        wcsncat_s (path, L"CrateFlightRecorder.crashlog", _TRUNCATE);

        HANDLE file = CreateFileW (path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file == INVALID_HANDLE_VALUE)
            return;

        char header[256];
        const int headerLen = snprintf (header, sizeof (header),
                                         "=== Crate Flight Recorder flush -- reason: %s (pid=%lu) ===\r\n",
                                         reasonForFlush, GetCurrentProcessId());
        DWORD written = 0;
        WriteFile (file, header, (DWORD) headerLen, &written, nullptr);

        // Chronological order, oldest-surviving-entry first. Before the
        // ring has ever wrapped (totalWrites <= capacity), that's simply
        // slot 0 onward; once wrapped, the oldest surviving entry is
        // whatever the NEXT write would overwrite.
        const uint64_t totalWrites = writeIndex.load (std::memory_order_relaxed);
        const bool hasWrapped = totalWrites > capacity;
        const uint64_t startSlot = hasWrapped ? (totalWrites & indexMask) : 0;
        const uint64_t count = hasWrapped ? capacity : totalWrites;

        for (uint64_t i = 0; i < count; ++i)
        {
            const FlightRecord& rec = records[(startSlot + i) & indexMask];

            char line[256];
            const int lineLen = snprintf (line, sizeof (line), "[qpc=%lld tid=%lu] %-16s %s\r\n",
                                           rec.qpcTimestamp, rec.threadId, rec.category, rec.message);
            WriteFile (file, line, (DWORD) lineLen, &written, nullptr);
        }

        CloseHandle (file);
    }

private:
    FlightRecorder() : records (new FlightRecord[capacity]) {}

    struct FlightRecord
    {
        long long qpcTimestamp = 0;
        unsigned long threadId = 0;
        char category[16] = {};
        char message[144] = {};
    };

    static constexpr uint64_t capacity = 16384; // power of 2 -- lets slot claims use a mask instead of a modulo
    static constexpr uint64_t indexMask = capacity - 1;

    std::atomic<uint64_t> writeIndex { 0 };
    std::unique_ptr<FlightRecord[]> records;

    FlightRecorder (const FlightRecorder&) = delete;
    FlightRecorder& operator= (const FlightRecorder&) = delete;
};

// Step 74 directive, Task 1a: catches anything that escapes every LOCAL
// SEH __except wrapper already in this codebase (sehProcessBlock(),
// sehGetStateInformation(), etc. — those catch and recover in place, so a
// genuinely fatal, unexpected fault is the ONLY thing that ever reaches
// this) — flushes the flight recorder, then returns
// EXCEPTION_CONTINUE_SEARCH so the OS's own default handling (WER,
// LocalDumps, an attached debugger) still runs exactly as it would
// without this filter installed. This never SUPPRESSES a crash, only
// guarantees the flight recorder survives it.
inline LONG WINAPI crateFlightRecorderExceptionFilter (EXCEPTION_POINTERS*)
{
    FlightRecorder::getInstance().flushToDisk ("UNHANDLED_EXCEPTION");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Called once, at the very top of each process's own entry point (see
// both Main.cpp files) — installing this is itself allocation-free and
// cheap, safe to do unconditionally before anything else runs.
inline void installFlightRecorderCrashHandler()
{
    SetUnhandledExceptionFilter (&crateFlightRecorderExceptionFilter);
}

#define CRATE_FR_LOG(category, message)       ::FlightRecorder::getInstance().log (category, message)
#define CRATE_FR_LOGF(category, fmt, ...)      ::FlightRecorder::getInstance().logf (category, fmt, __VA_ARGS__)
