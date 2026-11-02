# ytop

[![FreeBSD port version](https://repology.org/badge/version-for-repo/freebsd/ytop.svg)](https://www.freshports.org/sysutils/ytop/)

A minimal process monitor for FreeBSD, written in C++23. It uses FreeBSD's
base-system compiler and `libkvm`, with a built-in VT terminal module and no
ncurses dependency.

## Build and run

```sh
make
./ytop
```

Install to `/usr/local/bin` by default:

```sh
make install
```

Terminal-module regression tests use a pseudo-terminal and FreeBSD's base
`libutil`; they do not require a browser or ncurses:

```sh
make -C tests
./tests/terminal_test
./tests/core_test
```

Core regression tests cover table layout, selection, CPU calculations, and live
process/thread sampling. They temporarily restrict only their own main thread's
CPU affinity and restore it afterwards; run with permission to query the
processes and threads visible to the monitor.

An interactive VT-compatible terminal is required, such as the FreeBSD console
or an xterm-compatible terminal over SSH or ttyd. `TERM` must be set and must
not be `dumb`.
Live-process access follows FreeBSD permissions and jail visibility rules; root
access is not required when the system permits access.

Run `ytop` without arguments. CPU bars always use real system measurements.
Command-line arguments report usage and exit with an error before opening
the terminal.

## Controls

| Input | Action |
| --- | --- |
| `Up` / `Down` | Move the selection by one displayed row, including thread rows. Selection does not wrap; the list scrolls to keep it visible. With no selection, `Down` selects the first visible row and `Up` selects the last visible row. |
| `Space` | Enter detail mode for the selected process, showing all its threads and clearing the selection. In detail mode, exit even with no selection. Exiting restores the process-only list and selects the target process, keeping it visible; if it has disappeared, selection stays cleared. Does nothing in the normal list with no selection. |
| Left-click | Select a process or thread. In the normal list, clicking the already selected process enters detail mode. In detail mode, clicks only select rows and never collapse threads or exit the mode. Clicking the status bar, header, or blank space does not change the selection. |
| Mouse wheel | Move the selection one displayed row per step, like `Up` / `Down`, including thread rows. The list scrolls to keep the selection visible, without wrapping. With no selection, scrolling down selects the first visible row and scrolling up selects the last visible row. |
| `Esc` / Right-click | Clear an existing selection without changing the view or scroll position. With no selection in detail mode, return to the process list and select the target process as with `Space`. With no selection in the normal list, do nothing. |
| `q` / `Ctrl+C` | Quit and restore the terminal from either mode. |

Selection of a process or thread that disappears is cleared on the next sample.
Process filtering stays active if the target disappears; only the header remains
until `Space`, or `Esc` / right-click with no selection, exits detail mode,
or the filtered PID appears again.
All other keyboard input is ignored. Terminal resizing is supported.
Incomplete mouse escape sequences do not exit the monitor. Partial reports are
retained across input timeouts; a new escape sequence resynchronizes truncated
reports. Mouse controls use
SGR or legacy X10 reports; keyboard controls remain available on terminals
without mouse reporting. `Ctrl+Z` restores the terminal before suspension and
the display is redrawn immediately when resumed, without waiting for a sample.

The display refreshes every second. The cursor is hidden while the monitor runs
and its previous visibility is restored on terminals supporting private-mode
save/restore; otherwise it is made visible on exit. The bottom line is cleared
before returning to the shell so the prompt does not overlap a process row on
terminals without an alternate screen.

Rendering reuses frame/output buffers and skips unchanged rows. The terminal
module supports changed-span updates for plain ASCII rows, merging nearby
changes to reduce cursor and style commands. Rows with a right-side CPU bar,
color spans, or multibyte text use whole-row updates when changed. The TUI
uses plain ASCII table rows, enabling changed-span updates; colored CPU grid
rows use whole-row updates when changed.
No terminfo library is used.
The terminal is queried for synchronized-output mode (DEC mode 2026); frames
are bracketed only after a response confirms support. Terminals that do not
respond or report no support continue with ordinary buffered updates.
The current locale determines multibyte decoding and character widths. Invalid
bytes and terminal control characters in displayed text are replaced with `?`;
text is clipped at character boundaries.
The sampler separately replaces non-ASCII bytes in process names, thread names,
usernames, and wait-channel text with `?` before they reach the renderer.

## Display

There is no right-side CPU bar.
Thread detail mode shows logical CPU bars above the table in a responsive grid,
as `  0[||||||||||]`, without a space before `[`. CPU IDs are right-aligned in a fixed width of three;
the layout targets up to 256 logical CPUs (IDs 0 through 255),
without a `CPU` prefix. The grid uses at least two columns when there are two
or more CPUs and the terminal is wide enough. The preferred column count is
the integer square root of the CPU count (at least two), limited by screen
width at roughly 30 terminal columns per CPU. Within that limit, the largest
column count that divides the CPU count is preferred, avoiding empty slots.
If no divisor of at least two fits, two columns are used when possible,
leaving at most one empty slot. For example, 8 CPUs use four rows and two
columns, 10 CPUs use five rows and two columns, and 12 CPUs use four rows
and three columns. All CPU bars have the same length, calculated from the
narrowest column and fixed three-character CPU ID; leftover width remains blank rather than
extending the last bar. Each closing bracket is immediately followed by the
next three-character CPU ID, without extra column padding. Unused width is left
at the right edge. The CPU grid reserves at least one blank cell at the
terminal's right edge; the process/thread table still uses the full width. Narrow terminals
fall back to one column. CPUs are ordered top to bottom, then left to right.
Column heights are balanced, differing by at most one CPU; any extra CPUs
are assigned to the leftmost columns.
Green represents user plus nice,
red system time, and yellow interrupts; idle capacity uses gray `|` characters.
CPU IDs and brackets use ANSI bright black (gray). Cumulative
segment boundaries are rounded to the nearest cell. These are whole-CPU
measurements, not usage attributable only to the selected process.
A blank line precedes the CPU grid, and another separates it from the table.
The CPU area, including both blank lines, never exceeds half the terminal height
(rounded down). Columns increase when necessary, preferring divisors of the
CPU count, even if this makes bars narrower. Showing all CPUs takes priority over
avoiding empty slots. Dense layouts shorten bars,
reducing each CPU to its three-character ID, brackets and at least one bar cell.
If even this minimum cannot fit, excess rows are clipped; enlarge the terminal
to see all CPUs. Terminals shorter than six rows hide the grid.
Clicking CPU rows or the table header does not select
a thread. The process list never shows CPU bars.
The table header stays fixed below the CPU bars (on the first line when no CPU
grid is displayed).
All rows below the header are available for processes and threads.
The table uses the full terminal width.
Load averages, process summaries, and keyboard hints are not shown.
Header fields use the terminal's default text color; the selected row uses ANSI
blue in the process list and ANSI green in the thread list. Table backgrounds remain unchanged.
Colors use the terminal's 16-color palette, so their appearance depends on its
theme. Header and selection highlighting do not use reverse video.
Foreground changes fully reset rendition before applying the color and any
reverse-video attribute.

Normal process-list columns follow FreeBSD `top`:

1. PID
2. Real username
3. Thread count
4. Priority
5. Nice value
6. Virtual memory (`SIZE`)
7. Resident memory (`RES`)
8. State or wait channel (`STATE`)
9. Current or last CPU (`C`), or `-` if unavailable
10. Accumulated CPU time (`TIME`)
11. Unweighted CPU percentage (`CPU`)
12. Command name (`COMMAND`)

Detail mode replaces `PID` with `TID`, replaces `CPU` with user-mode CPU
percentage (`UCPU`) and kernel-mode CPU percentage (`SCPU`) for user processes,
and omits `USERNAME`,
`THR`, `SIZE`, and `RES`.
For kernel processes, detail mode shows only `CPU`, omitting `UCPU` and
`SCPU` from both the header and data rows. Kernel-process detail mode also
omits affinity information.
`STATE` immediately follows `NICE`, followed by `NIVCSW` and `NVCSW`.
`TIME` immediately precedes `UCPU` for user processes or `CPU` for kernel processes.
CPU percentage columns are followed by
the user-thread affinity column `A`, `C`, and `THREAD`.

There is no separate JID column. In the normal process list, commands with
`ki_jid > 0` are prefixed with `[jail:ID] `; host processes (`ki_jid == 0`) have
no jail prefix. Detail-mode thread names omit the jail prefix on all rows.

### Threads and detail mode

In the normal list, each process has one row, and `THR` shows the total thread
count. The normal list does not display `TID`. In detail mode, the parent row's
TID is `ki_tid` of the representative thread returned by `kvm_getprocs(KERN_PROC_PROC)`.

Detail mode shows only the target process and its threads underneath it. Thread rows are obtained
from `kvm_getprocs(KERN_PROC_ALL)` and are sorted by TID within each process.
Detail mode omits the `THR` column from the header and all rows.
The TID already shown on the parent row is omitted from the child rows. Thread
rows can be selected without toggling the parent. The PID column is not shown
in detail mode. There is no separate thread expansion or collapse.
The detail-mode PID persists across refreshes and selection changes until
`Space`, or `Esc` / right-click with no selection, returns to the normal list.
`Tab` is ignored.

Detail-mode thread rows show per-thread state, runtime, CPU usage, and thread names.
`THREAD` shows the thread name directly, falling back to the process name
when the thread name is empty.

In detail mode, the parent `THREAD` also uses the thread name matching its
displayed TID; other parent columns retain process-wide values. The parent and
each displayed thread show cumulative involuntary and voluntary context-switch
counts in `NIVCSW` and `NVCSW`, each followed by `+rate` for its per-second rate.
Both columns are left aligned with a fixed width of 9 characters, plus one
separating space. Both cumulative counts and rates use decimal
K/M/G/T/P/E suffixes for large values (for example, `123K` or `234M`), without
decimal places. Each compact value uses at most four characters for normal
counter ranges, so `123M+223K` fits exactly. Exceptionally long content
ends with `...`.
Counts are cumulative; rates use counter
differences divided by the actual elapsed sample time, rounded to integers.
The rate suffix is hidden when the rounded rate is zero or unavailable
on the first sample, for a newly observed record, or when
the corresponding counter decreases or no positive sample interval is available.
The counters come from
`ki_rusage.ru_nivcsw` and `ki_rusage.ru_nvcsw`, respectively. For a
single-threaded process, detail mode shows the parent's counts without adding
a thread row. If the representative thread
disappears between the two kernel queries, the parent keeps the process name
and process counters until a matching sample is available. Exiting detail mode restores
the process name and hides the counts.

User-process detail-mode rows, including the parent row's representative TID,
show the thread's cpuset CPU mask in a separate `A` column before `C`, using
comma-separated CPU numbers without a prefix or brackets. Its width adjusts
to fit all displayed masks, with a minimum width of three and right alignment
for both the header and values. For example:

```text
NIVCSW    NVCSW         A   C THREAD
12        345+10        0   0 thread
67+2      8901+50   0,2,3   2 thread
```

Masks equal to the monitor's root CPU set show `-` in `A`. Inherited process or jail
restrictions are included. Scheduler-level kernel thread bindings are not
represented by this cpuset mask, so kernel-process detail mode omits affinity.
The affinity indicates allowed CPUs, not the current CPU shown in the `C` column. An explicit binding
to all available CPUs is indistinguishable from no restriction. Affinity lookup
errors are reported; threads that exit during lookup are skipped.

Memory columns (`SIZE` and `RES`) are shown only in the normal process list.
Swap usage is not displayed.

### CPU accounting

Process and thread CPU histories are tracked separately. System processes and
kernel idle threads are included; zombies are hidden. Processes are sorted by
PID in ascending order.

Sampling follows the active view. The process list queries only process records,
without thread or affinity queries. Detail mode queries only the target PID's
process record and threads; hidden kernel-thread affinity and thread usernames
are not collected. Switching views samples immediately and resets record-rate
baselines, so context-switch rate suffixes are hidden until rates become available.
Only visible rows are formatted, lazily on first display after each sample.
Selection changes and scrolling reuse already formatted rows without copying
the list. Input waits until the next sample deadline rather than polling every
100 ms; keyboard/mouse input and signals can wake it earlier.

The first snapshot uses the kernel CPU estimate; later snapshots use runtime
differences without process-age weighting. A multi-threaded process can exceed
100%.

For user processes, `UCPU` and `SCPU` use differences in
`ki_rusage.ru_utime` and `ki_rusage.ru_stime`, divided by elapsed sample time.
For kernel processes (`P_KPROC`) and their threads, only `CPU` is displayed;
kernel CPU is not split using rusage time differences.
A value of 100% represents one CPU. Parent rows retain process
totals in detail mode; child rows use per-thread accounting. These rates are
not normalized to sum to 100%.

For user processes, the first sample displays `0.00%` in `UCPU` and `SCPU`
until interval measurements are available; `CPU` uses the kernel estimate.
On later samples, records with no previous entry
or decreased accumulated runtime are treated as new instances: their
accumulated CPU times are divided by the sample interval. For records treated
as the same instance, if either user-mode or kernel-mode time decreases,
both `UCPU` and `SCPU` show `-`. Sample intervals are measured using
`std::chrono::steady_clock`. Kernel accounting resolution and separately accounted
interrupt time can cause user-process `UCPU` + `SCPU` to differ slightly
from `CPU`.

## Code structure

### System CPU sampling

`top_sample` accepts an optional detail PID (`-1` for the process list) and
collects whole-system and per-CPU data. These system CPU summaries are retained
in the sampling API; per-CPU data is displayed above the thread table.
Like FreeBSD `top`, it sizes the per-CPU buffer using
`kern.smp.maxcpus`, reads `kern.cp_times`, and omits CPU slots whose counters
are all zero. `snapshot.cpus` retains the actual CPU IDs in ascending order,
including gaps in numbering. `snapshot.cpu` is the separate whole-system
summary read from `kern.cp_time`; its ID is `-1`.

Each `cpu_t` contains raw cumulative `ticks` and `percent_tenths`, indexed by
FreeBSD's `CP_USER`, `CP_NICE`, `CP_SYS`, `CP_INTR`, and `CP_IDLE` constants.
For example, `percent_tenths[CP_SYS] == 123` means 12.3% system CPU.
Percentages use FreeBSD `top`'s counter differences, counter-wrap handling,
and integer rounding to tenths of a percent. Each CPU and the whole-system
summary are normalized separately. The first sample compares with zero
counters (proportions since boot); zero-change intervals yield zero percentages.
Rounded percentages need not sum to exactly 100%.

CPU histories are indexed by actual CPU ID, independently of process/thread
histories, and are committed only after the entire sample succeeds. Sysctl
errors, malformed counter sizes, and samples with no active CPUs are reported.
`top_close` releases all histories. CPU sampling adds no disk I/O metrics.

The code uses data-only structs and free functions, without custom classes or
namespaces. Custom types use lowercase snake_case names ending in `_t`.
File-local helpers use `static` linkage.

Initialize a `top_t` with `top_t top{}`, call `top_open` and `top_sample`, then
call `top_close` on both success and failure paths to release the kernel handle
and sampling history. The TUI explicitly restores the terminal before
returning, including on errors.

| Files | Responsibility |
| --- | --- |
| `top.cpp` / `top.h` | Process and thread sampling through `kvm_getprocs`, including per-thread context-switch counts, username lookup, and ascending PID sorting; whole-system and per-CPU counters and percentages through sysctl. |
| `tui.cpp` / `tui.h` | Process/thread table layout, selection, scrolling, detail mode, and refresh loop. |
| `terminal.cpp` / `terminal.h` | VT rendering, terminal modes and restoration, signal handling, resizing, and keyboard/mouse input decoding. |
| `main.cpp` | Application startup and cleanup. |

## Scope

This monitor intentionally omits numeric system CPU summaries, per-CPU displays,
system memory and swap summaries, full command arguments, filtering, and
interactive sort selection. Threads are available through per-process expansion.
It is not a complete replacement for FreeBSD `top`.
