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
#include "tui.h"
#include "terminal.h"

#include <chrono>
#include <format>
#include <string>
#include <vector>
#include <stdio.h>
#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include <algorithm>

static std::string memory(uint64_t kb)
{
    constexpr const char* units[] = {"K", "M", "G", "T", "P"};
    size_t                unit    = 0;
    while (kb >= 10000 && unit + 1 < std::size(units))
    {
        kb = (kb + 512) / 1024;
        ++unit;
    }
    return std::format("{}{}", kb, units[unit]);
}

static std::string runtime(uint64_t microseconds)
{
    auto seconds = (microseconds + 500000) / 1000000;
    if (seconds < 6000)
    {
        return std::format("{}:{:02}", seconds / 60, seconds % 60);
    }
    return std::format("{}h", seconds / 3600);
}

struct display_row_t
{
    std::string      text;
    int              pid            = -1;
    lwpid_t          tid            = -1;
    bool             thread         = false;
    int              threads        = 0;
    const process_t* record         = nullptr;
    const process_t* command_source = nullptr;
    size_t           affinity_width = 3;
};

static std::string switch_number(long double value)
{
    char suffix = '\0';
    for (char unit : std::string_view("KMGTPE"))
    {
        if (value < 999.5L)
        {
            break;
        }
        value /= 1000;
        suffix = unit;
    }
    return suffix == '\0' ? std::format("{:.0f}", value) : std::format("{:.0f}{}", value, suffix);
}

static std::string switch_value(uint64_t count, const std::optional<double>& rate)
{
    auto value = switch_number(count);
    if (rate.has_value())
    {
        auto rounded = switch_number(*rate);
        if (rounded != "0")
        {
            value += "+" + rounded;
        }
    }
    if (value.size() > 9)
    {
        value.resize(6);
        value += "...";
    }
    return value;
}

static std::array<std::string, 2> switch_values(const process_t& p)
{
    return {switch_value(p.nivcsw, p.nivcsw_per_second),
            switch_value(p.nvcsw, p.nvcsw_per_second)};
}

static std::string process_line(const process_t& p, bool thread, const process_t& command_source, bool show_context_switches = false, size_t affinity_width = 3)
{
    std::string cpu = p.cpu < 0 ? "-" : std::to_string(p.cpu);
    std::string row;
    if (thread || show_context_switches)
    {
        row += std::format("{:>6} ", p.tid);
    }
    else
    {
        row += std::format("{:>5} ", p.pid);
        row += std::format("{:<10} ", p.username.substr(0, 10));
        row += std::format("{:>4} ", p.threads);
    }
    row += std::format("{:>3} {:>4} ", p.priority, p.nice);
    if (!thread && !show_context_switches)
    {
        row += std::format("{:>7} {:>6} ", memory(p.size_kb), memory(p.resident_kb));
    }
    if (!thread && !show_context_switches)
    {
        row += std::format("{:<6} {:>3} ", p.state.substr(0, 6), cpu);
    }
    else
    {
        row += std::format("{:<6} ", p.state.substr(0, 6));
        const auto values = switch_values(command_source);
        for (size_t i = 0; i < values.size(); ++i)
        {
            row += std::format("{:<9} ", values[i]);
        }
    }
    row += std::format("{:>6} ", runtime(p.runtime_us));
    if ((!thread && !show_context_switches) || p.kernel_process)
    {
        row += std::format("{:>8.2f}% ", p.cpu_percent);
    }
    if (thread || show_context_switches)
    {
        if (!p.kernel_process)
        {
            for (const auto& percent : {p.user_cpu_percent, p.system_cpu_percent})
            {
                if (percent.has_value())
                {
                    row += std::format("{:>8.2f}% ", *percent);
                }
                else
                {
                    row += std::format("{:>9} ", "-");
                }
            }
        }
        if (!p.kernel_process)
        {
            row += std::format("{:>{}} ", command_source.affinity.empty() ? "-" : command_source.affinity, affinity_width);
        }
        row += std::format("{:>3} ", cpu);
    }
    if (p.jid > 0 && !thread && !show_context_switches)
    {
        row += std::format("[jail:{}] ", p.jid);
    }
    row += command_source.command;
    return row;
}

static std::vector<display_row_t> lines(const snapshot_t& snapshot, int detail_pid, bool format_rows = true)
{
    std::vector<display_row_t> result;
    result.reserve(1 + snapshot.processes.size() + (detail_pid == -1 ? 0 : snapshot.threads.size()));
    bool                                  kernel_process = false;
    size_t                                affinity_width = 3;
    const std::array<std::string_view, 2> switch_headers = {"NIVCSW", "NVCSW"};
    if (detail_pid != -1)
    {
        for (const auto& process : snapshot.processes)
        {
            if (process.pid == detail_pid)
            {
                kernel_process = process.kernel_process;
                affinity_width = std::max(affinity_width, process.affinity.size());
            }
        }
        for (const auto& thread : snapshot.threads)
        {
            if (thread.pid == detail_pid)
            {
                affinity_width = std::max(affinity_width, thread.affinity.size());
            }
        }
    }
    std::string header;
    if (detail_pid != -1)
    {
        header += std::format("{:>6} ", "TID");
    }
    else
    {
        header += std::format("{:>5} ", "PID");
        header += std::format("{:<10} ", "USERNAME");
        header += std::format("{:>4} ", "THR");
    }
    header += std::format("{:>3} {:>4} ", "PRI", "NICE");
    if (detail_pid == -1)
    {
        header += std::format("{:>7} {:>6} ", "SIZE", "RES");
    }
    if (detail_pid == -1)
    {
        header += std::format("{:<6} {:>3} ", "STATE", "C");
    }
    else
    {
        header += std::format("{:<6} ", "STATE");
        for (size_t i = 0; i < switch_headers.size(); ++i)
        {
            header += std::format("{:<9} ", switch_headers[i]);
        }
    }
    header += std::format("{:>6} ", "TIME");
    if (detail_pid == -1 || kernel_process)
    {
        header += std::format("{:>9} ", "CPU");
    }
    if (detail_pid != -1)
    {
        if (!kernel_process)
        {
            header += std::format("{:>9} {:>9} ", "UCPU", "SCPU");
        }
        if (!kernel_process)
        {
            header += std::format("{:>{}} ", "A", affinity_width);
        }
        header += std::format("{:>3} ", "C");
    }
    header += detail_pid == -1 ? "COMMAND" : "THREAD";
    result.push_back({header});
    auto append = [&](const process_t& p, bool thread, const process_t& source) {
        std::string text;
        if (format_rows)
        {
            text = process_line(p, thread, source, detail_pid != -1, affinity_width);
        }
        result.push_back({std::move(text), p.pid, p.tid, thread, thread ? 1 : p.threads,
                          &p, &source, affinity_width});
    };
    for (const auto& p : snapshot.processes)
    {
        if (detail_pid == -1)
        {
            append(p, false, p);
            continue;
        }
        if (p.pid != detail_pid)
        {
            continue;
        }

        auto             thread_begin   = std::ranges::lower_bound(snapshot.threads, p.pid, {}, &process_t::pid);
        auto             thread_end     = std::ranges::upper_bound(snapshot.threads, p.pid, {}, &process_t::pid);
        const process_t* command_source = &p;
        auto             representative = std::ranges::find_if(thread_begin, thread_end, [&](const process_t& thread) { return thread.tid == p.tid; });
        if (representative != thread_end)
        {
            command_source = &*representative;
        }
        append(p, false, *command_source);
        for (auto thread = thread_begin; thread != thread_end; ++thread)
        {
            if (thread->tid != p.tid)
            {
                append(*thread, true, *thread);
            }
        }
    }
    return result;
}

static void move_selection(const std::vector<display_row_t>& output, int direction, int visible_rows, int& selected_pid, lwpid_t& selected_tid, int& scroll_offset)
{
    int total_rows = static_cast<int>(output.size()) - 1;
    if (total_rows <= 0 || visible_rows <= 0)
    {
        return;
    }
    int selected_row_index = 0;
    for (int i = 1; i <= total_rows; ++i)
    {
        const auto& row = output[i];
        if (row.pid == selected_pid && (row.thread ? row.tid == selected_tid : selected_tid == -1))
        {
            selected_row_index = i;
            break;
        }
    }
    if (selected_row_index == 0)
    {
        selected_row_index = direction > 0 ? scroll_offset + 1 : std::min(total_rows, scroll_offset + visible_rows);
    }
    else
    {
        selected_row_index = std::clamp(selected_row_index + direction, 1, total_rows);
    }
    const auto& row = output[selected_row_index];
    selected_pid    = row.pid;
    selected_tid    = row.thread ? row.tid : -1;
    if (selected_row_index - 1 < scroll_offset)
    {
        scroll_offset = selected_row_index - 1;
    }
    else if (selected_row_index > scroll_offset + visible_rows)
    {
        scroll_offset = selected_row_index - visible_rows;
    }
}

int tui(top_t& top)
{
    std::string error;
    int         status = 0;
    {
        terminal_t terminal;
        if (!terminal_open(terminal, error))
        {
            status = 1;
        }
        else
        {
            snapshot_t snapshot;
            int        selected_pid         = -1;
            lwpid_t    selected_tid         = -1;
            int        scroll_offset        = 0;
            int        scroll_before_filter = 0;
            int        filtered_pid         = -1;
            int        pressed_pid          = -1;
            lwpid_t    pressed_tid          = -1;
            bool       toggle_on_release    = false;
            bool       redraw               = true;
            auto       next_sample          = std::chrono::steady_clock::now();

            std::vector<display_row_t>  display_output;
            std::vector<terminal_row_t> frame;
            auto                        refresh = [&] {
                if (!top_sample(top, snapshot, error, filtered_pid))
                {
                    status = 1;
                    return false;
                }
                display_output = lines(snapshot, filtered_pid, false);
                next_sample    = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                redraw         = true;
                return true;
            };
            auto enter_detail = [&] {
                if (filtered_pid != -1 || selected_pid == -1)
                {
                    return;
                }
                scroll_before_filter = scroll_offset;
                scroll_offset        = 0;
                filtered_pid         = selected_pid;
                selected_pid         = -1;
                selected_tid         = -1;
                pressed_pid          = -1;
                pressed_tid          = -1;
                toggle_on_release    = false;
                refresh();
            };
            auto exit_detail = [&](int visible_rows) {
                int target_pid    = filtered_pid;
                selected_pid      = -1;
                selected_tid      = -1;
                filtered_pid      = -1;
                scroll_offset     = scroll_before_filter;
                pressed_pid       = -1;
                pressed_tid       = -1;
                toggle_on_release = false;
                if (!refresh())
                {
                    return;
                }
                selected_pid = top.previous.contains(target_pid) ? target_pid : -1;
                if (selected_pid != -1)
                {
                    move_selection(display_output, 0, visible_rows, selected_pid, selected_tid, scroll_offset);
                }
                redraw = true;
            };

            while (status == 0 && !terminal_stopped())
            {
                if (std::chrono::steady_clock::now() >= next_sample)
                {
                    if (!refresh())
                    {
                        status = 1;
                        break;
                    }
                    if (selected_pid != -1 && !std::ranges::any_of(display_output, [&](const display_row_t& row) {
                            return row.pid == selected_pid && (row.thread ? row.tid == selected_tid : selected_tid == -1);
                        }))
                    {
                        selected_pid      = -1;
                        selected_tid      = -1;
                        pressed_pid       = -1;
                        pressed_tid       = -1;
                        toggle_on_release = false;
                    }
                }
                bool resized = false;
                if (!terminal_size(terminal, resized, error))
                {
                    status = 1;
                    break;
                }
                redraw                 = redraw || resized;
                int height             = terminal.height;
                int width              = terminal.width;
                int total_rows         = static_cast<int>(display_output.size()) - 1;
                int table_width        = width;
                int visible_rows       = std::max(0, height - 1);
                int max_offset         = std::max(0, total_rows - visible_rows);
                scroll_offset          = std::clamp(scroll_offset, 0, max_offset);
                int visible_line_count = std::min(visible_rows, total_rows - scroll_offset) + 1;
                if (redraw)
                {
                    frame.resize(static_cast<size_t>(height));
                    for (int row = 0; row < height; ++row)
                    {
                        frame[row].reverse    = false;
                        frame[row].foreground = terminal_color_t::normal;
                        frame[row].colors.clear();
                        int table_row = row;
                        if (table_row >= visible_line_count)
                        {
                            frame[row].text.clear();
                            continue;
                        }
                        int   output_row_index = table_row == 0 ? 0 : table_row + scroll_offset;
                        bool  row_is_selected  = table_row > 0 && display_output[output_row_index].pid == selected_pid && (display_output[output_row_index].thread ? display_output[output_row_index].tid == selected_tid : selected_tid == -1);
                        auto& displayed        = display_output[output_row_index];
                        if (table_row != 0 && displayed.text.empty())
                        {
                            displayed.text = process_line(*displayed.record, displayed.thread, *displayed.command_source,
                                                          filtered_pid != -1, displayed.affinity_width);
                        }
                        frame[row].text       = displayed.text;
                        auto selection_color  = filtered_pid == -1 ? terminal_color_t::blue : terminal_color_t::green;
                        frame[row].foreground = row_is_selected ? selection_color : terminal_color_t::normal;
                    }
                    if (!terminal_render(terminal, frame, error))
                    {
                        status = 1;
                        break;
                    }
                    redraw = false;
                }
                terminal_event_t input;
                auto             remaining  = std::chrono::ceil<std::chrono::milliseconds>(next_sample - std::chrono::steady_clock::now()).count();
                int              timeout_ms = static_cast<int>(std::clamp<int64_t>(remaining, 0, 1000));
                if (!terminal_read(terminal, input, timeout_ms, error))
                {
                    status = 1;
                    break;
                }
                if (terminal_stopped() ||
                    (input.kind == terminal_event_kind_t::character && input.character == 'q'))
                {
                    break;
                }
                if (input.kind == terminal_event_kind_t::mouse_press || input.kind == terminal_event_kind_t::mouse_release)
                {
                    if (input.kind == terminal_event_kind_t::mouse_press)
                    {
                        pressed_pid       = -1;
                        pressed_tid       = -1;
                        toggle_on_release = false;
                        if (input.y > 0 && input.y < visible_line_count && input.x >= 0 && input.x < table_width)
                        {
                            const auto& pressed_row  = display_output[input.y + scroll_offset];
                            bool        was_selected = selected_pid == pressed_row.pid && (pressed_row.thread ? selected_tid == pressed_row.tid : selected_tid == -1);
                            pressed_pid              = pressed_row.pid;
                            pressed_tid              = pressed_row.tid;
                            toggle_on_release        = was_selected && filtered_pid == -1;
                            selected_pid             = pressed_row.pid;
                            selected_tid             = pressed_row.thread ? pressed_row.tid : -1;
                            redraw                   = true;
                        }
                    }
                    if (input.kind == terminal_event_kind_t::mouse_release)
                    {
                        if (toggle_on_release && input.y > 0 && input.y < visible_line_count && input.x >= 0 && input.x < table_width)
                        {
                            const auto& released_row = display_output[input.y + scroll_offset];
                            if (released_row.pid == pressed_pid && released_row.tid == pressed_tid && selected_pid == released_row.pid &&
                                selected_tid == (released_row.thread ? released_row.tid : -1))
                            {
                                enter_detail();
                            }
                        }
                        pressed_pid       = -1;
                        pressed_tid       = -1;
                        toggle_on_release = false;
                    }
                }
                else if (input.kind == terminal_event_kind_t::escape || input.kind == terminal_event_kind_t::mouse_right_press)
                {
                    if (selected_pid == -1 && filtered_pid != -1)
                    {
                        exit_detail(visible_rows);
                    }
                    else
                    {
                        selected_pid      = -1;
                        selected_tid      = -1;
                        pressed_pid       = -1;
                        pressed_tid       = -1;
                        toggle_on_release = false;
                        redraw            = true;
                    }
                }
                else if (input.kind == terminal_event_kind_t::redraw)
                {
                    redraw = true;
                }
                else if (input.kind == terminal_event_kind_t::up || input.kind == terminal_event_kind_t::down ||
                         input.kind == terminal_event_kind_t::wheel_up || input.kind == terminal_event_kind_t::wheel_down)
                {
                    int direction = input.kind == terminal_event_kind_t::up || input.kind == terminal_event_kind_t::wheel_up ? -1 : 1;
                    move_selection(display_output, direction, visible_rows, selected_pid, selected_tid, scroll_offset);
                    redraw = true;
                }
                else if (input.kind == terminal_event_kind_t::character && input.character == ' ')
                {
                    if (filtered_pid != -1)
                    {
                        exit_detail(visible_rows);
                    }
                    else
                    {
                        enter_detail();
                    }
                }
            }
        }
        std::string close_error;
        if (!terminal_close(terminal, close_error))
        {
            if (!error.empty())
            {
                error += "; ";
            }
            error += close_error;
            status = 1;
        }
    }
    if (!error.empty())
    {
        fprintf(stderr, "ytop: %s\n", error.c_str());
    }
    return status;
}