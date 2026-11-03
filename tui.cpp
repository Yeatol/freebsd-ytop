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
    while (kb >= 1024 && unit + 1 < std::size(units))
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
    size_t           username_width = 4;
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

static std::string process_line(const process_t& p, bool thread, const process_t& command_source, bool show_context_switches = false, size_t affinity_width = 3, size_t username_width = 4)
{
    std::string cpu = p.cpu < 0 ? "-" : std::to_string(p.cpu);
    std::string row;
    row.reserve(128 + affinity_width + command_source.command.size());
    if (thread || show_context_switches)
    {
        std::format_to(std::back_inserter(row), "{:>6} ", p.tid);
    }
    else
    {
        std::format_to(std::back_inserter(row), "{:>5} {:<{}} ", p.pid, p.username, username_width);
    }
    if (show_context_switches)
    {
        std::format_to(std::back_inserter(row), "{:>3} ", p.priority);
    }
    else
    {
        std::format_to(std::back_inserter(row), "{:>5} ", p.nice);
    }
    if (!thread && !show_context_switches)
    {
        std::format_to(std::back_inserter(row), "{:>5} {:>5} ", memory(p.size_kb), memory(p.resident_kb));
    }
    std::format_to(std::back_inserter(row), "{:>6} ", runtime(p.runtime_us));
    if ((!thread && !show_context_switches) || p.kernel_process)
    {
        std::format_to(std::back_inserter(row), "{:>8.2f}% ", p.cpu_percent);
    }
    if (thread || show_context_switches)
    {
        if (!p.kernel_process)
        {
            for (const auto& percent : {p.user_cpu_percent, p.kernel_cpu_percent})
            {
                if (percent.has_value())
                {
                    std::format_to(std::back_inserter(row), "{:>8.2f}% ", *percent);
                }
                else
                {
                    std::format_to(std::back_inserter(row), "{:>9} ", "-");
                }
            }
        }
        std::format_to(std::back_inserter(row), "{:<9} ", std::string_view(p.state).substr(0, 9));
        const auto values = switch_values(command_source);
        for (size_t i = 0; i < values.size(); ++i)
        {
            std::format_to(std::back_inserter(row), "{:<9} ", values[i]);
        }
        if (!p.kernel_process)
        {
            std::format_to(std::back_inserter(row), "{:>{}} ", command_source.affinity.empty() ? "-" : command_source.affinity, affinity_width);
        }
        std::format_to(std::back_inserter(row), "{:>3} ", cpu);
    }
    if (!thread && !show_context_switches)
    {
        const auto domain = p.jid > 0 ? std::format("j{}", p.jid) : p.kernel_process ? "k"
                                                                                     : "u";
        std::format_to(std::back_inserter(row), "{:>4} {:>4} ", domain, p.threads);
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
    size_t                                username_width = 4;
    const std::array<std::string_view, 2> switch_headers = {"NIVCSW", "NVCSW"};
    if (detail_pid == -1)
    {
        for (const auto& process : snapshot.processes)
        {
            username_width = std::max(username_width, process.username.size());
        }
    }
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
        header += std::format("{:<{}} ", "USER", username_width);
    }
    header += detail_pid != -1 ? std::format("{:>3} ", "PRI") : std::format("{:>5} ", "NICE");
    if (detail_pid == -1)
    {
        header += std::format("{:>5} {:>5} ", "SIZE", "RES");
    }
    header += std::format("{:>6} ", "TIME");
    if (detail_pid == -1 || kernel_process)
    {
        header += std::format("{:>9} ", detail_pid != -1 ? "KCPU" : "CPU");
    }
    if (detail_pid == -1)
    {
        header += std::format("{:>4} {:>4} ", "DOM", "THR");
    }
    if (detail_pid != -1)
    {
        if (!kernel_process)
        {
            header += std::format("{:>9} {:>9} ", "UCPU", "KCPU");
        }
        header += std::format("{:<9} ", "STATE");
        for (size_t i = 0; i < switch_headers.size(); ++i)
        {
            header += std::format("{:<9} ", switch_headers[i]);
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
            text = process_line(p, thread, source, detail_pid != -1, affinity_width, username_width);
        }
        result.push_back({std::move(text), p.pid, p.tid, thread, thread ? 1 : p.threads,
                          &p, &source, affinity_width, username_width});
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

static void scroll_view(const std::vector<display_row_t>& output, int direction, int visible_rows, int& scroll_offset)
{
    int total_rows = static_cast<int>(output.size()) - 1;
    if (total_rows <= 0 || visible_rows <= 0)
    {
        return;
    }
    int max_offset = std::max(0, total_rows - visible_rows);
    scroll_offset  = std::clamp(scroll_offset + direction * 3, 0, max_offset);
}

static terminal_row_t cpu_line(const cpu_t& cpu, int cells = 10)
{
    terminal_row_t row;
    row.text.reserve(static_cast<size_t>(cells) + 5);
    row.colors.reserve(5);
    row.text = std::format("{:>3}[", cpu.id);
    row.colors.push_back({0, terminal_color_t::bright_black});
    const size_t               start    = row.text.size();
    const int                  usage[]  = {cpu.percent_tenths[CP_USER] + cpu.percent_tenths[CP_NICE],
                                           cpu.percent_tenths[CP_SYS], cpu.percent_tenths[CP_INTR]};
    constexpr terminal_color_t colors[] = {terminal_color_t::green, terminal_color_t::red, terminal_color_t::yellow};
    int                        total    = 0;
    size_t                     filled   = 0;
    for (size_t i = 0; i < std::size(usage); ++i)
    {
        total += usage[i];
        size_t end = static_cast<size_t>((static_cast<int64_t>(std::clamp(total, 0, 1000)) * cells + 500) / 1000);
        if (end > filled)
        {
            row.colors.push_back({start + filled, colors[i]});
            row.text.append(end - filled, '|');
            filled = end;
        }
    }
    row.colors.push_back({start + filled, terminal_color_t::bright_black});
    row.text.append(static_cast<size_t>(cells) - filled, '|');
    row.text += ']';
    return row;
}

static int cpu_columns(int count, int width, int height = 0)
{
    int fitting   = std::max(1, width / 6);
    int preferred = 1;
    while (preferred + 1 <= count / (preferred + 1))
    {
        ++preferred;
    }
    int columns           = std::min({std::max(1, count), fitting, std::max(2, width / 30), std::max(2, preferred)});
    int preferred_columns = columns;
    columns               = std::min(columns, 2);
    for (int candidate = preferred_columns; candidate >= 2; --candidate)
    {
        if (count % candidate == 0)
        {
            columns = candidate;
            break;
        }
    }
    int max_rows = std::max(0, height / 2 - 2);
    if (max_rows > 0 && (count + columns - 1) / columns > max_rows)
    {
        int needed = (count + max_rows - 1) / max_rows;
        int limit  = std::min(std::max(1, count), std::max(1, width / 6));
        for (int candidate = needed; candidate <= limit; ++candidate)
        {
            if (count % candidate == 0)
            {
                return candidate;
            }
        }
        return std::min(needed, limit);
    }
    return columns;
}

struct cpu_layout_t
{
    int width;
    int columns;
    int column_width;
    int bar_cells;
    int rows        = 0;
    int first_row   = 0;
    int table_start = 0;
};

static cpu_layout_t cpu_grid_layout(int width, int columns)
{
    int column_width = width / columns;
    return {width, columns, column_width, std::max(0, column_width - 5)};
}

static cpu_layout_t cpu_layout(int count, int width, int height)
{
    int  columns     = cpu_columns(count, width, height);
    auto layout      = cpu_grid_layout(width, columns);
    layout.rows      = std::min((count + columns - 1) / columns, std::max(0, height / 2 - 2));
    layout.first_row = layout.rows > 0 ? height - layout.rows - 1 : 0;
    return layout;
}

struct view_layout_t
{
    int          width;
    int          height;
    cpu_layout_t cpu;
    int          visible_rows;
};

static view_layout_t view_layout(int width, int height, int cpu_count, bool detail)
{
    auto cpu          = cpu_layout(detail ? cpu_count : 0, std::max(0, width - 1), height);
    int  visible_rows = cpu.rows > 0 ? std::max(0, cpu.first_row - 2) : std::max(0, height - 1);
    return {width, height, cpu, visible_rows};
}

static int table_hit(const view_layout_t& layout, int visible_lines, int x, int y)
{
    int row = y - layout.cpu.table_start;
    return x >= 0 && x < layout.width && y < layout.height && row > 0 && row < visible_lines ? row : -1;
}

static terminal_row_t cpu_grid_line(const std::vector<cpu_t>& cpus, int row, const cpu_layout_t& layout)
{
    terminal_row_t result;
    result.text.reserve(static_cast<size_t>(layout.width));
    result.colors.reserve(static_cast<size_t>(layout.columns) * 5);
    size_t base_rows  = cpus.size() / static_cast<size_t>(layout.columns);
    size_t extra_rows = cpus.size() % static_cast<size_t>(layout.columns);
    for (int column = 0; column < layout.columns; ++column)
    {
        size_t column_index = static_cast<size_t>(column);
        size_t column_rows  = base_rows + (column_index < extra_rows ? 1 : 0);
        if (static_cast<size_t>(row) >= column_rows)
        {
            break;
        }
        size_t index = column_index * base_rows + std::min(column_index, extra_rows) + static_cast<size_t>(row);
        int    start = column * layout.column_width;
        auto   bar   = cpu_line(cpus[index], layout.bar_cells);
        result.text.resize(static_cast<size_t>(start), ' ');
        for (auto span : bar.colors)
        {
            span.offset += result.text.size();
            result.colors.push_back(span);
        }
        result.text += bar.text;
    }
    return result;
}

struct view_state_t
{
    int     selected_pid         = -1;
    lwpid_t selected_tid         = -1;
    int     scroll_offset        = 0;
    int     scroll_before_filter = 0;
    int     filtered_pid         = -1;
    int     pressed_pid          = -1;
    lwpid_t pressed_tid          = -1;
    bool    toggle_on_release    = false;
};

static void clear_press(view_state_t& state)
{
    state.pressed_pid       = -1;
    state.pressed_tid       = -1;
    state.toggle_on_release = false;
}

static void clear_selection(view_state_t& state)
{
    state.selected_pid = -1;
    state.selected_tid = -1;
    clear_press(state);
}

static bool row_selected(const display_row_t& row, const view_state_t& state)
{
    return row.pid == state.selected_pid && (row.thread ? row.tid == state.selected_tid : state.selected_tid == -1);
}

static bool enter_detail_view(view_state_t& state)
{
    if (state.filtered_pid != -1 || state.selected_pid == -1)
    {
        return false;
    }
    state.scroll_before_filter = state.scroll_offset;
    state.scroll_offset        = 0;
    state.filtered_pid         = state.selected_pid;
    clear_selection(state);
    return true;
}

static int exit_detail_view(view_state_t& state)
{
    int target_pid      = state.filtered_pid;
    state.filtered_pid  = -1;
    state.scroll_offset = state.scroll_before_filter;
    clear_selection(state);
    return target_pid;
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
            snapshot_t   snapshot;
            view_state_t state;
            bool         redraw         = true;
            bool         cpu_rows_dirty = true;
            auto         next_sample    = std::chrono::steady_clock::now();

            std::vector<display_row_t>  display_output;
            std::vector<terminal_row_t> frame;
            std::vector<terminal_row_t> cpu_frame;
            auto                        refresh = [&] {
                if (!top_sample(top, snapshot, error, state.filtered_pid))
                {
                    status = 1;
                    return false;
                }
                display_output = lines(snapshot, state.filtered_pid, false);
                cpu_rows_dirty = true;
                next_sample    = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                redraw         = true;
                return true;
            };
            auto enter_detail = [&] {
                if (!enter_detail_view(state))
                {
                    return;
                }
                refresh();
            };
            auto exit_detail = [&] {
                int target_pid = exit_detail_view(state);
                if (!refresh())
                {
                    return;
                }
                state.selected_pid = top.previous.contains(target_pid) ? target_pid : -1;
                if (state.selected_pid != -1)
                {
                    auto layout = view_layout(terminal.width, terminal.height, 0, false);
                    move_selection(display_output, 0, layout.visible_rows, state.selected_pid, state.selected_tid, state.scroll_offset);
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
                    if (state.selected_pid != -1 && !std::ranges::any_of(display_output, [&](const display_row_t& row) {
                            return row_selected(row, state);
                        }))
                    {
                        clear_selection(state);
                    }
                }
                bool resized = false;
                if (!terminal_size(terminal, resized, error))
                {
                    status = 1;
                    break;
                }
                redraw                 = redraw || resized;
                auto layout            = view_layout(terminal.width, terminal.height, static_cast<int>(snapshot.cpus.size()),
                                                     state.filtered_pid != -1);
                int  total_rows        = static_cast<int>(display_output.size()) - 1;
                int  max_offset        = std::max(0, total_rows - layout.visible_rows);
                state.scroll_offset    = std::clamp(state.scroll_offset, 0, max_offset);
                int visible_line_count = std::min(layout.visible_rows, total_rows - state.scroll_offset) + 1;
                if (redraw)
                {
                    if (cpu_rows_dirty || resized)
                    {
                        cpu_frame.resize(static_cast<size_t>(layout.cpu.rows));
                        for (int row = 0; row < layout.cpu.rows; ++row)
                        {
                            cpu_frame[row] = cpu_grid_line(snapshot.cpus, row, layout.cpu);
                        }
                        cpu_rows_dirty = false;
                    }
                    frame.resize(static_cast<size_t>(layout.height));
                    for (int row = 0; row < layout.height; ++row)
                    {
                        frame[row].reverse    = false;
                        frame[row].foreground = terminal_color_t::normal;
                        frame[row].colors.clear();
                        if (row >= layout.cpu.first_row && row < layout.cpu.first_row + layout.cpu.rows)
                        {
                            frame[row] = cpu_frame[row - layout.cpu.first_row];
                            continue;
                        }
                        int table_row = row - layout.cpu.table_start;
                        if (table_row < 0 || table_row >= visible_line_count)
                        {
                            frame[row].text.clear();
                            continue;
                        }
                        int   output_row_index = table_row == 0 ? 0 : table_row + state.scroll_offset;
                        bool  row_is_selected  = table_row > 0 && row_selected(display_output[output_row_index], state);
                        auto& displayed        = display_output[output_row_index];
                        if (table_row != 0 && displayed.text.empty())
                        {
                            displayed.text = process_line(*displayed.record, displayed.thread, *displayed.command_source,
                                                          state.filtered_pid != -1, displayed.affinity_width, displayed.username_width);
                        }
                        frame[row].text       = displayed.text;
                        frame[row].foreground = row_is_selected ? terminal_color_t::blue : terminal_color_t::normal;
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
                    int table_y = table_hit(layout, visible_line_count, input.x, input.y);
                    if (input.kind == terminal_event_kind_t::mouse_press)
                    {
                        clear_press(state);
                        if (table_y != -1)
                        {
                            const auto& pressed_row  = display_output[table_y + state.scroll_offset];
                            bool        was_selected = row_selected(pressed_row, state);
                            state.pressed_pid        = pressed_row.pid;
                            state.pressed_tid        = pressed_row.tid;
                            state.toggle_on_release  = was_selected && state.filtered_pid == -1;
                            state.selected_pid       = pressed_row.pid;
                            state.selected_tid       = pressed_row.thread ? pressed_row.tid : -1;
                            redraw                   = true;
                        }
                    }
                    if (input.kind == terminal_event_kind_t::mouse_release)
                    {
                        if (state.toggle_on_release && table_y != -1)
                        {
                            const auto& released_row = display_output[table_y + state.scroll_offset];
                            if (released_row.pid == state.pressed_pid && released_row.tid == state.pressed_tid && row_selected(released_row, state))
                            {
                                enter_detail();
                            }
                        }
                        clear_press(state);
                    }
                }
                else if (input.kind == terminal_event_kind_t::escape || input.kind == terminal_event_kind_t::mouse_right_press)
                {
                    if (state.selected_pid == -1 && state.filtered_pid != -1)
                    {
                        exit_detail();
                    }
                    else
                    {
                        clear_selection(state);
                        redraw = true;
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
                    if ((input.kind == terminal_event_kind_t::wheel_up || input.kind == terminal_event_kind_t::wheel_down) &&
                        state.selected_pid == -1)
                    {
                        scroll_view(display_output, direction, layout.visible_rows, state.scroll_offset);
                    }
                    else
                    {
                        move_selection(display_output, direction, layout.visible_rows, state.selected_pid, state.selected_tid, state.scroll_offset);
                    }
                    redraw = true;
                }
                else if (input.kind == terminal_event_kind_t::character && input.character == ' ')
                {
                    if (state.filtered_pid != -1)
                    {
                        exit_detail();
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