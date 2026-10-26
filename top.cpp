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

#include <sys/proc.h>
#include <sys/user.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/rtprio.h>
#include <sys/sysctl.h>
#include <sys/resource.h>

#include <kvm.h>
#include <sys/time.h>
#include <pwd.h>
#include <fcntl.h>
#include <paths.h>
#include <stddef.h>
#include <unistd.h>

#include <format>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <algorithm>

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
    if (top.kernel != nullptr)
    {
        kvm_close(top.kernel);
    }
    top = {};
}

bool top_open(top_t& top, std::string& error)
{
    char message[_POSIX2_LINE_MAX]{};
    top.kernel = kvm_openfiles(nullptr, _PATH_DEVNULL, nullptr, O_RDONLY, message);
    if (top.kernel == nullptr)
    {
        error = std::format("kvm_openfiles: {}", message);
        return false;
    }
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

static bool sample_records(top_t& top, snapshot_t& snapshot, bool threads, const std::unordered_map<int, previous_process_t>& previous, std::chrono::steady_clock::time_point sampled_at, sample_history_t& next_history, std::string& error)
{
    int         count   = 0;
    kinfo_proc* entries = kvm_getprocs(top.kernel, threads ? KERN_PROC_ALL : KERN_PROC_PROC, 0, &count);
    if (entries == nullptr)
    {
        error = std::format("kvm_getprocs: {}", kvm_geterr(top.kernel));
        return false;
    }

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
    for (int i = 0; i < count; ++i)
    {
        const auto& p = entries[i];
        if (p.ki_stat == 0)
        {
            continue;
        }
        if (p.ki_stat == SZOMB)
        {
            continue;
        }

        std::string affinity;
        if (threads)
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
                        std::format_to(std::back_inserter(affinity), "{}{}", affinity.empty() ? "[CPU" : ",", cpu);
                    }
                }
                if (!affinity.empty())
                {
                    affinity += "]";
                }
            }
        }

        double                cpu_fraction   = static_cast<double>(p.ki_pctcpu) / top.fscale;
        auto                  user_time_us   = microseconds(p.ki_rusage.ru_utime);
        auto                  system_time_us = microseconds(p.ki_rusage.ru_stime);
        std::optional<double> user_cpu;
        std::optional<double> system_cpu;
        bool                  kernel_process = (p.ki_flag & P_KPROC) != 0;
        if (sampled_at == std::chrono::steady_clock::time_point{})
        {
            user_cpu   = 0.0;
            system_cpu = 0.0;
        }
        int  record_id       = threads ? p.ki_tid : p.ki_pid;
        auto previous_record = previous.find(record_id);
        bool same_process    = previous_record != previous.end() && p.ki_runtime >= previous_record->second.runtime_us;
        if (sample_interval_us != 0)
        {
            // A PID may have been reused between samples.
            if (same_process)
            {
                cpu_fraction = static_cast<double>(p.ki_runtime - previous_record->second.runtime_us) / sample_interval_us;
                if (!kernel_process && user_time_us >= previous_record->second.user_us && system_time_us >= previous_record->second.system_us)
                {
                    user_cpu   = static_cast<double>(user_time_us - previous_record->second.user_us) / sample_interval_us * 100;
                    system_cpu = static_cast<double>(system_time_us - previous_record->second.system_us) / sample_interval_us * 100;
                }
            }
            else if (!same_process)
            {
                cpu_fraction = static_cast<double>(p.ki_runtime) / sample_interval_us;
                if (!kernel_process)
                {
                    user_cpu   = static_cast<double>(user_time_us) / sample_interval_us * 100;
                    system_cpu = static_cast<double>(system_time_us) / sample_interval_us * 100;
                }
            }
        }
        auto first_seen = same_process ? previous_record->second.first_seen : sample_timestamp;
        history.emplace(record_id, previous_process_t{first_seen, p.ki_runtime, user_time_us, system_time_us});
        if (threads)
        {
            cpu_fraction = std::min(cpu_fraction, 1.0);
        }
        if (kernel_process)
        {
            user_cpu.reset();
            system_cpu = cpu_fraction * 100;
        }

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
        std::string command = text(p.ki_comm, sizeof(p.ki_comm));
        if (threads)
        {
            std::string thread_name = std::format("{}{}", text(p.ki_tdname, sizeof(p.ki_tdname)), text(p.ki_moretdname, sizeof(p.ki_moretdname)));
            if (thread_name != command)
            {
                command = std::format("{}{{{}}}", command, thread_name);
            }
        }
        if (!threads && !(p.ki_flag & P_INMEM))
        {
            command = std::format("<{}>", command);
        }
        int      cpu_id      = p.ki_stat == SRUN && p.ki_oncpu != NOCPU ? p.ki_oncpu : p.ki_lastcpu;
        uint64_t size_kb     = static_cast<uint64_t>(p.ki_size) / 1024;
        uint64_t resident_kb = static_cast<uint64_t>(p.ki_rssize) * top.page_size / 1024;
        uint64_t swap_kb     = static_cast<uint64_t>(p.ki_swrss > p.ki_rssize ? p.ki_swrss - p.ki_rssize : 0) * top.page_size / 1024;
        next.processes.push_back(process_t{
            .pid                = p.ki_pid,
            .tid                = p.ki_tid,
            .jid                = p.ki_jid,
            .username           = username_entry->second,
            .threads            = p.ki_numthreads,
            .priority           = p.ki_pri.pri_level - PUSER,
            .nice               = nice(p),
            .size_kb            = size_kb,
            .resident_kb        = resident_kb,
            .swap_kb            = swap_kb,
            .state              = state(p),
            .cpu                = cpu_id == NOCPU ? -1 : cpu_id,
            .runtime_us         = p.ki_runtime,
            .cpu_percent        = cpu_fraction * 100,
            .nvcsw              = static_cast<uint64_t>(p.ki_rusage.ru_nvcsw),
            .nivcsw             = static_cast<uint64_t>(p.ki_rusage.ru_nivcsw),
            .user_cpu_percent   = user_cpu,
            .system_cpu_percent = system_cpu,
            .command            = command,
            .affinity           = affinity});
    }
    std::ranges::sort(next.processes, compare_processes);
    snapshot                = std::move(next);
    next_history.previous   = std::move(history);
    next_history.sampled_at = sample_timestamp;
    return true;
}

static bool compare_threads(const process_t& a, const process_t& b)
{
    return a.pid != b.pid ? a.pid < b.pid : a.tid < b.tid;
}

bool top_sample(top_t& top, snapshot_t& snapshot, std::string& error)
{
    snapshot_t       next;
    snapshot_t       threads;
    sample_history_t process_history;
    sample_history_t thread_history;
    if (!sample_records(top, next, false, top.previous, top.sampled_at, process_history, error) || !sample_records(top, threads, true, top.thread_previous, top.thread_sampled_at, thread_history, error))
    {
        return false;
    }
    std::ranges::sort(threads.processes, compare_threads);
    next.threads          = std::move(threads.processes);
    top.previous          = std::move(process_history.previous);
    top.sampled_at        = process_history.sampled_at;
    top.thread_previous   = std::move(thread_history.previous);
    top.thread_sampled_at = thread_history.sampled_at;
    snapshot              = std::move(next);
    return true;
}