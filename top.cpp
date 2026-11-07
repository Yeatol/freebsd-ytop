/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Yeatol <vim@vimer.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "top.h"

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <libprocstat.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/proc.h>
#include <sys/time.h>
#include <sys/user.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/sysctl.h>
#include <sys/priority.h>
#include <sys/resource.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <format>
#include <string>
#include <vector>
#include <cstring>
#include <utility>
#include <iterator>
#include <limits.h>
#include <optional>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <unordered_map>

static std::string text(const char* value, size_t capacity)
{
    std::string result(value, strnlen(value, capacity));
    for (char& ch : result)
    {
        if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) >= 127)
        {
            ch = '?';
        }
    }
    return result;
}

static std::string state(const kinfo_proc& p)
{
    switch (p.ki_stat)
    {
        case SRUN:
            return p.ki_oncpu == NOCPU ? "RUN" : std::format("CPU{}", p.ki_oncpu);
        case SLOCK:
            if (p.ki_kiflag & KI_LOCKBLOCK)
            {
                return std::format("*{}", text(p.ki_lockname, 6));
            }
            [[fallthrough]];
        case SSLEEP:
            return text(p.ki_wmesg, 6);
        case SIDL:
            return "START";
        case SSTOP:
            return "STOP";
        case SZOMB:
            return "ZOMB";
        case SWAIT:
            return "WAIT";
        default:
            return std::format("?{}", p.ki_stat);
    }
}

static std::string nice(const kinfo_proc& p)
{
    bool        kernel   = (p.ki_flag & P_KPROC) != 0;
    int         priority = kernel ? p.ki_pri.pri_native : p.ki_pri.pri_user;
    std::string prefix   = kernel ? "k" : "";
    std::string suffix   = PRI_NEED_RR(p.ki_pri.pri_class) ? "" : "F";
    switch (PRI_BASE(p.ki_pri.pri_class))
    {
        case PRI_ITHD:
            return "-";
        case PRI_REALTIME:
            return std::format("{}r{}{}", prefix, priority - PRI_MIN_REALTIME, suffix);
        case PRI_IDLE:
            return std::format("{}i{}{}", prefix, priority - PRI_MIN_IDLE, suffix);
        case PRI_TIMESHARE:
            return kernel ? "-" : std::format("{}", p.ki_nice - NZERO);
        default:
            return "?";
    }
}

static bool read_integer(const char* name, int& value, std::string& error)
{
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) == -1)
    {
        error = std::format("{}: {}", name, std::strerror(errno));
        return false;
    }
    if (size != sizeof(value))
    {
        error = std::format("{}: unexpected sysctl value size", name);
        return false;
    }
    return true;
}

void top_close(top_t& top)
{
    if (top.procstat != nullptr)
    {
        procstat_close(top.procstat);
    }
    top = {};
}

bool top_open(top_t& top, std::string& error)
{
    top.page_size = sysconf(_SC_PAGESIZE);
    if (top.page_size <= 0)
    {
        error = "cannot determine page size";
        return false;
    }
    if (!read_integer("kern.fscale", top.fscale, error))
    {
        return false;
    }
    if (top.fscale <= 0)
    {
        error = "invalid kernel CPU scaling values";
        return false;
    }
    if (!read_integer("kern.smp.maxcpus", top.max_cpus, error))
    {
        return false;
    }
    if (top.max_cpus <= 0)
    {
        error = "kern.smp.maxcpus: invalid CPU count";
        return false;
    }
    top.procstat = procstat_open_sysctl();
    if (top.procstat == nullptr)
    {
        error = std::format("procstat_open_sysctl: {}", std::strerror(errno));
        return false;
    }
    return true;
}

static bool compare_processes(const process_t& a, const process_t& b)
{
    return a.pid < b.pid;
}

static uint64_t microseconds(const timeval& time)
{
    return static_cast<uint64_t>(time.tv_sec) * 1000000 + static_cast<uint64_t>(time.tv_usec);
}

struct sample_history_t
{
    std::unordered_map<int, previous_process_t> previous;
    std::chrono::steady_clock::time_point       sampled_at;
};

struct procstat_procs_deleter
{
    procstat* handle;

    void operator()(kinfo_proc* entries) const
    {
        if (entries != nullptr)
        {
            procstat_freeprocs(handle, entries);
        }
    }
};

struct procstat_files_deleter
{
    procstat* handle;

    void operator()(filestat_list* files) const
    {
        if (files != nullptr)
        {
            procstat_freefiles(handle, files);
        }
    }
};

static std::optional<double> counter_rate(uint64_t current, uint64_t previous, uint64_t interval_us)
{
    if (interval_us == 0 || current < previous)
    {
        return std::nullopt;
    }
    return static_cast<double>(current - previous) * 1000000.0 / interval_us;
}

static bool sample_records(top_t& top, snapshot_t& snapshot, bool threads, int detail_pid, const std::unordered_map<int, previous_process_t>& previous, std::chrono::steady_clock::time_point sampled_at, sample_history_t& next_history, std::string& error)
{
    unsigned int count     = 0;
    int          operation = detail_pid == -1 ? KERN_PROC_PROC : KERN_PROC_PID;
    if (threads)
    {
        operation |= KERN_PROC_INC_THREAD;
    }
    errno               = 0;
    kinfo_proc* entries = procstat_getprocs(top.procstat, operation, detail_pid == -1 ? 0 : detail_pid, &count);
    if (entries == nullptr && detail_pid != -1 && errno == ESRCH)
    {
        count = 0;
    }
    else if (entries == nullptr)
    {
        error = std::format("procstat_getprocs: {}", std::strerror(errno));
        return false;
    }
    std::unique_ptr<kinfo_proc, procstat_procs_deleter> entries_owner(entries, procstat_procs_deleter{top.procstat});

    auto sample_timestamp   = std::chrono::steady_clock::now();
    auto sample_interval_us = sampled_at != std::chrono::steady_clock::time_point{} && sample_timestamp > sampled_at ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(sample_timestamp - sampled_at).count()) : 0;

    snapshot_t next;
    next.processes.reserve(static_cast<size_t>(count));
    std::unordered_map<int, previous_process_t> history;
    history.reserve(static_cast<size_t>(count));
    std::unordered_map<uid_t, std::string> usernames;
    cpuset_t                               available;
    if (threads && cpuset_getaffinity(CPU_LEVEL_ROOT, CPU_WHICH_PID, -1, sizeof(available), &available) == -1)
    {
        error = std::format("cpuset_getaffinity (root): {}", std::strerror(errno));
        return false;
    }
    for (unsigned int i = 0; i < count; ++i)
    {
        auto& p = entries_owner.get()[i];
        if (p.ki_stat == 0)
        {
            continue;
        }
        if (p.ki_stat == SZOMB)
        {
            continue;
        }

        std::string affinity;
        if (threads && !(p.ki_flag & P_KPROC))
        {
            cpuset_t mask;
            if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, p.ki_tid, sizeof(mask), &mask) == -1)
            {
                if (errno == ESRCH)
                {
                    continue; // The thread exited after the kernel snapshot.
                }
                error = std::format("cpuset_getaffinity (TID {}): {}", p.ki_tid, std::strerror(errno));
                return false;
            }
            if (!CPU_EQUAL(&mask, &available))
            {
                for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                {
                    if (CPU_ISSET(cpu, &mask))
                    {
                        std::format_to(std::back_inserter(affinity), "{}{}", affinity.empty() ? "" : ",", cpu);
                    }
                }
            }
        }

        double                cpu_fraction   = static_cast<double>(p.ki_pctcpu) / top.fscale;
        auto                  user_time_us   = microseconds(p.ki_rusage.ru_utime);
        auto                  kernel_time_us = microseconds(p.ki_rusage.ru_stime);
        std::optional<double> user_cpu;
        std::optional<double> kernel_cpu;
        bool                  kernel_process = (p.ki_flag & P_KPROC) != 0;
        if (sampled_at == std::chrono::steady_clock::time_point{})
        {
            user_cpu   = 0.0;
            kernel_cpu = 0.0;
        }
        int                   record_id       = threads ? p.ki_tid : p.ki_pid;
        auto                  previous_record = previous.find(record_id);
        bool                  same_process    = previous_record != previous.end() && p.ki_runtime >= previous_record->second.runtime_us;
        auto                  nvcsw           = static_cast<uint64_t>(p.ki_rusage.ru_nvcsw);
        auto                  nivcsw          = static_cast<uint64_t>(p.ki_rusage.ru_nivcsw);
        std::optional<double> nvcsw_per_second;
        std::optional<double> nivcsw_per_second;
        if (same_process)
        {
            nvcsw_per_second  = counter_rate(nvcsw, previous_record->second.nvcsw, sample_interval_us);
            nivcsw_per_second = counter_rate(nivcsw, previous_record->second.nivcsw, sample_interval_us);
        }
        if (sample_interval_us != 0)
        {
            // A PID may have been reused between samples.
            if (same_process)
            {
                cpu_fraction = static_cast<double>(p.ki_runtime - previous_record->second.runtime_us) / sample_interval_us;
                if (!kernel_process && user_time_us >= previous_record->second.user_us && kernel_time_us >= previous_record->second.kernel_us)
                {
                    user_cpu   = static_cast<double>(user_time_us - previous_record->second.user_us) / sample_interval_us * 100;
                    kernel_cpu = static_cast<double>(kernel_time_us - previous_record->second.kernel_us) / sample_interval_us * 100;
                }
            }
            else if (!same_process)
            {
                cpu_fraction = static_cast<double>(p.ki_runtime) / sample_interval_us;
                if (!kernel_process)
                {
                    user_cpu   = static_cast<double>(user_time_us) / sample_interval_us * 100;
                    kernel_cpu = static_cast<double>(kernel_time_us) / sample_interval_us * 100;
                }
            }
        }
        history.emplace(record_id, previous_process_t{p.ki_runtime, user_time_us, kernel_time_us, nvcsw, nivcsw});
        if (threads)
        {
            cpu_fraction = std::min(cpu_fraction, 1.0);
        }
        if (kernel_process)
        {
            user_cpu.reset();
            kernel_cpu = cpu_fraction * 100;
        }

        std::string username;
        if (!threads)
        {
            auto username_entry = usernames.find(p.ki_ruid);
            if (username_entry == usernames.end())
            {
                errno                 = 0;
                const passwd* account = getpwuid(p.ki_ruid);
                if (account == nullptr && errno != 0)
                {
                    error = std::format("getpwuid: {}", std::strerror(errno));
                    return false;
                }
                username_entry = usernames.emplace(p.ki_ruid, account == nullptr ? std::to_string(p.ki_ruid) : text(account->pw_name, std::strlen(account->pw_name))).first;
            }
            username = username_entry->second;
        }
        std::string command = text(p.ki_comm, sizeof(p.ki_comm));
        if (threads)
        {
            std::string thread_name = std::format("{}{}", text(p.ki_tdname, sizeof(p.ki_tdname)), text(p.ki_moretdname, sizeof(p.ki_moretdname)));
            if (!thread_name.empty())
            {
                command = thread_name;
            }
        }
        if (!threads && !(p.ki_flag & P_INMEM))
        {
            command = std::format("<{}>", command);
        }
        int                   cpu_id      = p.ki_stat == SRUN && p.ki_oncpu != NOCPU ? p.ki_oncpu : p.ki_lastcpu;
        uint64_t              size_kb     = static_cast<uint64_t>(p.ki_size) / 1024;
        uint64_t              resident_kb = static_cast<uint64_t>(p.ki_rssize) * top.page_size / 1024;
        std::optional<size_t> nofile;
        if (!threads)
        {
            std::unique_ptr<filestat_list, procstat_files_deleter> files(
                procstat_getfiles(top.procstat, &p, 0), procstat_files_deleter{top.procstat});
            if (files != nullptr)
            {
                nofile = 0;
                filestat* file;
                STAILQ_FOREACH(file, files.get(), next)
                {
                    if (file->fs_fd >= 0)
                    {
                        ++*nofile;
                    }
                }
            }
        }
        next.processes.push_back(process_t{
            .pid                = p.ki_pid,
            .tid                = p.ki_tid,
            .jid                = p.ki_jid,
            .kernel_process     = kernel_process,
            .username           = std::move(username),
            .threads            = p.ki_numthreads,
            .nofile             = nofile,
            .priority           = p.ki_pri.pri_level - PUSER,
            .nice               = nice(p),
            .size_kb            = size_kb,
            .resident_kb        = resident_kb,
            .state              = state(p),
            .cpu                = cpu_id == NOCPU ? -1 : cpu_id,
            .runtime_us         = p.ki_runtime,
            .cpu_percent        = cpu_fraction * 100,
            .nvcsw              = nvcsw,
            .nivcsw             = nivcsw,
            .nvcsw_per_second   = nvcsw_per_second,
            .nivcsw_per_second  = nivcsw_per_second,
            .user_cpu_percent   = user_cpu,
            .kernel_cpu_percent = kernel_cpu,
            .command            = command,
            .affinity           = affinity});
    }
    if (!threads)
    {
        std::ranges::sort(next.processes, compare_processes);
    }
    snapshot                = std::move(next);
    next_history.previous   = std::move(history);
    next_history.sampled_at = sample_timestamp;
    return true;
}

static bool compare_threads(const process_t& a, const process_t& b)
{
    return a.pid != b.pid ? a.pid < b.pid : a.tid < b.tid;
}

static std::array<int, CPUSTATES> cpu_percentages(const std::array<long, CPUSTATES>& current, const std::array<long, CPUSTATES>& previous)
{
    std::array<long, CPUSTATES> differences{};
    long                        total = 0;
    for (size_t i = 0; i < current.size(); ++i)
    {
        auto difference = static_cast<unsigned long>(current[i]) - static_cast<unsigned long>(previous[i]);
        long change     = static_cast<long>(difference);
        if (change < 0)
        {
            // Match FreeBSD top's counter-wrap handling.
            change = static_cast<int>(difference);
        }
        differences[i] = change;
        total += change;
    }
    if (total == 0)
    {
        total = 1;
    }
    std::array<int, CPUSTATES> result{};
    long                       half_total = total / 2;
    for (size_t i = 0; i < result.size(); ++i)
    {
        result[i] = static_cast<int>((differences[i] * 1000 + half_total) / total);
    }
    return result;
}

static bool sample_cpus(const top_t& top, snapshot_t& snapshot, std::vector<std::array<long, CPUSTATES>>& per_cpu_ticks, std::string& error)
{
    per_cpu_ticks.resize(top.max_cpus);
    size_t size = per_cpu_ticks.size() * sizeof(per_cpu_ticks[0]);
    if (sysctlbyname("kern.cp_times", per_cpu_ticks.data(), &size, nullptr, 0) == -1)
    {
        error = std::format("kern.cp_times: {}", std::strerror(errno));
        return false;
    }
    if (size == 0 || size % sizeof(per_cpu_ticks[0]) != 0 || size > per_cpu_ticks.size() * sizeof(per_cpu_ticks[0]))
    {
        error = "kern.cp_times: unexpected sysctl value size";
        return false;
    }
    per_cpu_ticks.resize(size / sizeof(per_cpu_ticks[0]));
    const std::array<long, CPUSTATES> zero{};
    for (size_t id = 0; id < per_cpu_ticks.size(); ++id)
    {
        const auto& ticks = per_cpu_ticks[id];
        if (std::ranges::all_of(ticks, [](long value) { return value == 0; }))
        {
            continue;
        }
        const auto& previous = id < top.previous_per_cpu_ticks.size() ? top.previous_per_cpu_ticks[id] : zero;
        snapshot.cpus.push_back(cpu_t{static_cast<int>(id), ticks, cpu_percentages(ticks, previous)});
    }
    if (snapshot.cpus.empty())
    {
        error = "kern.cp_times: no active CPUs";
        return false;
    }
    size = sizeof(snapshot.cpu.ticks);
    if (sysctlbyname("kern.cp_time", snapshot.cpu.ticks.data(), &size, nullptr, 0) == -1)
    {
        error = std::format("kern.cp_time: {}", std::strerror(errno));
        return false;
    }
    if (size != sizeof(snapshot.cpu.ticks))
    {
        error = "kern.cp_time: unexpected sysctl value size";
        return false;
    }
    snapshot.cpu.percent_tenths = cpu_percentages(snapshot.cpu.ticks, top.previous_cpu_ticks);
    return true;
}

bool top_sample(top_t& top, snapshot_t& snapshot, std::string& error, int detail_pid)
{
    snapshot_t                                        next;
    snapshot_t                                        threads;
    sample_history_t                                  process_history;
    sample_history_t                                  thread_history;
    const bool                                        same_view = detail_pid == top.sampled_pid;
    const std::unordered_map<int, previous_process_t> empty_history;
    if (!sample_records(top, next, false, detail_pid, same_view ? top.previous : empty_history,
                        same_view ? top.sampled_at : std::chrono::steady_clock::time_point{}, process_history, error))
    {
        return false;
    }
    if (detail_pid != -1 &&
        !sample_records(top, threads, true, detail_pid, same_view ? top.thread_previous : empty_history,
                        same_view ? top.thread_sampled_at : std::chrono::steady_clock::time_point{}, thread_history, error))
    {
        return false;
    }
    std::vector<std::array<long, CPUSTATES>> per_cpu_ticks;
    if (!sample_cpus(top, next, per_cpu_ticks, error))
    {
        return false;
    }
    std::ranges::sort(threads.processes, compare_threads);
    next.threads               = std::move(threads.processes);
    top.previous               = std::move(process_history.previous);
    top.sampled_at             = process_history.sampled_at;
    top.thread_previous        = std::move(thread_history.previous);
    top.thread_sampled_at      = thread_history.sampled_at;
    top.sampled_pid            = detail_pid;
    top.previous_cpu_ticks     = next.cpu.ticks;
    top.previous_per_cpu_ticks = std::move(per_cpu_ticks);
    snapshot                   = std::move(next);
    return true;
}