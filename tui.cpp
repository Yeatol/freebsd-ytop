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

#include "tui.h"
#include "top.h"

#include <sys/ioctl.h>

#include <curses.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <stdio.h>
#include <cerrno>
#include <chrono>
#include <format>
#include <string>
#include <vector>
#include <cstring>
#include <stddef.h>
#include <algorithm>
#include <unordered_map>

static volatile sig_atomic_t stopped = 0;

static void stop(int)
{
    stopped = 1;
}

static bool screen_open(bool& active, int& cursor_visibility)
{
    active = initscr() != nullptr;
    if (!active)
    {
        return false;
    }
    if (cbreak() == ERR || noecho() == ERR || keypad(stdscr, TRUE) == ERR)
    {
        return false;
    }
    cursor_visibility = curs_set(0);
    if (cursor_visibility == ERR)
    {
        return false;
    }
    mouseinterval(0);
    mmask_t events = BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON4_PRESSED | BUTTON5_PRESSED;
    return (mousemask(events, nullptr) & events) == events;
}

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
    std::string text;
    int         pid     = -1;
    lwpid_t     tid     = -1;
    bool        thread  = false;
    int         threads = 0;
};

static std::string process_line(const process_t& p, bool thread, const process_t& command_source, bool show_context_switches = false)
{
    std::string pid     = thread ? "  ->" : std::to_string(p.pid);
    std::string cpu     = p.cpu < 0 ? "-" : std::to_string(p.cpu);
    std::string threads = thread ? "-" : std::to_string(p.threads);
    std::string row     = std::format(
        "{:>5} {:>6} {:<10} {:>4} {:>3} {:>4} {:>7} {:>6} {:>6} "
        "{:<6} {:>3} {:>6} {:>6.2f}% ",
        pid, p.tid, p.username.substr(0, 10), threads,
        p.priority, p.nice, memory(p.size_kb), memory(p.resident_kb),
        memory(p.swap_kb), p.state.substr(0, 6), cpu,
        runtime(p.runtime_us), p.cpu_percent);
    for (const auto& percent : {p.user_cpu_percent, p.system_cpu_percent})
    {
        if (percent.has_value())
        {
            row += std::format("{:>6.2f}% ", *percent);
        }
        else
        {
            row += std::format("{:>7} ", "-");
        }
    }
    if (p.jid > 0)
    {
        row += std::format("[jail:{}] ", p.jid);
    }
    if (!command_source.affinity.empty())
    {
        row += std::format("{} ", command_source.affinity);
    }
    row += command_source.command;
    if (thread || show_context_switches)
    {
        row += std::format(" (nivcsw={}, nvcsw={})", command_source.nivcsw, command_source.nvcsw);
    }
    return row;
}

static std::vector<display_row_t> lines(const snapshot_t& snapshot, const std::unordered_map<int, std::chrono::steady_clock::time_point>& expanded)
{
    std::vector<display_row_t> result;
    result.reserve(1 + snapshot.processes.size());
    result.push_back({"  PID    TID USERNAME    THR PRI NICE    SIZE    RES   SWAP STATE    C   TIME     CPU CPU_USR CPU_SYS COMMAND"});
    for (const auto& p : snapshot.processes)
    {
        auto expanded_process = expanded.find(p.pid);
        if (expanded_process == expanded.end())
        {
            result.push_back({process_line(p, false, p), p.pid, p.tid, false, p.threads});
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
        result.push_back({process_line(p, false, *command_source, true), p.pid, p.tid, false, p.threads});
        for (auto thread = thread_begin; thread != thread_end; ++thread)
        {
            if (thread->tid != p.tid)
            {
                result.push_back({process_line(*thread, true, *thread), thread->pid, thread->tid, true, 1});
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

static void toggle_threads(std::unordered_map<int, std::chrono::steady_clock::time_point>& expanded, int pid, std::chrono::steady_clock::time_point first_seen)
{
    if (expanded.contains(pid))
    {
        expanded.erase(pid);
    }
    else
    {
        expanded.emplace(pid, first_seen);
    }
}

int tui(top_t& top)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        fprintf(stderr, "ytop: an interactive terminal is required\n");
        return 1;
    }
    stopped = 0;
    std::string error;
    int         status = 0;
    {
        bool screen_active     = false;
        int  cursor_visibility = ERR;
        if (!screen_open(screen_active, cursor_visibility))
        {
            error  = "cannot initialize terminal with mouse support and hidden cursor";
            status = 1;
        }
        else
        {
            struct sigaction action{};
            action.sa_handler = stop;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGINT, &action, nullptr) == -1 || sigaction(SIGTERM, &action, nullptr) == -1 || sigaction(SIGHUP, &action, nullptr) == -1)
            {
                error  = std::format("sigaction: {}", std::strerror(errno));
                status = 1;
            }
            snapshot_t snapshot;
            int        selected_pid      = -1;
            lwpid_t    selected_tid      = -1;
            int        scroll_offset     = 0;
            int        pressed_pid       = -1;
            lwpid_t    pressed_tid       = -1;
            bool       toggle_on_release = false;
            bool       redraw            = true;
            auto       next_sample       = std::chrono::steady_clock::now();

            std::vector<display_row_t>                                     output;
            std::unordered_map<int, std::chrono::steady_clock::time_point> expanded;

            while (status == 0 && !stopped)
            {
                if (std::chrono::steady_clock::now() >= next_sample)
                {
                    if (!top_sample(top, snapshot, error))
                    {
                        status = 1;
                        break;
                    }
                    if (!top.previous.contains(selected_pid))
                    {
                        selected_pid = -1;
                    }
                    for (auto it = expanded.begin(); it != expanded.end();)
                    {
                        auto process = top.previous.find(it->first);
                        if (process == top.previous.end() || process->second.first_seen != it->second)
                        {
                            it = expanded.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                    output      = lines(snapshot, expanded);
                    next_sample = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    redraw      = true;
                }
                winsize size{};
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1)
                {
                    error  = std::format("reading terminal size: {}", std::strerror(errno));
                    status = 1;
                    break;
                }
                if (size.ws_row != 0 && size.ws_col != 0 && is_term_resized(size.ws_row, size.ws_col))
                {
                    if (resizeterm(size.ws_row, size.ws_col) == ERR)
                    {
                        error  = "cannot resize terminal";
                        status = 1;
                        break;
                    }
                    redraw = true;
                }
                int height = 0;
                int width  = 0;
                getmaxyx(stdscr, height, width);
                int total_rows         = static_cast<int>(output.size()) - 1;
                int visible_rows       = std::max(0, height - 1);
                int max_offset         = std::max(0, total_rows - visible_rows);
                scroll_offset          = std::clamp(scroll_offset, 0, max_offset);
                int visible_line_count = std::min(visible_rows, total_rows - scroll_offset) + 1;
                if (redraw)
                {
                    erase();
                    for (int row = 0; row < visible_line_count; ++row)
                    {
                        int  output_row_index = row == 0 ? 0 : row + scroll_offset;
                        bool row_is_selected  = row > 0 && output[output_row_index].pid == selected_pid && (output[output_row_index].thread ? output[output_row_index].tid == selected_tid : selected_tid == -1);
                        if (row == 0 || row_is_selected)
                        {
                            attron(A_REVERSE);
                        }
                        if (row == 0 || row_is_selected)
                        {
                            mvhline(row, 0, ' ', width);
                        }
                        mvaddnstr(row, 0, output[output_row_index].text.c_str(), std::max(0, width - 1));
                        if (row == 0 || row_is_selected)
                        {
                            attroff(A_REVERSE);
                        }
                    }
                    if (refresh() == ERR)
                    {
                        error  = "cannot refresh terminal";
                        status = 1;
                        break;
                    }
                    redraw = false;
                }
                pollfd input_descriptor{.fd = STDIN_FILENO, .events = POLLIN};
                int    input_ready = poll(&input_descriptor, 1, 100);
                if (input_ready == -1 && errno != EINTR)
                {
                    error  = std::format("waiting for terminal input: {}", std::strerror(errno));
                    status = 1;
                    break;
                }
                int input_event = ERR;
                if (input_ready > 0 && (input_descriptor.revents & POLLIN))
                {
                    timeout(100);
                    input_event = getch();
                }
                if (stopped || input_event == 'q')
                {
                    break;
                }
                if (input_event == KEY_MOUSE)
                {
                    MEVENT mouse{};
                    if (getmouse(&mouse) == ERR)
                    {
                        // KEY_MOUSE may have no decoded report after an incomplete escape sequence.
                        continue;
                    }
                    if (mouse.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED))
                    {
                        int scroll_delta = (mouse.bstate & BUTTON4_PRESSED) ? -3 : 3;
                        scroll_offset    = std::clamp(scroll_offset + scroll_delta, 0, max_offset);
                        redraw           = true;
                    }
                    if (mouse.bstate & BUTTON1_PRESSED)
                    {
                        pressed_pid       = -1;
                        pressed_tid       = -1;
                        toggle_on_release = false;
                        if (mouse.y > 0 && mouse.y < visible_line_count && mouse.x >= 0 && mouse.x < width)
                        {
                            const auto& pressed_row  = output[mouse.y + scroll_offset];
                            bool        was_selected = selected_pid == pressed_row.pid && (pressed_row.thread ? selected_tid == pressed_row.tid : selected_tid == -1);
                            pressed_pid              = pressed_row.pid;
                            pressed_tid              = pressed_row.tid;
                            toggle_on_release        = was_selected && !pressed_row.thread && pressed_row.threads > 0;
                            selected_pid             = pressed_row.pid;
                            selected_tid             = pressed_row.thread ? pressed_row.tid : -1;
                            redraw                   = true;
                        }
                    }
                    if (mouse.bstate & BUTTON1_RELEASED)
                    {
                        if (toggle_on_release && mouse.y > 0 && mouse.y < visible_line_count && mouse.x >= 0 && mouse.x < width)
                        {
                            const auto& released_row = output[mouse.y + scroll_offset];
                            if (!released_row.thread && released_row.pid == pressed_pid && released_row.tid == pressed_tid && selected_pid == released_row.pid && selected_tid == -1)
                            {
                                toggle_threads(expanded, released_row.pid, top.previous.at(released_row.pid).first_seen);
                                output = lines(snapshot, expanded);
                                redraw = true;
                            }
                        }
                        pressed_pid       = -1;
                        pressed_tid       = -1;
                        toggle_on_release = false;
                    }
                }
                else if (input_event == KEY_UP || input_event == KEY_DOWN)
                {
                    move_selection(output, input_event == KEY_UP ? -1 : 1, visible_rows, selected_pid, selected_tid, scroll_offset);
                    redraw = true;
                }
                else if (input_event == ' ' && selected_tid == -1)
                {
                    auto expandable_process = std::ranges::find_if(snapshot.processes, [&](const process_t& p) { return p.pid == selected_pid && p.threads > 0; });
                    if (expandable_process != snapshot.processes.end())
                    {
                        toggle_threads(expanded, selected_pid, top.previous.at(selected_pid).first_seen);
                        output = lines(snapshot, expanded);
                        redraw = true;
                    }
                }
                else if (input_event == KEY_RESIZE)
                {
                    redraw = true;
                }
            }
        }
        if (screen_active)
        {
            mousemask(0, nullptr);
            if (attrset(A_NORMAL) == ERR || move(getmaxy(stdscr) - 1, 0) == ERR || clrtoeol() == ERR || refresh() == ERR)
            {
                if (error.empty())
                {
                    error = "cannot clear terminal exit line";
                }
                status = 1;
            }
            if (cursor_visibility != ERR && curs_set(cursor_visibility) == ERR)
            {
                if (error.empty())
                {
                    error = "cannot restore cursor visibility";
                }
                status = 1;
            }
            if (endwin() == ERR)
            {
                if (error.empty())
                {
                    error = "cannot restore terminal";
                }
                status = 1;
            }
        }
    }
    if (!error.empty())
    {
        fprintf(stderr, "ytop: %s\n", error.c_str());
    }
    return status;
}