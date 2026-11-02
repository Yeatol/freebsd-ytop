/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Yeatol <vim@vimer.org>
 */

#include "terminal.h"

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <libutil.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

static void check(bool passed, const char* message)
{
    if (!passed)
    {
        fprintf(stderr, "FAIL: %s (%s)\n", message, std::strerror(errno));
        std::exit(1);
    }
}

static std::string drain(int master)
{
    std::string output;
    char buffer[4096];
    ssize_t count;
    while ((count = read(master, buffer, sizeof(buffer))) > 0)
    {
        output.append(buffer, static_cast<size_t>(count));
    }
    check(count == -1 && errno == EAGAIN, "drain PTY output");
    return output;
}

static void send_input(int master, std::string_view input)
{
    check(write(master, input.data(), input.size()) == static_cast<ssize_t>(input.size()), "write PTY input");
}

static terminal_event_t read_event(terminal_t& terminal)
{
    std::string error;
    terminal_event_t event;
    check(terminal_read(terminal, event, 0, error), error.c_str());
    return event;
}

static void expect_event(terminal_t& terminal, int master, std::string_view input, terminal_event_kind_t kind, int x = 0, int y = 0)
{
    send_input(master, input);
    auto event = read_event(terminal);
    check(event.kind == kind && event.x == x && event.y == y, "decoded input event");
}

static bool same_mode(const termios& first, const termios& second)
{
    return first.c_iflag == second.c_iflag && first.c_oflag == second.c_oflag &&
           first.c_cflag == second.c_cflag && first.c_lflag == second.c_lflag &&
           std::memcmp(first.c_cc, second.c_cc, sizeof(first.c_cc)) == 0 &&
           cfgetispeed(&first) == cfgetispeed(&second) && cfgetospeed(&first) == cfgetospeed(&second);
}

static void test_input(terminal_t& terminal, int master)
{
    expect_event(terminal, master, "\033[A", terminal_event_kind_t::up);
    expect_event(terminal, master, "\033OB", terminal_event_kind_t::down);
    expect_event(terminal, master, "\033[1;5A", terminal_event_kind_t::up);
    expect_event(terminal, master, "\033[<0;4;3M", terminal_event_kind_t::mouse_press, 3, 2);
    expect_event(terminal, master, "\033[<0;4;3m", terminal_event_kind_t::mouse_release, 3, 2);
    expect_event(terminal, master, "\033[<64;4;3M", terminal_event_kind_t::wheel_up, 3, 2);
    expect_event(terminal, master, "\033[<65;4;3M", terminal_event_kind_t::wheel_down, 3, 2);
    expect_event(terminal, master, "\033[M !!", terminal_event_kind_t::mouse_press);
    expect_event(terminal, master, "\033[M#!!", terminal_event_kind_t::mouse_release);
    expect_event(terminal, master, "\033[M`!!", terminal_event_kind_t::wheel_up);
    expect_event(terminal, master, "\033[<1;1;1M", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033[<32;1;1M", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033[<128;1;1M", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033[<0;0;1M", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033[<999999999999;1;1M", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033[<0;1;1;2M", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033[15~", terminal_event_kind_t::none);
    expect_event(terminal, master, "\033q", terminal_event_kind_t::none);

    expect_event(terminal, master, "\033[<0;12;", terminal_event_kind_t::none);
    expect_event(terminal, master, "4M", terminal_event_kind_t::mouse_press, 11, 3);
    expect_event(terminal, master, "\033[M ", terminal_event_kind_t::none);
    expect_event(terminal, master, "!!", terminal_event_kind_t::mouse_press);
    expect_event(terminal, master, "\033", terminal_event_kind_t::none);
    usleep(120000);
    check(read_event(terminal).kind == terminal_event_kind_t::none, "standalone Escape expires");
    check(terminal.input.empty(), "expired Escape is removed");
    expect_event(terminal, master, "\033[<0;", terminal_event_kind_t::none);
    usleep(120000);
    check(read_event(terminal).kind == terminal_event_kind_t::none, "incomplete mouse report waits without a key event");
    expect_event(terminal, master, "81;1M", terminal_event_kind_t::mouse_press, 80, 0);

    for (size_t split = 2; split < 6; ++split)
    {
        std::string report = "\033[M q!";
        expect_event(terminal, master, std::string_view(report).substr(0, split), terminal_event_kind_t::none);
        usleep(120000);
        check(read_event(terminal).kind == terminal_event_kind_t::none, "delayed X10 report waits without a key event");
        expect_event(terminal, master, std::string_view(report).substr(split), terminal_event_kind_t::mouse_press, 80, 0);
    }
    expect_event(terminal, master, "\033[M ", terminal_event_kind_t::none);
    usleep(120000);
    check(read_event(terminal).kind == terminal_event_kind_t::none, "truncated X10 report waits");
    expect_event(terminal, master, "\033[A", terminal_event_kind_t::up);
    expect_event(terminal, master, "\033[<0;", terminal_event_kind_t::none);
    usleep(120000);
    check(read_event(terminal).kind == terminal_event_kind_t::none, "truncated SGR report waits");

    send_input(master, "\033[A\033[B q");
    check(read_event(terminal).kind == terminal_event_kind_t::up, "first buffered key");
    check(read_event(terminal).kind == terminal_event_kind_t::down, "second buffered key");
    check(read_event(terminal).character == ' ', "buffered space");
    check(read_event(terminal).character == 'q', "buffered quit");
    expect_event(terminal, master, std::string("\033[") + std::string(200, '1'), terminal_event_kind_t::none);
    expect_event(terminal, master, "Mq", terminal_event_kind_t::character);
    check(!terminal.discard_sequence && terminal.input.empty(), "oversized sequence discarded through its final byte");
}

static void test_render(terminal_t& terminal, int master)
{
    std::string error;
    std::vector<terminal_row_t> rows = {{"header", true}, {"safe\033[31m"}, {"A\xe4\xb8\xad" "B"}};
    check(terminal_render(terminal, rows, error), error.c_str());
    auto output = drain(master);
    check(output.find("\033[7m") != std::string::npos && output.find("header    ") != std::string::npos, "reverse row fills width");
    check(output.find("safe?[31m") != std::string::npos && output.find("\033[31m") == std::string::npos, "control characters cannot inject terminal commands");
    check(output.find("A\xe4\xb8\xad" "B") != std::string::npos, "UTF-8 preserved");
    check(terminal_render(terminal, rows, error), error.c_str());
    check(drain(master).empty(), "unchanged frame writes nothing");
    rows.resize(1);
    check(terminal_render(terminal, rows, error), error.c_str());
    output = drain(master);
    check(output.find("\033[K") != std::string::npos, "removed rows are cleared");

    winsize size{.ws_row = 2, .ws_col = 3, .ws_xpixel = 0, .ws_ypixel = 0};
    check(ioctl(STDOUT_FILENO, TIOCSWINSZ, &size) == 0, "resize PTY");
    bool changed = false;
    check(terminal_size(terminal, changed, error) && changed && terminal.previous.empty(), "resize invalidates frame");
    rows = {{"A\xe4\xb8\xad" "B"}, {"\xe4\xb8\xad\xe4\xb8\xad", true}};
    check(terminal_render(terminal, rows, error), error.c_str());
    drain(master);
    check(terminal.previous[0].text == "A\xe4\xb8\xad", "clip by columns at UTF-8 boundary");
    check(terminal.previous[1].text == "\xe4\xb8\xad ", "pad after a wide character");
    rows = {{std::string("\xff\0\n", 3)}};
    check(terminal_render(terminal, rows, error), error.c_str());
    drain(master);
    check(terminal.previous[0].text == "???", "invalid bytes, NUL and newline are sanitized");
    rows = {{"?" "?!"}};
    check(terminal_render(terminal, rows, error), error.c_str());
    output = drain(master);
    check(output.find('!') != std::string::npos && output.find("\033[2K") == std::string::npos,
          "sanitized ASCII output remains eligible for differential updates");
    rows = {{"abc\xe4\xb8\xad"}};
    check(terminal_render(terminal, rows, error), error.c_str());
    drain(master);
    rows = {{"abd\xe4\xb8\xad"}};
    check(terminal_render(terminal, rows, error), error.c_str());
    output = drain(master);
    check(terminal.previous[0].text == "abd" && output.find('d') != std::string::npos &&
          output.find('a') == std::string::npos && output.find("\033[2K") == std::string::npos,
          "clipped non-ASCII suffix does not disable ASCII differential updates");
    rows = {{"e\xcc\x81" "ab"}};
    check(terminal_render(terminal, rows, error), error.c_str());
    drain(master);
    check(terminal.previous[0].text == "e\xcc\x81" "ab", "combining characters do not consume columns");
    size = {.ws_row = 1, .ws_col = 1, .ws_xpixel = 0, .ws_ypixel = 0};
    check(ioctl(STDOUT_FILENO, TIOCSWINSZ, &size) == 0, "resize to one cell");
    check(terminal_size(terminal, changed, error), error.c_str());
    check(terminal_render(terminal, {{"\xe4\xb8\xad", true}}, error), error.c_str());
    drain(master);
    check(terminal.previous[0].text == " ", "wide character does not overflow a one-column terminal");
}

struct test_screen_t
{
    int width;
    int height;
    int x = 0;
    int y = 0;
    bool reverse = false;
    std::string text;
    std::vector<bool> styles;
};

static void apply_output(test_screen_t& screen, std::string_view output)
{
    for (size_t i = 0; i < output.size();)
    {
        unsigned char ch = output[i++];
        if (ch == '\r')
        {
            screen.x = 0;
            continue;
        }
        if (ch != '\033')
        {
            check(ch >= 32 && ch < 127, "ASCII screen emulator receives printable text");
            size_t cell = static_cast<size_t>(screen.y * screen.width + screen.x);
            screen.text[cell] = ch;
            screen.styles[cell] = screen.reverse;
            screen.x = std::min(screen.width - 1, screen.x + 1);
            continue;
        }
        check(i < output.size() && output[i++] == '[', "complete CSI introducer");
        size_t begin = i;
        while (i < output.size() && (output[i] < '@' || output[i] > '~'))
        {
            ++i;
        }
        check(i < output.size(), "complete CSI final");
        auto parameters = output.substr(begin, i - begin);
        char final = output[i++];
        if (parameters.starts_with('?'))
        {
            check((final == 'h' || final == 'l') && parameters == "?2026", "only synchronization commands in frame");
            continue;
        }
        int first = 0;
        int second = 1;
        if (!parameters.empty())
        {
            auto result = std::from_chars(parameters.data(), parameters.data() + parameters.size(), first);
            check(result.ec == std::errc{}, "valid CSI parameter");
            if (result.ptr != parameters.data() + parameters.size())
            {
                check(*result.ptr == ';', "CSI parameter separator");
                check(std::from_chars(result.ptr + 1, parameters.data() + parameters.size(), second).ec == std::errc{}, "valid second CSI parameter");
            }
        }
        if (final == 'H')
        {
            screen.y = std::clamp(first - 1, 0, screen.height - 1);
            screen.x = std::clamp(second - 1, 0, screen.width - 1);
        }
        else if (final == 'C')
        {
            screen.x = std::min(screen.width - 1, screen.x + std::max(1, first));
        }
        else if (final == 'B')
        {
            screen.y = std::min(screen.height - 1, screen.y + std::max(1, first));
        }
        else if (final == 'K')
        {
            check(first == 0 || first == 2, "supported erase mode");
            for (int column = first == 2 ? 0 : screen.x; column < screen.width; ++column)
            {
                size_t cell = static_cast<size_t>(screen.y * screen.width + column);
                screen.text[cell] = ' ';
                screen.styles[cell] = screen.reverse;
            }
        }
        else if (final == 'm')
        {
            check(first == 0 || first == 7, "supported style");
            screen.reverse = first == 7;
        }
        else
        {
            check(false, "unexpected renderer command");
        }
    }
}

static void expect_screen(const test_screen_t& screen, const std::vector<terminal_row_t>& rows)
{
    for (int y = 0; y < screen.height; ++y)
    {
        for (int x = 0; x < screen.width; ++x)
        {
            size_t cell = static_cast<size_t>(y * screen.width + x);
            bool present = static_cast<size_t>(y) < rows.size();
            char expected = present && static_cast<size_t>(x) < rows[y].text.size() ? rows[y].text[x] : ' ';
            check(screen.text[cell] == expected, "rendered cell matches complete frame");
            check(screen.styles[cell] == (present && rows[y].reverse), "rendered style matches complete frame");
        }
    }
}

static void test_differential_render(terminal_t& terminal, int master)
{
    winsize size{.ws_row = 4, .ws_col = 120, .ws_xpixel = 0, .ws_ypixel = 0};
    check(ioctl(STDOUT_FILENO, TIOCSWINSZ, &size) == 0, "set differential test dimensions");
    std::string error;
    bool changed = false;
    check(terminal_size(terminal, changed, error), error.c_str());
    test_screen_t screen{.width = 120, .height = 4, .text = std::string(480, ' '), .styles = std::vector<bool>(480)};
    std::vector<terminal_row_t> rows(4);
    for (auto& row : rows)
    {
        row.text = std::string(120, 'a');
    }
    rows[0].reverse = true;
    auto render = [&]() {
        check(terminal_render(terminal, rows, error), error.c_str());
        auto output = drain(master);
        apply_output(screen, output);
        expect_screen(screen, rows);
        return output;
    };
    auto full = render();
    check(full.size() < 560, "full frame does not repeat per-row style resets");
    rows[1].text[60] = 'b';
    auto single = render();
    check(single.size() <= 12 && single.find('a') == std::string::npos, "single-cell change only writes that cell");
    rows[1].text[10] = 'c';
    rows[1].text[110] = 'd';
    auto separated = render();
    check(separated.size() <= 24, "distant changes use separate short spans");
    rows[1].text[20] = 'x';
    rows[1].text[23] = 'y';
    auto nearby = render();
    check(nearby.find("xaay") != std::string::npos, "nearby changes share one span");
    rows[1].text.resize(30);
    render();
    rows[1].text.clear();
    render();
    rows[2].reverse = true;
    render();
    rows[2].reverse = false;
    render();
    rows[3].text[119] = 'z';
    render();
    check(render().empty(), "unchanged differential frame emits no bytes");

    uint32_t random = 12345;
    for (int frame = 0; frame < 200; ++frame)
    {
        random = random * 1664525 + 1013904223;
        size_t row = (random >> 16) % rows.size();
        size_t length = random % 121;
        rows[row].text.resize(length, ' ');
        if (length != 0)
        {
            rows[row].text[(random >> 8) % length] = static_cast<char>('!' + random % 94);
        }
        rows[row].reverse = (random & 8) != 0;
        render();
    }

    for (auto& row : rows)
    {
        row.text.assign(120, 'x');
    }
    render();
    render();
    auto first = terminal.previous.data();
    auto second = terminal.frame.data();
    auto first_text = terminal.previous[1].text.data();
    auto second_text = terminal.frame[1].text.data();
    auto output_buffer = terminal.output.data();
    auto capacity = terminal.output.capacity();
    for (int i = 0; i < 20; ++i)
    {
        rows[1].text[60] = i % 2 == 0 ? 'y' : 'x';
        render();
        check(terminal.previous.data() == (i % 2 == 0 ? second : first), "frame arrays reused");
        check(terminal.previous[1].text.data() == (i % 2 == 0 ? second_text : first_text), "row storage reused");
        check(terminal.output.data() == output_buffer && terminal.output.capacity() == capacity, "output buffer reused");
    }
    fprintf(stderr, "120-column frame: full=%zu bytes, single-cell=%zu bytes, two distant cells=%zu bytes\n",
            full.size(), single.size(), separated.size());
}

static void test_synchronized_output(terminal_t& terminal, int master)
{
    std::string error;
    check(terminal.sync_query_pending && !terminal.synchronized_output, "synchronized output disabled until confirmed");
    expect_event(terminal, master, "\033[?2026;", terminal_event_kind_t::none);
    check(!terminal.synchronized_output, "partial capability reply does not enable mode");
    expect_event(terminal, master, "2$y", terminal_event_kind_t::none);
    check(terminal.synchronized_output && !terminal.sync_query_pending, "supported mode confirmed by terminal");
    check(terminal_render(terminal, {{"synchronized"}}, error), error.c_str());
    auto output = drain(master);
    check(output.starts_with("\033[?2026h") && output.ends_with("\033[?2026l"), "frame bounded by synchronization commands");
    check(terminal_render(terminal, {{"synchronized"}}, error), error.c_str());
    check(drain(master).empty(), "unchanged synchronized frame writes nothing");
    expect_event(terminal, master, "\033[?2026;0$y", terminal_event_kind_t::none);
    check(terminal.synchronized_output, "unsolicited reply does not change mode");
}

static void test_suspend(int master, const termios& original)
{
    winsize size{.ws_row = 4, .ws_col = 20, .ws_xpixel = 0, .ws_ypixel = 0};
    check(ioctl(STDOUT_FILENO, TIOCSWINSZ, &size) == 0, "set suspension test dimensions");
    pid_t child = fork();
    check(child >= 0, "fork suspension test");
    if (child == 0)
    {
        terminal_t terminal;
        std::string error;
        check(terminal_open(terminal, error), error.c_str());
        check(terminal_render(terminal, {{"before suspend"}}, error), error.c_str());
        check(!terminal.previous.empty(), "frame cached before suspension");
        check(raise(SIGTSTP) == 0, "request suspension");
        terminal_event_t event;
        check(terminal_read(terminal, event, 0, error), error.c_str());
        check(event.kind == terminal_event_kind_t::redraw, "resume requests an immediate redraw");
        check(terminal.previous.empty(), "resume invalidates the previous frame");
        check(terminal_render(terminal, {{"resumed"}}, error), error.c_str());
        termios mode{};
        check(tcgetattr(STDIN_FILENO, &mode) == 0 && !(mode.c_lflag & (ICANON | ECHO)), "raw mode restored after continue");
        check(terminal_close(terminal, error), error.c_str());
        _exit(0);
    }
    int status = 0;
    check(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status), "terminal process suspended");
    termios mode{};
    check(tcgetattr(STDIN_FILENO, &mode) == 0 && same_mode(mode, original), "terminal restored before suspension");
    drain(master);
    check(kill(child, SIGCONT) == 0, "continue terminal process");
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "terminal resumes and exits cleanly");
    check(drain(master).find("resumed") != std::string::npos, "redraw reaches terminal immediately after resume");
}

int main()
{
    setenv("TERM", "xterm-256color", 1);
    setenv("LC_ALL", "C.UTF-8", 1);
    int master = -1;
    int slave  = -1;
    winsize size{.ws_row = 4, .ws_col = 10, .ws_xpixel = 0, .ws_ypixel = 0};
    check(openpty(&master, &slave, nullptr, nullptr, &size) == 0, "open PTY");
    check(fcntl(master, F_SETFL, O_NONBLOCK) == 0, "nonblocking PTY master");
    int saved_input = dup(STDIN_FILENO);
    int saved_output = dup(STDOUT_FILENO);
    check(saved_input >= 0 && saved_output >= 0, "save standard descriptors");
    check(dup2(slave, STDIN_FILENO) >= 0 && dup2(slave, STDOUT_FILENO) >= 0, "attach PTY");
    termios original{};
    check(tcgetattr(slave, &original) == 0, "read initial terminal mode");
    struct sigaction original_action{};
    check(sigaction(SIGINT, nullptr, &original_action) == 0, "read initial signal handler");

    terminal_t terminal;
    std::string error;
    check(terminal_open(terminal, error), error.c_str());
    termios mode{};
    check(tcgetattr(slave, &mode) == 0 && !(mode.c_lflag & (ICANON | ECHO)) && (mode.c_lflag & ISIG), "raw input retains terminal signals");
    auto startup = drain(master);
    check(startup.find("\033[?1049h") != std::string::npos && startup.find("\033[?1006h") != std::string::npos, "alternate screen and SGR mouse enabled");
    test_input(terminal, master);
    test_render(terminal, master);
    test_differential_render(terminal, master);
    test_synchronized_output(terminal, master);
    check(raise(SIGWINCH) == 0 && !terminal_stopped(), "resize signal does not stop terminal");
    check(raise(SIGINT) == 0 && terminal_stopped(), "SIGINT requests clean exit");
    check(terminal_close(terminal, error), error.c_str());
    check(tcgetattr(slave, &mode) == 0 && same_mode(original, mode), "terminal settings restored");
    auto shutdown = drain(master);
    check(shutdown.find("\033[?1006l") != std::string::npos && shutdown.find("\033[?1049l") != std::string::npos &&
          shutdown.find("\033[?25r") != std::string::npos, "mouse, cursor and alternate screen restored");
    struct sigaction restored_action{};
    check(sigaction(SIGINT, nullptr, &restored_action) == 0 && original_action.sa_handler == restored_action.sa_handler, "signal handler restored");
    check(terminal_close(terminal, error), "close is idempotent");

    for (int signal : {SIGTERM, SIGHUP, SIGQUIT})
    {
        check(terminal_open(terminal, error), error.c_str());
        check(!terminal.synchronized_output, "reopen rechecks synchronized-output support");
        expect_event(terminal, master, "\033[?2026;0$y", terminal_event_kind_t::none);
        check(!terminal.synchronized_output && !terminal.sync_query_pending, "unsupported synchronization uses ordinary output");
        check(!terminal_stopped(), "reopen resets stop flag");
        check(raise(signal) == 0 && terminal_stopped(), "termination signal requests cleanup");
        check(terminal_close(terminal, error), error.c_str());
        check(tcgetattr(slave, &mode) == 0 && same_mode(original, mode), "mode restored after termination signal");
        drain(master);
    }
    test_suspend(master, original);

    setenv("TERM", "dumb", 1);
    check(!terminal_open(terminal, error) && !terminal.mode_saved, "unsupported terminal rejected before mode changes");
    check(terminal_close(terminal, error), "cleanup after initialization failure");
    setenv("TERM", "xterm", 1);
    error.clear();
    check(terminal_open(terminal, error), error.c_str());
    drain(master);
    int invalid_output = open("/dev/null", O_RDONLY);
    check(invalid_output >= 0 && dup2(invalid_output, STDOUT_FILENO) >= 0, "simulate output failure");
    check(!terminal_render(terminal, {{"test"}}, error) && !error.empty(), "render errors are reported");
    error.clear();
    check(!terminal_close(terminal, error) && !error.empty(), "cleanup output failure is reported");
    check(tcgetattr(slave, &mode) == 0 && same_mode(original, mode), "mode restored despite output failure");
    close(invalid_output);
    check(dup2(saved_input, STDIN_FILENO) >= 0 && dup2(saved_output, STDOUT_FILENO) >= 0, "restore standard descriptors");
    close(saved_input);
    close(saved_output);
    close(slave);
    close(master);
    fprintf(stderr, "terminal tests passed\n");
}
