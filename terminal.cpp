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

#include "terminal.h"

#include <poll.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cwchar>
#include <format>
#include <string>
#include <vector>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <string_view>
#include <system_error>

static volatile sig_atomic_t stopped           = 0;
static volatile sig_atomic_t suspend_requested = 0;
static constexpr std::array  signals           = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH, SIGTSTP};
static constexpr auto        escape_timeout    = std::chrono::milliseconds(100);

static void terminal_signal(int signal)
{
    if (signal == SIGTSTP)
    {
        suspend_requested = 1;
    }
    else if (signal != SIGWINCH)
    {
        stopped = 1;
    }
}

static bool terminal_error(std::string& error, const char* operation)
{
    error = std::format("{}: {}", operation, std::strerror(errno));
    return false;
}

static bool write_output(std::string_view output, std::string& error)
{
    while (!output.empty())
    {
        ssize_t written = write(STDOUT_FILENO, output.data(), output.size());
        if (written == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return terminal_error(error, "writing terminal output");
        }
        if (written == 0)
        {
            error = "writing terminal output: no progress";
            return false;
        }
        output.remove_prefix(static_cast<size_t>(written));
    }
    return true;
}

static bool enter_screen(terminal_t& terminal, std::string& error)
{
    termios mode = terminal.saved_mode;
    cfmakeraw(&mode);
    mode.c_lflag |= ISIG;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) == -1)
    {
        return terminal_error(error, "setting terminal mode");
    }
    terminal.screen_active       = true;
    terminal.synchronized_output = false;
    terminal.sync_query_pending  = true;
    // Save private modes where supported; close also supplies conventional defaults.
    return write_output("\033[?1049h\033[?25s\033[?7s\033[?1000s\033[?1006s"
                        "\033[?25l\033[?7l\033[?1000h\033[?1006h\033[0m\033[2J\033[H\033[?2026$p",
                        error);
}

static bool leave_screen(terminal_t& terminal, std::string& error)
{
    bool success = true;
    if (terminal.screen_active)
    {
        std::string restore;
        if (terminal.synchronized_output)
        {
            restore = "\033[?2026l";
        }
        restore += std::format(
            "\033[0m\033[?1006l\033[?1000l\033[?1006r\033[?1000r"
            "\033[{};1H\033[2K\033[?7h\033[?7r\033[?25h\033[?25r\033[?1049l",
            terminal.height);
        success                = write_output(restore, error);
        terminal.screen_active = false;
    }
    if (terminal.mode_saved && tcsetattr(STDIN_FILENO, TCSANOW, &terminal.saved_mode) == -1)
    {
        std::string mode_error;
        terminal_error(mode_error, "restoring terminal mode");
        if (!error.empty())
        {
            error += "; ";
        }
        error += mode_error;
        success = false;
    }
    return success;
}

bool terminal_open(terminal_t& terminal, std::string& error)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        error = "an interactive terminal is required";
        return false;
    }
    const char* type = std::getenv("TERM");
    if (type == nullptr || *type == '\0' || std::strcmp(type, "dumb") == 0)
    {
        error = "a VT-compatible terminal is required (check TERM)";
        return false;
    }
    if (std::setlocale(LC_CTYPE, "") == nullptr)
    {
        error = "cannot initialize terminal character locale";
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &terminal.saved_mode) == -1)
    {
        return terminal_error(error, "reading terminal mode");
    }
    terminal.mode_saved = true;
    stopped             = 0;
    suspend_requested   = 0;
    struct sigaction action{};
    action.sa_handler = terminal_signal;
    sigemptyset(&action.sa_mask);
    for (int signal : signals)
    {
        if (sigaction(signal, &action, &terminal.saved_signals[terminal.signal_count]) == -1)
        {
            return terminal_error(error, "installing terminal signal handler");
        }
        ++terminal.signal_count;
    }
    bool changed = false;
    return terminal_size(terminal, changed, error) && enter_screen(terminal, error);
}

bool terminal_close(terminal_t& terminal, std::string& error)
{
    bool success        = leave_screen(terminal, error);
    terminal.mode_saved = false;
    while (terminal.signal_count != 0)
    {
        --terminal.signal_count;
        if (sigaction(signals[terminal.signal_count], &terminal.saved_signals[terminal.signal_count], nullptr) == -1)
        {
            std::string signal_error;
            terminal_error(signal_error, "restoring terminal signal handler");
            if (!error.empty())
            {
                error += "; ";
            }
            error += signal_error;
            success = false;
        }
    }
    terminal.previous.clear();
    terminal.frame.clear();
    terminal.output.clear();
    terminal.synchronized_output = false;
    terminal.sync_query_pending  = false;
    terminal.input.clear();
    terminal.partial_since    = {};
    terminal.discard_sequence = false;
    return success;
}

bool terminal_size(terminal_t& terminal, bool& changed, std::string& error)
{
    winsize size{};
    changed = false;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1)
    {
        return terminal_error(error, "reading terminal size");
    }
    if (size.ws_row != 0 && size.ws_col != 0 &&
        (terminal.height != size.ws_row || terminal.width != size.ws_col))
    {
        terminal.height = size.ws_row;
        terminal.width  = size.ws_col;
        terminal.previous.clear();
        changed = true;
    }
    return true;
}

static bool ascii_text(std::string_view text)
{
    return std::ranges::all_of(text, [](unsigned char ch) { return ch >= 32 && ch < 127; });
}

static bool display_text(std::string_view text, int width, bool reverse, std::string& result)
{
    result.clear();
    if (ascii_text(text))
    {
        result.assign(text.substr(0, static_cast<size_t>(width)));
        if (reverse)
        {
            result.resize(static_cast<size_t>(width), ' ');
        }
        return true;
    }
    mbstate_t state{};
    int       columns = 0;
    for (size_t offset = 0; offset < text.size();)
    {
        wchar_t character = 0;
        size_t  length    = std::mbrtowc(&character, text.data() + offset, text.size() - offset, &state);
        int     cells     = -1;
        if (length == static_cast<size_t>(-1) || length == static_cast<size_t>(-2))
        {
            state     = {};
            length    = 1;
            character = 0;
        }
        else
        {
            length = std::max<size_t>(1, length);
            if (character >= 32 && !(character >= 127 && character < 160))
            {
                cells = wcwidth(character);
            }
        }
        if (cells < 0)
        {
            cells = 1;
        }
        if (columns + cells > width)
        {
            break;
        }
        if (character < 32 || (character >= 127 && character < 160) || wcwidth(character) < 0)
        {
            result += '?';
        }
        else if (cells != 0 || columns != 0)
        {
            result.append(text.substr(offset, length));
        }
        columns += cells;
        offset += length;
    }
    if (reverse)
    {
        result.append(static_cast<size_t>(width - columns), ' ');
    }
    return ascii_text(result);
}

static void move_cursor(std::string& output, int row, int column, int& cursor_row, int& cursor_column)
{
    char  buffer[32];
    char* end = buffer;
    *end++    = '\033';
    *end++    = '[';
    end       = std::to_chars(end, buffer + sizeof(buffer), row + 1).ptr;
    *end++    = ';';
    end       = std::to_chars(end, buffer + sizeof(buffer), column + 1).ptr;
    *end++    = 'H';
    std::string_view movement(buffer, static_cast<size_t>(end - buffer));
    char             relative[32];
    char*            relative_end = relative;
    if (row == cursor_row && column == 0)
    {
        *relative_end++ = '\r';
    }
    else if (row == cursor_row && column > cursor_column)
    {
        *relative_end++ = '\033';
        *relative_end++ = '[';
        relative_end    = std::to_chars(relative_end, relative + sizeof(relative), column - cursor_column).ptr;
        *relative_end++ = 'C';
    }
    else if (row == cursor_row + 1 && column == 0)
    {
        std::memcpy(relative_end, "\033[B\r", 4);
        relative_end += 4;
    }
    if (row != cursor_row || column != cursor_column)
    {
        if (relative_end != relative && static_cast<size_t>(relative_end - relative) < movement.size())
        {
            movement = {relative, static_cast<size_t>(relative_end - relative)};
        }
        output.append(movement);
    }
    cursor_row    = row;
    cursor_column = column;
}

static char cell(std::string_view text, size_t column)
{
    return column < text.size() ? text[column] : ' ';
}

static void set_foreground(std::string& output, terminal_color_t color, terminal_color_t& current, bool reverse)
{
    if (color != current)
    {
        // Xshell needs a full reset when leaving a bright foreground.
        output += "\033[0m\033[";
        output += std::to_string(static_cast<int>(color));
        output += 'm';
        if (reverse)
        {
            output += "\033[7m";
        }
        current = color;
    }
}

static bool sparse_color_changes(const terminal_row_t& current, const terminal_row_t& previous)
{
    if (current.colors.size() != previous.colors.size())
    {
        return true;
    }
    int changes = 0;
    for (size_t i = 0; i < current.colors.size(); ++i)
    {
        if (current.colors[i] != previous.colors[i] && ++changes > 3)
        {
            // Dense updates are cheaper as one row than many cursor/style changes.
            return false;
        }
    }
    return true;
}

bool terminal_render(terminal_t& terminal, const std::vector<terminal_row_t>& rows, std::string& error)
{
    auto& frame  = terminal.frame;
    auto& output = terminal.output;
    frame.resize(static_cast<size_t>(terminal.height));
    output.clear();
    mbstate_t   block_state{};
    wchar_t     block_character = 0;
    const bool  unicode_block   = std::mbrtowc(&block_character, "\u2588", 3, &block_state) == 3 &&
                                  block_character == L'\u2588' && wcwidth(block_character) == 1;
    const char* bar_character   = unicode_block ? "\u2588" : "-";
    if (terminal.synchronized_output)
    {
        output += "\033[?2026h";
    }
    size_t start         = output.size();
    int    cursor_row    = -2;
    int    cursor_column = -1;
    bool   reverse       = false;
    auto   foreground    = terminal_color_t::normal;
    for (int row = 0; row < terminal.height; ++row)
    {
        auto& line      = frame[row];
        bool  ascii     = true;
        line.reverse    = false;
        line.foreground = terminal_color_t::normal;
        line.colors.clear();
        line.right_bar.clear();
        line.text.clear();
        if (static_cast<size_t>(row) < rows.size())
        {
            line.reverse    = rows[row].reverse;
            line.foreground = rows[row].foreground;
            line.colors     = rows[row].colors;
            line.right_bar  = rows[row].right_bar;
            if (line.right_bar.size() > static_cast<size_t>(terminal.width))
            {
                error = "right CPU bars exceed terminal width";
                return false;
            }
            ascii = display_text(rows[row].text, terminal.width - static_cast<int>(line.right_bar.size()),
                                 line.reverse || !line.right_bar.empty(), line.text);
            line.text.append(line.right_bar.size(), ' ');
            if (!line.colors.empty() && (!ascii || !std::ranges::is_sorted(line.colors, {}, &terminal_color_span_t::offset)))
            {
                error = "color spans require sanitized ASCII text and ordered byte offsets";
                return false;
            }
        }
        const auto* previous = static_cast<size_t>(row) < terminal.previous.size() ? &terminal.previous[row] : nullptr;
        if (previous != nullptr && line.text == previous->text && line.reverse == previous->reverse &&
            line.foreground == previous->foreground && line.colors == previous->colors && line.right_bar == previous->right_bar)
        {
            continue;
        }
        if (line.reverse != reverse)
        {
            output += line.reverse ? "\033[7m" : "\033[0m";
            reverse = line.reverse;
            if (!reverse)
            {
                foreground = terminal_color_t::normal;
            }
        }
        set_foreground(output, line.foreground, foreground, reverse);
        if (previous != nullptr && ascii &&
            line.text.size() == previous->text.size() && line.reverse == previous->reverse &&
            line.foreground == previous->foreground && line.right_bar.empty() && previous->right_bar.empty() &&
            (!line.colors.empty() || !previous->colors.empty()) && sparse_color_changes(line, *previous) &&
            (!previous->colors.empty() || ascii_text(previous->text)))
        {
            size_t current_span   = 0;
            size_t previous_span  = 0;
            auto   current_color  = line.foreground;
            auto   previous_color = previous->foreground;
            size_t begin          = 0;
            bool   writing        = false;
            auto   run_color      = line.foreground;
            bool   same_text      = line.text == previous->text;
            auto   flush          = [&](size_t end) {
                if (writing)
                {
                    move_cursor(output, row, static_cast<int>(begin), cursor_row, cursor_column);
                    set_foreground(output, run_color, foreground, reverse);
                    output.append(line.text, begin, end - begin);
                    cursor_column = std::min(terminal.width - 1, static_cast<int>(end));
                    writing       = false;
                }
            };
            for (size_t column = 0; column < line.text.size();)
            {
                while (current_span < line.colors.size() && line.colors[current_span].offset <= column)
                {
                    current_color = line.colors[current_span++].color;
                }
                while (previous_span < previous->colors.size() && previous->colors[previous_span].offset <= column)
                {
                    previous_color = previous->colors[previous_span++].color;
                }
                bool changed = current_color != previous_color || (!same_text && line.text[column] != previous->text[column]);
                if (!changed || (writing && current_color != run_color))
                {
                    flush(column);
                }
                if (changed && !writing)
                {
                    begin     = column;
                    run_color = current_color;
                    writing   = true;
                }
                if (same_text)
                {
                    size_t current_end  = current_span < line.colors.size() ? line.colors[current_span].offset : line.text.size();
                    size_t previous_end = previous_span < previous->colors.size() ? previous->colors[previous_span].offset : line.text.size();
                    column              = std::min({current_end, previous_end, line.text.size()});
                }
                else
                {
                    ++column;
                }
            }
            flush(line.text.size());
            set_foreground(output, line.foreground, foreground, reverse);
            continue;
        }
        if (previous == nullptr || line.reverse != previous->reverse ||
            line.foreground != previous->foreground || line.colors != previous->colors || !line.colors.empty() ||
            !line.right_bar.empty() || line.right_bar != previous->right_bar ||
            !ascii || !ascii_text(previous->text))
        {
            move_cursor(output, row, 0, cursor_row, cursor_column);
            if (!ascii)
            {
                output += "\033[2K";
            }
            size_t offset = 0;
            for (const auto& span : line.colors)
            {
                if (span.offset >= line.text.size())
                {
                    break;
                }
                output.append(line.text, offset, span.offset - offset);
                set_foreground(output, span.color, foreground, reverse);
                offset = span.offset;
            }
            if (!line.right_bar.empty())
            {
                output.append(line.text, offset, line.text.size() - offset - line.right_bar.size());
                for (auto color : line.right_bar)
                {
                    set_foreground(output, color, foreground, reverse);
                    output += color == terminal_color_t::normal ? " " : bar_character;
                }
            }
            else
            {
                output.append(line.text, offset);
            }
            set_foreground(output, line.foreground, foreground, reverse);
            if (ascii && line.text.size() < static_cast<size_t>(terminal.width))
            {
                output += "\033[K";
            }
            // Multibyte text has display widths different from its byte count.
            cursor_column = ascii ? std::min(terminal.width - 1, static_cast<int>(line.text.size())) : -1;
            continue;
        }
        size_t limit  = std::max(line.text.size(), previous->text.size());
        size_t column = 0;
        while (column < limit)
        {
            if (cell(line.text, column) == cell(previous->text, column))
            {
                ++column;
                continue;
            }
            size_t begin = column++;
            size_t end   = column;
            // Rewriting short unchanged gaps is cheaper than another cursor command.
            while (column < limit && column - end < 8)
            {
                if (cell(line.text, column) != cell(previous->text, column))
                {
                    end = column + 1;
                }
                ++column;
            }
            move_cursor(output, row, static_cast<int>(begin), cursor_row, cursor_column);
            if (!line.reverse && begin >= line.text.size())
            {
                output += "\033[K";
                break;
            }
            size_t text_end = std::min(end, line.text.size());
            if (text_end > begin)
            {
                output.append(line.text, begin, text_end - begin);
            }
            if (end > text_end)
            {
                output.append(end - text_end, ' ');
            }
            cursor_column = std::min(terminal.width - 1, static_cast<int>(end));
            column        = end;
        }
    }
    if (output.size() != start)
    {
        if (reverse || foreground != terminal_color_t::normal)
        {
            output += "\033[0m";
        }
        if (terminal.synchronized_output)
        {
            output += "\033[?2026l";
        }
        if (!write_output(output, error))
        {
            return false;
        }
    }
    else
    {
        output.clear();
    }
    terminal.previous.swap(frame);
    return true;
}

static void mouse_event(terminal_event_t& event, int button, int x, int y, bool released)
{
    event.x = x;
    event.y = y;
    button &= ~28;
    if (button == 64 || button == 65)
    {
        if (!released)
        {
            event.kind = button == 64 ? terminal_event_kind_t::wheel_up : terminal_event_kind_t::wheel_down;
        }
    }
    else if (button == 2 && !released)
    {
        event.kind = terminal_event_kind_t::mouse_right_press;
    }
    else if (button == 0 || button == 3)
    {
        event.kind = released || button == 3 ? terminal_event_kind_t::mouse_release : terminal_event_kind_t::mouse_press;
    }
}

static void sgr_mouse(std::string_view parameters, bool released, terminal_event_t& event)
{
    std::array<int, 3> values{};
    for (size_t i = 0; i < values.size(); ++i)
    {
        auto end    = parameters.find(';');
        auto field  = parameters.substr(0, end);
        auto parsed = std::from_chars(field.data(), field.data() + field.size(), values[i]);
        if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || values[i] < 0)
        {
            return;
        }
        if (i + 1 < values.size())
        {
            if (end == std::string_view::npos)
            {
                return;
            }
            parameters.remove_prefix(end + 1);
        }
        else if (end != std::string_view::npos)
        {
            return;
        }
    }
    if (values[1] > 0 && values[2] > 0)
    {
        mouse_event(event, values[0], values[1] - 1, values[2] - 1, released);
    }
}

static bool decode_input(terminal_t& terminal, terminal_event_t& event)
{
    while (!terminal.input.empty())
    {
        size_t consumed = 1;
        if (terminal.discard_sequence)
        {
            if (terminal.input.front() == '\033')
            {
                terminal.discard_sequence = false;
                continue;
            }
            unsigned char character   = terminal.input.front();
            terminal.discard_sequence = character < 0x40 || character > 0x7e;
        }
        else if (terminal.input.front() != '\033')
        {
            event.kind      = terminal_event_kind_t::character;
            event.character = terminal.input.front();
        }
        else
        {
            if (terminal.input.size() < 2)
            {
                return false;
            }
            if (terminal.input[1] == '\033')
            {
                event.kind = terminal_event_kind_t::escape;
            }
            else if (terminal.input[1] == '[' && terminal.input.size() >= 3 && terminal.input[2] == 'M')
            {
                auto next_escape = terminal.input.find('\033', 3);
                if (next_escape < 6)
                {
                    terminal.input.erase(0, next_escape);
                    terminal.partial_since = {};
                    continue;
                }
                if (terminal.input.size() < 6)
                {
                    return false;
                }
                consumed   = 6;
                int button = static_cast<unsigned char>(terminal.input[3]) - 32;
                int x      = static_cast<unsigned char>(terminal.input[4]) - 33;
                int y      = static_cast<unsigned char>(terminal.input[5]) - 33;
                if (button >= 0 && x >= 0 && y >= 0)
                {
                    mouse_event(event, button, x, y, false);
                }
            }
            else if (terminal.input[1] == '[' || terminal.input[1] == 'O')
            {
                size_t end = 2;
                while (end < terminal.input.size() &&
                       terminal.input[end] != '\033' &&
                       (static_cast<unsigned char>(terminal.input[end]) < 0x40 ||
                        static_cast<unsigned char>(terminal.input[end]) > 0x7e))
                {
                    ++end;
                }
                if (end < terminal.input.size() && terminal.input[end] == '\033')
                {
                    terminal.input.erase(0, end);
                    terminal.partial_since = {};
                    continue;
                }
                if (end == terminal.input.size())
                {
                    if (end > 128)
                    {
                        terminal.input.clear();
                        terminal.discard_sequence = true;
                    }
                    return false;
                }
                consumed = end + 1;
                std::string_view parameters(terminal.input.data() + 2, end - 2);
                char             final = terminal.input[end];
                if (terminal.input[1] == '[' && final == 'y' && parameters.starts_with("?2026;") && parameters.ends_with('$'))
                {
                    if (terminal.sync_query_pending)
                    {
                        terminal.synchronized_output = parameters == "?2026;1$" || parameters == "?2026;2$";
                        terminal.sync_query_pending  = false;
                    }
                }
                else if (terminal.input[1] == '[' && parameters.starts_with('<') && (final == 'M' || final == 'm'))
                {
                    sgr_mouse(parameters.substr(1), final == 'm', event);
                }
                else if ((parameters.empty() || parameters == "1" || parameters.starts_with("1;")) &&
                         (final >= 'A' && final <= 'D'))
                {
                    switch (final)
                    {
                        case 'A':
                            event.kind = terminal_event_kind_t::up;
                            break;
                        case 'B':
                            event.kind = terminal_event_kind_t::down;
                            break;
                        case 'C':
                            event.kind = terminal_event_kind_t::right;
                            break;
                        case 'D':
                            event.kind = terminal_event_kind_t::left;
                            break;
                    }
                }
            }
            else
            {
                consumed = 2;
            }
        }
        terminal.input.erase(0, consumed);
        terminal.partial_since = {};
        if (event.kind != terminal_event_kind_t::none)
        {
            return true;
        }
    }
    return false;
}

bool terminal_read(terminal_t& terminal, terminal_event_t& event, int timeout_ms, std::string& error)
{
    event = {};
    if (suspend_requested && !stopped)
    {
        suspend_requested = 0;
        if (!leave_screen(terminal, error))
        {
            return false;
        }
        if (raise(SIGSTOP) != 0)
        {
            return terminal_error(error, "suspending terminal");
        }
        if (!enter_screen(terminal, error))
        {
            return false;
        }
        terminal.previous.clear();
        event.kind = terminal_event_kind_t::redraw;
        return true;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!stopped)
    {
        if (decode_input(terminal, event))
        {
            return true;
        }
        auto now        = std::chrono::steady_clock::now();
        auto wait_until = deadline;
        if (!terminal.input.empty())
        {
            if (terminal.partial_since == std::chrono::steady_clock::time_point{})
            {
                terminal.partial_since = now;
            }
            wait_until = std::min(wait_until, terminal.partial_since + escape_timeout);
            if (now >= terminal.partial_since + escape_timeout)
            {
                // Keep structured reports intact so delayed coordinates cannot become keys.
                if (terminal.input.size() == 1)
                {
                    terminal.input.clear();
                    event.kind = terminal_event_kind_t::escape;
                }
                terminal.partial_since = {};
                return true;
            }
        }
        int    wait = static_cast<int>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(wait_until - now).count()));
        pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
        int    ready = poll(&descriptor, 1, wait);
        if (ready == -1)
        {
            if (errno == EINTR)
            {
                return true;
            }
            return terminal_error(error, "waiting for terminal input");
        }
        if (ready == 0)
        {
            return true;
        }
        if (descriptor.revents & (POLLERR | POLLNVAL))
        {
            error = "terminal input is unavailable";
            return false;
        }
        if (descriptor.revents & (POLLIN | POLLHUP))
        {
            char    buffer[256];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0)
            {
                if (errno == EINTR)
                {
                    return true;
                }
                return terminal_error(error, "reading terminal input");
            }
            if (count == 0)
            {
                error = "terminal input closed";
                return false;
            }
            terminal.input.append(buffer, static_cast<size_t>(count));
        }
    }
    return true;
}

bool terminal_stopped()
{
    return stopped != 0;
}
