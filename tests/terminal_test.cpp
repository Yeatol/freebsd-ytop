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
    check(output.find("\033[7mheader    ") != std::string::npos, "reverse row fills width");
    check(output.find("safe?[31m") != std::string::npos && output.find("\033[31m") == std::string::npos, "control characters cannot inject terminal commands");
    check(output.find("A\xe4\xb8\xad" "B") != std::string::npos, "UTF-8 preserved");
    check(terminal_render(terminal, rows, error), error.c_str());
    check(drain(master).empty(), "unchanged frame writes nothing");
    rows.resize(1);
    check(terminal_render(terminal, rows, error), error.c_str());
    output = drain(master);
    check(output.find("\033[2;1H\033[0m\033[2K") != std::string::npos, "removed rows are cleared");

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
