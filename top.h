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

#pragma once

#include <sys/time.h>
#include <sys/types.h>

#include <vector>
#include <string>
#include <stdint.h>
#include <optional>
#include <unordered_map>

struct __kvm;

struct process_t
{
    int                   pid;
    lwpid_t               tid;
    int                   jid;
    std::string           username;
    int                   threads;
    int                   priority;
    std::string           nice;
    uint64_t              size_kb;
    uint64_t              resident_kb;
    uint64_t              swap_kb;
    std::string           state;
    int                   cpu;
    uint64_t              runtime_us;
    double                cpu_percent;
    uint64_t              nvcsw;
    uint64_t              nivcsw;
    std::optional<double> user_cpu_percent;
    std::optional<double> system_cpu_percent;
    std::string           command;
    std::string           affinity;
};

struct snapshot_t
{
    std::vector<process_t> processes;
    std::vector<process_t> threads;
};

struct previous_process_t
{
    timeval  start;
    uint64_t runtime_us;
    uint64_t user_us;
    uint64_t system_us;
};

struct top_t
{
    __kvm*                                      kernel     = nullptr;
    long                                        page_size  = 0;
    int                                         fscale     = 0;
    uint64_t                                    sampled_at = 0;
    timeval                                     sampled_wall{};
    std::unordered_map<int, previous_process_t> previous;
    uint64_t                                    thread_sampled_at = 0;
    timeval                                     thread_sampled_wall{};
    std::unordered_map<int, previous_process_t> thread_previous;
};

bool top_open(top_t& top, std::string& error);

void top_close(top_t& top);

bool top_sample(top_t& top, snapshot_t& snapshot, std::string& error);