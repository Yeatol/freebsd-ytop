# ytop

[![FreeBSD port version](https://repology.org/badge/version-for-repo/freebsd/ytop.svg)](https://www.freshports.org/sysutils/ytop/)

A minimal process monitor for FreeBSD, written in C++23. It uses FreeBSD's
base-system compiler, `libkvm`, and `ncurses`.

## Build and run

```sh
make
./ytop
```

Install to `/usr/local/bin` by default:

```sh
make install
```

An interactive terminal is required. Command-line arguments are not supported.
Live-process access follows FreeBSD permissions and jail visibility rules; root
access is not required when the system permits access.

## Controls

| Input | Action |
| --- | --- |
| `Up` / `Down` | Move the selection by one displayed row, including thread rows. Selection does not wrap; the list scrolls to keep it visible. With no selection, `Down` selects the first visible row and `Up` selects the last visible row. |
| `Space` | Expand or collapse the selected parent process when it has more than one thread. Does nothing on a thread row, a single-thread process, or when nothing is selected. |
| Left-click | Select a process. Clicking the already selected process expands or collapses its threads when it has more than one thread. Clicking the header or blank space does not change the selection. |
| Mouse wheel | Scroll three rows per step. Scrolling is bounded by the list and adjusted when rows disappear or collapse. |
| `q` / `Ctrl+C` | Quit and restore the terminal. |

Other keyboard input is ignored. Terminal resizing is supported. Escape keys and
incomplete mouse escape sequences do not exit the monitor. Mouse reporting
support is required.

The display refreshes every second. The cursor is hidden while the monitor runs
and its previous visibility is restored on exit. The bottom line is cleared
before returning to the shell so the prompt does not overlap a process row on
terminals without an alternate screen.

## Display

Only the process table is displayed, with its header on the first line. The
header stays fixed while scrolling. Load averages, process summaries, and
keyboard hints are not shown.

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
rows use `->` in the PID column and can be selected without toggling the parent.
Multiple processes can remain expanded at once. Expansion persists across
refreshes and is cleared when a process exits or its PID is reused.

Expanded rows show per-thread state, runtime, CPU usage, and thread names.
Thread COMMAND uses `process_name{thread_name}`, omitting the braces and thread
name when both names are identical. Unnamed threads use `process_name{}`.

While expanded, the parent COMMAND also uses the thread name matching its
displayed TID; other parent columns retain process-wide values. If that thread
disappears between the two kernel queries, the parent keeps the process name
until a matching sample is available. Collapsing restores the process name.

Expanded rows, including the parent row's representative TID, prefix COMMAND
with the thread's effective CPU affinity. For example:

```text
[CPU0] process{thread}
[jail:69] [CPU0,2,3] process{thread}
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

`CPU_USR` and `CPU_SYS` use differences in `ki_rusage.ru_utime` and
`ki_rusage.ru_stime`, divided by elapsed sample time. A value of 100% represents
one CPU. Parent rows retain process totals when expanded; child rows use
per-thread accounting. These rates are not normalized to sum to 100%.

The first sample displays `0.00%` until interval measurements are available,
except for kernel processes (`P_KPROC`) and their threads: `CPU_SYS` initially
matches `CPU`, while `CPU_USR` remains `0.00%`. A missing baseline on later
samples or backwards counters show `-`; processes born since the previous
sample use their accumulated times. Kernel accounting resolution and separately
accounted interrupt time can cause `CPU_USR` + `CPU_SYS` to differ slightly
from `CPU`.

## Code structure

The code uses data-only structs and free functions, without custom classes or
namespaces. Custom types use lowercase snake_case names ending in `_t`.
File-local helpers use `static` linkage.

Initialize a `top_t` with `top_t top{}`, call `top_open` and `top_sample`, then
call `top_close` on both success and failure paths to release the kernel handle
and sampling history. The TUI explicitly restores the terminal before
returning, including on errors.

| Files | Responsibility |
| --- | --- |
| `top.cpp` / `top.h` | Process sampling through `kvm_getprocs(KERN_PROC_PROC)`, username lookup, and ascending PID sorting. |
| `tui.cpp` / `tui.h` | Curses rendering, signal handling, and refresh loop. |
| `main.cpp` | Application startup and cleanup. |

## Scope

This demo intentionally omits system CPU, memory, and swap summaries, thread
mode, full command arguments, filtering, and interactive sort selection. It is
not a complete replacement for FreeBSD `top`.
