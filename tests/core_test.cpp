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

static void cpu_fixture(std::vector<cpu_t>& cpus, int count)
{
    cpus.resize(static_cast<size_t>(count));
    for (int id = 0; id < count; ++id)
    {
        cpus[id] = {};
        cpus[id].id = id;
        cpus[id].percent_tenths[CP_USER] = 1000;
    }
}

static void test_cpu()
{
    std::vector<cpu_t> simulated;
    check(cpu_prefix_rows(12, 120, 80) == 6 && cpu_prefix_rows(12, 80, 24) == 8,
          "CPU prefix reserves top and bottom blank lines");
    check(cpu_prefix_rows(12, 120, 5) == 0 && cpu_prefix_rows(12, 120, 4) == 0 &&
          cpu_prefix_rows(0, 120, 80) == 0, "short terminal preserves table header and data");
    check(cpu_columns(24, 120, 16) == 4 && cpu_prefix_rows(24, 120, 16) == 8,
          "CPU layout adds columns to fit half the screen without gaps");
    check(cpu_columns(12, 120, 8) == 6 && cpu_prefix_rows(12, 120, 8) == 4,
          "short CPU area expands columns before clipping");
    check(cpu_columns(128, 120, 24) == 16 && cpu_prefix_rows(128, 120, 24) == 10,
          "dense CPU layout fits all cores within half the screen");
    cpu_fixture(simulated, 128);
    auto dense = cpu_grid_line(simulated, 7, 16, 120);
    check(dense.text.size() == 112 && dense.text.substr(105).starts_with("127["),
          "compact grid displays the final CPU without clipping");
    check(cpu_columns(127, 120, 24) == 13,
          "showing all CPUs takes priority over avoiding grid gaps");
    cpu_fixture(simulated, 256);
    int max_columns = cpu_columns(256, 120, 40);
    auto max_grid = cpu_grid_line(simulated, 15, max_columns, 120);
    check(max_columns == 16 && max_grid.text.find("255[") != std::string::npos &&
          max_grid.text.size() <= 120 && cpu_prefix_rows(256, 120, 40) <= 20,
          "256 CPUs retain three-character IDs and equal-width compact bars");
    for (int height = 1; height <= 81; ++height)
    {
        for (int width : {2, 15, 80, 120})
        {
            check(cpu_prefix_rows(128, width, height) <= height / 2,
                  "CPU area including blank lines never exceeds half the screen");
        }
    }
    cpu_t bar_cpu;
    bar_cpu.id = 7;
    bar_cpu.percent_tenths[CP_USER] = 200;
    bar_cpu.percent_tenths[CP_NICE] = 100;
    bar_cpu.percent_tenths[CP_SYS] = 200;
    bar_cpu.percent_tenths[CP_INTR] = 100;
    auto bar = cpu_line(bar_cpu);
    check(bar.text == "  7[||||||||||]", "CPU ID is right aligned and idle cells are filled");
    check(bar.colors.size() == 5 && bar.colors[0].offset == 0 &&
          bar.colors[1].offset == 4 && bar.colors[2].offset == 7 &&
          bar.colors[3].offset == 9 && bar.colors[4].offset == 10 &&
          bar.colors[0].color == terminal_color_t::bright_black &&
          bar.colors[1].color == terminal_color_t::green &&
          bar.colors[2].color == terminal_color_t::red &&
          bar.colors[3].color == terminal_color_t::yellow &&
          bar.colors[4].color == terminal_color_t::bright_black, "CPU IDs and brackets are gray while usage retains its colors");
    bar_cpu.percent_tenths = {};
    auto idle = cpu_line(bar_cpu);
    check(idle.text == "  7[||||||||||]" && idle.colors.size() == 2 &&
          idle.colors.back().offset == 4 &&
          idle.colors.back().color == terminal_color_t::bright_black, "idle CPU bar is filled with gray pipes");
    bar_cpu.id = 123;
    bar_cpu.percent_tenths[CP_USER] = 334;
    bar_cpu.percent_tenths[CP_SYS] = 334;
    bar_cpu.percent_tenths[CP_INTR] = 333;
    bar = cpu_line(bar_cpu);
    check(bar.text == "123[||||||||||]" && bar.colors.back().offset == 14,
          "rounded full CPU usage does not overflow ten cells");
    bar_cpu.percent_tenths = {};
    bar_cpu.percent_tenths[CP_SYS] = 49;
    check(cpu_line(bar_cpu).colors.size() == 2, "sub-half-cell usage rounds down to gray idle cells");
    bar_cpu.percent_tenths[CP_SYS] = 50;
    bar = cpu_line(bar_cpu);
    check(bar.text == "123[||||||||||]" && bar.colors[1].color == terminal_color_t::red &&
          bar.colors.back().offset == 5 && bar.colors.back().color == terminal_color_t::bright_black,
          "half-cell system usage rounds up with no empty user span");
    std::vector<cpu_t> cpus(8);
    for (size_t i = 0; i < cpus.size(); ++i)
    {
        cpus[i].id = static_cast<int>(i);
        cpus[i].percent_tenths[CP_USER] = 1000;
    }
    check(cpu_columns(8, 120) == 2 && cpu_columns(8, 80) == 2 &&
          cpu_columns(8, 240) == 2 && cpu_columns(12, 120) == 3 &&
          cpu_columns(16, 120) == 4 &&
          cpu_columns(2, 120) == 2 && cpu_columns(1, 120) == 1 &&
          cpu_columns(8, 11) == 1, "CPU columns adapt to screen width and core count");
    check(cpu_columns(10, 120) == 2 && cpu_columns(18, 120) == 3 &&
          cpu_columns(20, 120) == 4 && cpu_columns(25, 120) == 2 &&
          cpu_columns(25, 150) == 5 && cpu_columns(12, 80) == 2 &&
          cpu_columns(13, 120) == 2, "CPU grid prefers divisors and minimizes unavoidable empty slots");
    for (int count = 2; count <= 128; ++count)
    {
        int columns = cpu_columns(count, 120);
        int rows = (count + columns - 1) / columns;
        check(rows * columns - count <= 1, "CPU grid leaves at most one empty slot");
    }
    for (int width : {16, 40, 79, 80, 120, 121})
    {
        int columns = cpu_columns(8, width);
        int rows = (8 + columns - 1) / columns;
        for (int row = 0; row < rows; ++row)
        {
            auto grid = cpu_grid_line(cpus, row, columns, width);
            check(grid.text.size() <= static_cast<size_t>(width), "CPU grid fits screen width");
            size_t previous = 0;
            for (auto span : grid.colors)
            {
                check(span.offset >= previous && span.offset < grid.text.size(), "grid color offsets remain ordered and valid");
                previous = span.offset;
            }
        }
        auto grid = cpu_grid_line(cpus, 0, columns, width);
        size_t bar_width = std::string::npos;
        size_t position = 0;
        for (int column = 0; column < columns; ++column)
        {
            size_t open = grid.text.find('[', position);
            size_t close = grid.text.find(']', open);
            check(open != std::string::npos && close != std::string::npos, "each CPU bar has brackets");
            if (column == 0)
            {
                bar_width = close - open;
            }
            check(close - open == bar_width, "all CPU bars have equal widths including final column");
            if (column + 1 < columns)
            {
                check(grid.text[close + 4] == '[', "next three-character CPU ID directly follows closing bracket");
            }
            position = close + 1;
        }
    }
    auto grid = cpu_grid_line(cpus, 3, cpu_columns(8, 120), 120);
    for (int screen_width : {40, 79, 80, 120, 121})
    {
        int cpu_width = screen_width - 1;
        auto padded = cpu_grid_line(cpus, 0, cpu_columns(8, cpu_width, 24), cpu_width);
        check(padded.text.size() <= static_cast<size_t>(screen_width - 1),
              "CPU grid reserves at least one blank cell at screen right");
    }
    check(cpu_prefix_rows(8, 120, 80) == 6 && grid.text.starts_with("  3[") &&
          grid.text.substr(60, 4) == "  7[",
          "eight CPUs form four rows and two columns ordered vertically");
    cpu_fixture(cpus, 12);
    grid = cpu_grid_line(cpus, 0, cpu_columns(12, 120), 120);
    check(grid.text.starts_with("  0[") && grid.text.substr(40, 4) == "  4[" &&
          grid.text.substr(80, 4) == "  8[",
          "twelve CPUs form four rows and three equal-height columns");
    cpu_fixture(cpus, 10);
    grid = cpu_grid_line(cpus, 0, 4, 120);
    check(grid.text.starts_with("  0[") && grid.text.substr(30, 4) == "  3[" &&
          grid.text.substr(60, 4) == "  6[" && grid.text.substr(90, 4) == "  8[",
          "uneven CPU counts are balanced across columns");
    grid = cpu_grid_line(cpus, 2, 4, 120);
    check(grid.text.starts_with("  2[") && grid.text.substr(30, 4) == "  5[" &&
          grid.text.size() == 60, "only longer columns contain a final CPU");
    cpu_t full_cpu{};
    full_cpu.id = 0;
    full_cpu.percent_tenths[CP_USER] = 1000;
    check(cpu_line(full_cpu, 20).text == "  0[||||||||||||||||||||]",
          "CPU proportions scale to variable bar width");
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
