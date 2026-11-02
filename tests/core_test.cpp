/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Yeatol <vim@vimer.org>
 */

#include "../top.cpp"
#include "../tui.cpp"

#include <thread>
#include <atomic>
#include <cstdlib>
#include <sys/thr.h>

static void check(bool passed, const std::string& message)
{
    if (!passed)
    {
        fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

static process_t record(int pid, lwpid_t tid, bool kernel = false)
{
    process_t result{};
    result.pid = pid;
    result.tid = tid;
    result.kernel_process = kernel;
    result.username = "test";
    result.threads = 2;
    result.nice = "0";
    result.state = "RUN";
    result.command = "process";
    result.user_cpu_percent = 1.0;
    result.system_cpu_percent = 2.0;
    return result;
}

static void test_layout()
{
    snapshot_t snapshot;
    snapshot.processes = {record(10, 100), record(20, 200, true)};
    snapshot.threads = {record(10, 100), record(10, 101), record(20, 200, true), record(20, 201, true)};
    snapshot.threads[0].command = "representative";
    snapshot.threads[1].command = "worker";
    snapshot.threads[1].nivcsw = 12;
    snapshot.threads[1].nvcsw = 345;
    snapshot.threads[1].nivcsw_per_second = 2.6;
    snapshot.threads[1].nvcsw_per_second = 50.0;
    snapshot.processes[0].jid = 69;
    snapshot.threads[0].jid = 69;
    snapshot.threads[1].jid = 69;
    auto normal = lines(snapshot, -1);
    check(normal.size() == 3 && normal[0].text.find("COMMAND") != std::string::npos, "process-only layout");
    check(normal[1].text.find("process") != std::string::npos && normal[1].text.find("nivcsw") == std::string::npos,
          "process name and counters in normal mode");
    check(normal[1].text.find("[jail:69] process") != std::string::npos, "process list retains jail prefix");
    auto detail = lines(snapshot, 10);
    auto deferred = lines(snapshot, 10, false);
    check(deferred.size() == detail.size() && deferred[0].text == detail[0].text,
          "deferred layout preserves header and row count");
    for (size_t i = 1; i < deferred.size(); ++i)
    {
        const auto& row = deferred[i];
        check(row.text.empty() && row.pid == detail[i].pid && row.tid == detail[i].tid &&
              process_line(*row.record, row.thread, *row.command_source, true, row.affinity_width) == detail[i].text,
              "deferred rows retain identity and produce identical text on demand");
    }
    check(detail[0].text.find(" CPU ") == std::string::npos &&
          detail[0].text.find("UCPU") != std::string::npos && detail[0].text.find("SCPU") != std::string::npos,
          "user detail shows split CPU columns without total CPU");
    check(detail.size() == 3 && detail[1].tid == 100 && detail[2].tid == 101, "representative TID is not duplicated");
    check(!detail[1].thread && detail[2].thread, "parent and thread selection identities");
    size_t affinity = detail[0].text.find("  A ");
    check(affinity != std::string::npos, "affinity header has minimum width three");
    size_t cpu = affinity + 4;
    size_t name = detail[0].text.find("THREAD");
    check(detail[0].text.substr(cpu, 4) == "  C ", "A precedes C");
    for (size_t i = 1; i < detail.size(); ++i)
    {
        check(detail[i].text.find("[jail:") == std::string::npos, "thread list omits jail prefix on all rows");
        check(detail[i].text.substr(affinity, 4) == "  - ", "unrestricted affinity is right aligned");
        check(detail[i].text.substr(cpu, 4) == "  0 ", "CPU is right aligned");
    }
    check(detail[1].text.substr(name) == "representative", "parent uses representative name");
    check(detail[2].text.substr(name) == "worker", "thread name has no appended counters");
    size_t switches = detail[0].text.find("NIVCSW ");
    check(switches == detail[0].text.find("STATE") + 7, "switch columns immediately follow STATE");
    check(switches < affinity && detail[0].text.substr(switches, 20) == "NIVCSW    NVCSW     ", "two fixed-width switch columns precede A");
    check(detail[1].text.substr(switches, 20) == "0         0         ", "unavailable rates show only counts");
    check(detail[2].text.substr(switches, 20) == "12+3      345+50    ", "combined counts and rates are left aligned");
    snapshot.threads[1].nivcsw_per_second = 0.0;
    snapshot.threads[1].nvcsw_per_second = 0.4;
    detail = lines(snapshot, 10);
    check(detail[2].text.substr(switches, 20) == "12        345       ", "zero and rounded-zero suffixes are hidden");
    snapshot.threads[1].nvcsw = UINT64_MAX;
    snapshot.threads[1].nivcsw_per_second = 12345678.0;
    detail = lines(snapshot, 10);
    check(detail[2].text.substr(switches, 20) == "12+12M    18E       ", "counts and rates use compact units");
    check(detail[0].text == lines(snapshot_t{.processes = {record(10, 100)}, .threads = {record(10, 100)}}, 10)[0].text,
          "large counts and rates do not change column widths");
    check(switch_value(12345678, std::nullopt) == "12M", "cumulative values use compact units");
    check(switch_value(12, 123000.0) == "12+123K" &&
          switch_value(12, 999500.0) == "12+1M", "rate suffixes promote rounded unit boundaries");
    check(switch_value(123000000, 223000.0) == "123M+223K" &&
          switch_value(999499999, 999499.0) == "999M+999K", "combined values fit nine characters");
    check(switch_value(999500, std::nullopt) == "1M", "cumulative values promote rounded unit boundaries");
    check(detail[1].text.find("representative") == detail[0].text.find("THREAD") &&
          detail[2].text.find("worker") == detail[0].text.find("THREAD"), "large counts and rates preserve column alignment");
    snapshot.threads[1].affinity = "0,2,3,7";
    detail = lines(snapshot, 10);
    affinity = detail[0].text.find("      A ");
    check(affinity != std::string::npos, "affinity header expands for long masks");
    check(detail[1].text.substr(affinity, 8) == "      - " &&
          detail[2].text.substr(affinity, 8) == "0,2,3,7 ", "long affinity masks keep all rows aligned");
    auto kernel = lines(snapshot, 20);
    check(kernel[0].text.find(" CPU ") != std::string::npos, "kernel detail retains total CPU");
    check(normal[0].text.find(" CPU ") != std::string::npos, "process list retains total CPU");
    check(kernel[0].text.find("UCPU") == std::string::npos && kernel[0].text.find(" A ") == std::string::npos,
          "kernel layout omits user counters and affinity");
    check(kernel[1].text.find("process") == kernel[0].text.find("THREAD"), "kernel columns aligned");
    check(lines(snapshot, 99).size() == 1, "disappeared process leaves only header");
    snapshot.threads.erase(snapshot.threads.begin());
    detail = lines(snapshot, 10);
    check(detail.size() == 3 && detail[1].text.ends_with("process"),
          "missing representative falls back to process name");
    check(memory(9999) == "9999K" && memory(10240) == "10M", "memory unit boundary");
    check(runtime(59500000) == "1:00" && runtime(6000000000) == "1h", "runtime rounding and hour format");
}

static void test_selection()
{
    snapshot_t snapshot;
    snapshot.processes = {record(10, 100)};
    for (int i = 0; i < 12; ++i)
    {
        snapshot.threads.push_back(record(10, 100 + i));
    }
    auto rows = lines(snapshot, 10);
    int pid = -1;
    lwpid_t tid = -1;
    int scroll = 0;
    move_selection(rows, 1, 3, pid, tid, scroll);
    check(pid == 10 && tid == -1 && scroll == 0, "down without selection picks first visible row");
    move_selection(rows, -1, 3, pid, tid, scroll);
    check(tid == -1, "selection does not wrap at top");
    for (int i = 0; i < 20; ++i)
    {
        move_selection(rows, 1, 3, pid, tid, scroll);
    }
    check(tid == 111 && scroll == 9, "selection scrolls and stops at last thread");
    pid = -1;
    tid = -1;
    move_selection(rows, -1, 3, pid, tid, scroll);
    check(pid == 10 && tid == 111, "up without selection picks last visible row");
    pid = 99;
    tid = -1;
    move_selection(rows, 1, 3, pid, tid, scroll);
    check(pid == 10 && tid == 109, "stale selection picks first visible row");
    move_selection(rows, 1, 0, pid, tid, scroll);
    check(tid == 109, "zero-height table preserves selection");
    move_selection(lines(snapshot, 99), 1, 3, pid, tid, scroll);
    check(tid == 109, "empty table does not index a data row");
}

static void test_cpu()
{
    check(counter_rate(25, 10, 2000000) == 7.5, "switch rate uses actual elapsed seconds");
    check(counter_rate(10, 10, 500000) == 0.0, "unchanged switch counter gives zero rate");
    check(!counter_rate(9, 10, 1000000).has_value(), "decreased switch counter has no rate");
    check(!counter_rate(25, 10, 0).has_value(), "zero sample interval has no rate");
    std::array<long, CPUSTATES> zero{};
    check(cpu_percentages(zero, zero) == std::array<int, CPUSTATES>{}, "no CPU ticks means zero usage");
    std::array<long, CPUSTATES> ticks{};
    ticks[CP_USER] = 100;
    ticks[CP_SYS] = 200;
    ticks[CP_IDLE] = 700;
    auto percentages = cpu_percentages(ticks, zero);
    check(percentages[CP_USER] == 100 && percentages[CP_SYS] == 200 && percentages[CP_IDLE] == 700,
          "CPU proportions use tenths of a percent");
    check(text("a\033\xff", 3) == "a??", "sampler sanitizes terminal control and non-ASCII bytes");
}

static void test_live_sampling()
{
    std::atomic<bool> stop = false;
    std::thread worker([&] {
        while (!stop.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    long tid = 0;
    check(thr_self(&tid) == 0, "read test thread ID");
    cpuset_t original;
    check(cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, tid, sizeof(original), &original) == 0, "read own affinity");
    cpuset_t restricted;
    CPU_ZERO(&restricted);
    int first_cpu = 0;
    while (first_cpu < CPU_SETSIZE && !CPU_ISSET(first_cpu, &original))
    {
        ++first_cpu;
    }
    check(first_cpu < CPU_SETSIZE, "own CPU set is not empty");
    CPU_SET(first_cpu, &restricted);
    check(cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, tid, sizeof(restricted), &restricted) == 0, "restrict own test thread");
    cpuset_t root;
    check(cpuset_getaffinity(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1, sizeof(root), &root) == 0, "read root CPU set");
    top_t top;
    std::string error;
    check(top_open(top, error), error);
    snapshot_t snapshot;
    for (int sample = 0; sample < 20; ++sample)
    {
        check(top_sample(top, snapshot, error, getpid()), error);
        check(snapshot.processes.size() == 1 && snapshot.processes[0].pid == getpid(), "detail samples only target process");
        check(std::ranges::all_of(snapshot.threads, [](const process_t& p) { return p.pid == getpid(); }),
              "detail samples only target threads");
        check(std::ranges::is_sorted(snapshot.processes, compare_processes), "live processes sorted by PID");
        check(std::ranges::is_sorted(snapshot.threads, compare_threads), "live threads sorted by PID/TID");
        auto own = std::ranges::find(snapshot.threads, tid, &process_t::tid);
        check(own != snapshot.threads.end() && own->pid == getpid(), "own thread appears in sampler");
        if (sample == 0)
        {
            check(!own->nvcsw_per_second.has_value() && !own->nivcsw_per_second.has_value(), "first sample has no switch rates");
        }
        else
        {
            check(own->nvcsw_per_second.has_value() && *own->nvcsw_per_second >= 0 &&
                  own->nivcsw_per_second.has_value() && *own->nivcsw_per_second >= 0, "live switch rates available after first interval");
        }
        check(own->affinity == (CPU_EQUAL(&root, &restricted) ? "" : std::to_string(first_cpu)), "live affinity matches cpuset");
        check(std::ranges::count(snapshot.threads, getpid(), &process_t::pid) >= 2, "live worker thread appears");
        check(!snapshot.cpus.empty(), "live CPU sample is not empty");
        for (const auto& thread : snapshot.threads)
        {
            check(thread.cpu_percent >= 0 && thread.cpu_percent <= 100, "thread CPU percentage within one CPU");
        }
        for (int percent : snapshot.cpu.percent_tenths)
        {
            check(percent >= 0 && percent <= 1000, "whole-system CPU percentage in range");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto previous_time = top.sampled_at;
    auto process_count = snapshot.processes.size();
    int max_cpus = top.max_cpus;
    top.max_cpus = 0;
    check(!top_sample(top, snapshot, error, getpid()) && !error.empty(), "malformed CPU buffer fails explicitly");
    check(top.sampled_at == previous_time && snapshot.processes.size() == process_count, "failed sample preserves committed state");
    top.max_cpus = max_cpus;
    error.clear();
    check(top_sample(top, snapshot, error, getpid()), error);
    check(top_sample(top, snapshot, error), error);
    check(snapshot.threads.empty() && top.thread_previous.empty(), "process view skips thread sampling and history");
    check(snapshot.processes.size() > 1, "process view samples all visible processes");
    check(top_sample(top, snapshot, error, getpid()), error);
    auto own = std::ranges::find(snapshot.threads, tid, &process_t::tid);
    check(own != snapshot.threads.end() && !own->nvcsw_per_second.has_value(), "switching views resets thread rate baseline");
    check(top_sample(top, snapshot, error, 999999), error);
    check(snapshot.processes.empty() && snapshot.threads.empty(), "missing detail target produces empty snapshot");
    top_close(top);
    check(top.kernel == nullptr && top.previous.empty() && top.thread_previous.empty(), "sampler cleanup releases state");
    top_close(top);
    check(cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, tid, sizeof(original), &original) == 0, "restore own test affinity");
    stop = true;
    worker.join();
}

int main()
{
    test_layout();
    test_selection();
    test_cpu();
    test_live_sampling();
    fprintf(stderr, "core tests passed\n");
}
