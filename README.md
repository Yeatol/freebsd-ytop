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
```

An interactive VT-compatible terminal is required, such as the FreeBSD console
or an xterm-compatible terminal over SSH or ttyd. `TERM` must be set and must
not be `dumb`. Command-line arguments are not supported.
Live-process access follows FreeBSD permissions and jail visibility rules; root
access is not required when the system permits access.

## Controls

| Input | Action |
| --- | --- |
| `Up` / `Down` | Move the selection by one displayed row, including thread rows. Selection does not wrap; the list scrolls to keep it visible. With no selection, `Down` selects the first visible row and `Up` selects the last visible row. |
| `Space` | For a selected multi-threaded process, expand or collapse its thread rows. For a selected single-threaded process, show or hide its main thread's context-switch counts. On a selected thread row, collapse its parent process and select the parent row, keeping it visible. Does nothing when nothing is selected. |
| Left-click | Select a process or thread. Clicking the already selected process toggles its thread rows when it has more than one thread, or toggles its main thread's context-switch counts when it has one thread. Clicking an already selected thread collapses its parent process and selects the parent row, keeping it visible. Clicking the header or blank space does not change the selection. |
| Mouse wheel | Scroll three rows per step. Scrolling is bounded by the list and adjusted when rows disappear or collapse. |
| `Esc` / Right-click | Clear the selection without collapsing expanded processes or changing the scroll position. |
| `q` / `Ctrl+C` | Quit and restore the terminal. |

Selection of a process or thread that disappears is cleared on the next sample.
Other keyboard input is ignored. Terminal resizing is supported. Escape keys and
incomplete mouse escape sequences do not exit the monitor. Partial reports are
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

Rendering reuses frame/output buffers and updates changed ASCII spans, merging
nearby changes to reduce cursor and style commands. Rows containing multibyte
text retain whole-row updates. No terminfo library is used.
ASCII classification is reused within each row's rendering operation, without
adding classification state to the cross-frame cache.
The terminal is queried for synchronized-output mode (DEC mode 2026); frames
are bracketed only after a response confirms support. Terminals that do not
respond or report no support continue with ordinary buffered updates.
The current locale determines multibyte decoding and character widths. Invalid
bytes and terminal control characters in displayed text are replaced with `?`;
text is clipped at character boundaries.

## Display

The process table has a fixed header on the first line. All remaining rows,
including the bottom line, are available for processes and threads.
There is no bottom status bar. A one-row terminal shows only the header.
Load averages, process summaries, and keyboard hints are not shown.
Header fields use the terminal's default text color; the selected row uses ANSI blue;
backgrounds remain unchanged.
Colors use the terminal's 16-color palette, so their appearance depends on its
theme. Header and selection highlighting do not use reverse video.
Foreground changes fully reset rendition before applying the color and any
reverse-video attribute. This avoids bright colors leaking into subsequent text
on terminals such as Xshell where clearing intensity alone is insufficient.

Columns follow FreeBSD `top`, with a TID column after PID:

1. PID
2. TID
3. Real username
4. Thread count
5. Priority
6. Nice value
7. Virtual and resident memory
8. Swap usage
9. State or wait channel
10. CPU
11. Accumulated CPU time
12. Unweighted CPU percentage (`CPU`)
13. User-mode CPU percentage (`CPU_USR`)
14. Kernel-mode CPU percentage (`CPU_SYS`)
15. Command name

There is no separate JID column. Commands for processes and threads with
`ki_jid > 0` are prefixed with `[jail:ID] `; host processes (`ki_jid == 0`) have
no jail prefix.

### Threads and expansion

Collapsed processes have one row, and `THR` shows the total thread count. The
TID on that row is `ki_tid` of the representative thread returned by
`kvm_getprocs(KERN_PROC_PROC)`.

Expand a process to show its threads underneath it. Expanded rows are obtained
from `kvm_getprocs(KERN_PROC_ALL)` and are sorted by TID within each process.
The TID already shown on the parent row is omitted from the child rows. Thread
rows show their parent process's PID in the PID column and can be selected
without toggling the parent.
Multiple processes can remain expanded at once. Expansion persists across
refreshes and is cleared when a process disappears from a sample or its
accumulated runtime decreases. PID reuse between samples is not detected if
the new process's accumulated runtime is at least that of the previous process.

Expanded rows show per-thread state, runtime, CPU usage, and thread names.
Thread COMMAND uses `process_name{thread_name}`, omitting the braces and thread
name when both names are identical. Unnamed threads use `process_name{}`.

While expanded, the parent COMMAND also uses the thread name matching its
displayed TID; other parent columns retain process-wide values. The parent and
each displayed thread append that thread's cumulative voluntary and involuntary
context-switch counts in the form `(nivcsw=N, nvcsw=N)`. These values come from
`ki_rusage.ru_nivcsw` and `ki_rusage.ru_nvcsw`, respectively. For a
single-threaded process, `Space` or clicking the selected process toggles the
parent's counts without adding a thread row. If the representative thread
disappears between the two kernel queries, the parent keeps the process name
and process counters until a matching sample is available. Collapsing restores
the process name and hides the counts.

Expanded rows, including the parent row's representative TID, prefix COMMAND
with the thread's effective CPU affinity. For example:

```text
[CPU0] process{thread} (nivcsw=N, nvcsw=N)
[jail:69] [CPU0,2,3] process{thread} (nivcsw=N, nvcsw=N)
```

Masks equal to the monitor's root CPU set are omitted. Inherited process or jail
restrictions and kernel thread bindings are included. The affinity indicates
allowed CPUs, not the current CPU shown in the `C` column. An explicit binding
to all available CPUs is indistinguishable from no restriction. Affinity lookup
errors are reported; threads that exit during lookup are skipped.

Thread memory columns refer to shared process memory. SWAP follows the
reference `top` formula: `max(ki_swrss - ki_rssize, 0)` pages, converted to the
same K/M/G units as SIZE and RES. Thread SWAP is also shared process memory,
not an additional per-thread allocation.

### CPU accounting

Process and thread CPU histories are tracked separately. System processes and
kernel idle threads are included; zombies are hidden. Processes are sorted by
PID in ascending order.

The first snapshot uses the kernel CPU estimate; later snapshots use runtime
differences without process-age weighting. A multi-threaded process can exceed
100%.

For user processes, `CPU_USR` and `CPU_SYS` use differences in
`ki_rusage.ru_utime` and `ki_rusage.ru_stime`, divided by elapsed sample time.
For kernel processes (`P_KPROC`) and their threads, `CPU_USR` is shown as `-`
and `CPU_SYS` equals `CPU`; kernel CPU is not split using rusage time
differences. A value of 100% represents one CPU. Parent rows retain process
totals when expanded; child rows use per-thread accounting. These rates are
not normalized to sum to 100%.

For user processes, the first sample displays `0.00%` until interval
measurements are available. On later samples, records with no previous entry
or decreased accumulated runtime are treated as new instances: their
accumulated CPU times are divided by the sample interval. For records treated
as the same instance, if either user-mode or kernel-mode time decreases,
both `CPU_USR` and `CPU_SYS` show `-`. Sample intervals are measured using
`std::chrono::steady_clock`. Kernel accounting resolution and separately accounted
interrupt time can cause user-process `CPU_USR` + `CPU_SYS` to differ slightly
from `CPU`.

## Code structure

### System CPU sampling

`top_sample` also collects whole-system and per-CPU data without changing the
process-table UI. Like FreeBSD `top`, it sizes the per-CPU buffer using
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
| `tui.cpp` / `tui.h` | Process-table layout, selection, scrolling, thread expansion, and refresh loop. |
| `terminal.cpp` / `terminal.h` | VT rendering, terminal modes and restoration, signal handling, resizing, and keyboard/mouse input decoding. |
| `main.cpp` | Application startup and cleanup. |

## Scope

This demo intentionally omits system CPU, memory, and swap summaries, thread
mode, full command arguments, filtering, and interactive sort selection. It is
not a complete replacement for FreeBSD `top`.
