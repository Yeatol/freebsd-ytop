/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Yeatol <vim@vimer.org>
 */

#pragma once

#include <termios.h>
#include <signal.h>

#include <array>
#include <chrono>
#include <string>
#include <vector>

enum class terminal_event_kind_t
{
    none,
    redraw,
    character,
    up,
    down,
    mouse_press,
    mouse_release,
    wheel_up,
    wheel_down
};

struct terminal_event_t
{
    terminal_event_kind_t kind      = terminal_event_kind_t::none;
    char                  character = 0;
    int                   x         = 0;
    int                   y         = 0;
};

struct terminal_row_t
{
    std::string text;
    bool        reverse = false;
};

struct terminal_t
{
    termios                               saved_mode{};
    bool                                  mode_saved    = false;
    bool                                  screen_active = false;
    std::array<struct sigaction, 6>       saved_signals{};
    size_t                                signal_count = 0;
    int                                   height       = 24;
    int                                   width        = 80;
    std::vector<terminal_row_t>           previous;
    std::string                           input;
    std::chrono::steady_clock::time_point partial_since{};
    bool                                  discard_sequence = false;
};

bool terminal_open(terminal_t& terminal, std::string& error);
bool terminal_close(terminal_t& terminal, std::string& error);
bool terminal_size(terminal_t& terminal, bool& changed, std::string& error);
bool terminal_render(terminal_t& terminal, const std::vector<terminal_row_t>& rows, std::string& error);
bool terminal_read(terminal_t& terminal, terminal_event_t& event, int timeout_ms, std::string& error);
bool terminal_stopped();
