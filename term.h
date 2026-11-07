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

#include <termios.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>
#include <signal.h>
#include <stddef.h>

enum class term_event_kind_t
{
    none,
    redraw,
    escape,
    character,
    up,
    down,
    left,
    right,
    mouse_press,
    mouse_release,
    mouse_right_press,
    wheel_up,
    wheel_down
};

struct term_event_t
{
    term_event_kind_t kind      = term_event_kind_t::none;
    char              character = 0;
    int               x         = 0;
    int               y         = 0;
};

enum class term_color_t
{
    normal = 39,
    black  = 30,
    red,
    green,
    yellow,
    blue,
    magenta,
    cyan,
    white,
    bright_black = 90,
    bright_red,
    bright_green,
    bright_yellow,
    bright_blue,
    bright_magenta,
    bright_cyan,
    bright_white
};

struct term_color_span_t
{
    size_t       offset = 0;
    term_color_t color  = term_color_t::normal;

    bool operator==(const term_color_span_t&) const = default;
};

struct term_row_t
{
    std::string                    text;
    bool                           reverse    = false;
    term_color_t                   foreground = term_color_t::normal;
    std::vector<term_color_span_t> colors{};
    std::vector<term_color_t>      right_bar{};
};

struct term_t
{
    termios                               saved_mode{};
    bool                                  mode_saved    = false;
    bool                                  screen_active = false;
    std::array<struct sigaction, 6>       saved_signals{};
    size_t                                signal_count = 0;
    int                                   height       = 24;
    int                                   width        = 80;
    std::vector<term_row_t>               previous;
    std::vector<term_row_t>               frame;
    std::string                           output;
    bool                                  synchronized_output = false;
    bool                                  sync_query_pending  = false;
    std::string                           input;
    std::chrono::steady_clock::time_point partial_since{};
    bool                                  discard_sequence = false;
};

bool term_open(term_t& terminal, std::string& error);
bool term_close(term_t& terminal, std::string& error);
bool term_size(term_t& terminal, bool& changed, std::string& error);
bool term_render(term_t& terminal, const std::vector<term_row_t>& rows, std::string& error);
bool term_read(term_t& terminal, term_event_t& event, int timeout_ms, std::string& error);
bool term_stopped();
