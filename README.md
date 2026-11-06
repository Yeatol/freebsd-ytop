# ytop

[![FreeBSD port version](https://repology.org/badge/version-for-repo/freebsd/ytop.svg)](https://www.freshports.org/sysutils/ytop/)

A minimal process monitor for FreeBSD, written in C++23, for monitoring CPU-bound server applications. It uses FreeBSD's base-system compiler and `libkvm`, with a built-in VT terminal module.

## Dependencies

- C++23-capable compiler
- FreeBSD `make`
- `libkvm`

## Process list

```text
  PID USER    NICE  SIZE   RES   TIME       CPU  DOM  THR COMMAND
    0 root       -    0K    5M   0:07     0.04%    k  300 kernel
    1 root       0   15M    3M   0:00     0.00%    u    1 init
   11 root    ki31    0K  256K    54h  1686.88%    k   16 idle
   12 root       -    0K   64K   0:00     0.00%    k    4 intr
 1700 nobody     0   82M   30M   0:00     0.00%    u   17 stund
 1800 root       0  256M  128M   0:12    12.50%   j1    1 c++
 1801 root       0  256M  128M   0:24    25.00%   j2    1 c++
 1802 root       0  256M  128M   0:36    37.50%   j3    1 c++
```

- `PID`: Process ID.
- `USER`: Username associated with the process's real user ID.
- `NICE`: Nice value for time-sharing processes; lower values mean higher scheduling priority. Real-time and idle priorities use `r` and `i` prefixes, respectively; kernel threads have an additional `k` prefix, fixed-priority scheduling has an `F` suffix, and `-` means not applicable.
- `SIZE`: Process virtual memory size, including text, data, and stack.
- `RES`: Resident memory size.
- `TIME`: Accumulated user and system CPU time for the process.
- `CPU`: Unweighted CPU percentage; 100% represents one logical CPU, and multithreaded processes can exceed 100%.
- `DOM`: Process domain: `k` for kernel processes, `u` for host user processes, and `j<ID>` for processes in a jail.
- `THR`: Total thread count.
- `COMMAND`: Command name.

## Thread list

```text
   TID PRI   TIME      UCPU      KCPU STATE     NIVCSW    NVCSW       A   C THREAD
101161  59   0:00     0.00%     0.00% sigwai    0         4           -   6 main-thread
101200  59   0:00     0.00%     0.00% kqread    1         1           0   0 network-thread-0
101201  59   0:00     0.00%     0.00% kqread    0         2           1   1 network-thread-1
101202  59   0:00     0.00%     0.00% kqread    1         2           2   2 network-thread-2
101203  59   0:00     0.00%     0.00% kqread    1         5           3   3 network-thread-3
101204  59   0:00     0.00%     0.00% kqread    0         3           4   4 network-thread-4
101205  59   0:00     0.00%     0.00% kqread    1         3           5   5 network-thread-5
101206  59   0:00     0.00%     0.00% kqread    1         3           6   6 network-thread-6
101207  59   0:00     0.00%     0.00% kqread    1         2           7   7 network-thread-7
101208  59   0:00     0.00%     0.00% kqread    1         3           8   8 network-thread-8
101209  59   0:00     0.00%     0.00% kqread    1         2           9   9 network-thread-9
101210  59   0:00     0.00%     0.00% kqread    0         3          10  10 network-thread-10
101211  59   0:00     0.00%     0.00% kqread    1         3          11  11 network-thread-11
101212  59   0:00     0.00%     0.00% kqread    0         4          12  12 network-thread-12
101213  59   0:00     0.00%     0.00% kqread    1         2          13  13 network-thread-13
101214  59   0:00     0.00%     0.00% kqread    1         2          14  14 network-thread-14
101215  59   0:00     0.00%     0.00% kqread    1         3          15  15 network-thread-15
```

- `TID`: Thread ID.
- `PRI`: Current thread scheduling priority; lower values mean higher priority.
- `TIME`: Accumulated user and system CPU time for the thread.
- `UCPU`: User CPU percentage.
- `KCPU`: System CPU percentage; kernel threads show only `KCPU`.
- `STATE`: Current thread state; sleeping threads show the wait channel, and threads blocked on a lock show its name prefixed with `*`.
- `NIVCSW`: Cumulative involuntary context-switch count; `+rate` is the number of switches per second.
- `NVCSW`: Cumulative voluntary context-switch count; `+rate` is the number of switches per second.
- `A`: CPU affinity.
- `C`: CPU ID on which the thread is running or last ran.
- `THREAD`: Thread name.

## Mouse interaction

- Left-click: Select a row; click the selected process again to enter the thread list.
- Right-click: Clear selection; with no selection in the thread list, return to the process list and select the original process if it still exists.
- Mouse wheel: Scroll three rows per step without changing selection.

## Keyboard interaction

- `Up`: Select the previous row, wrapping from the first row to the last and scrolling automatically; with no selection, select the last row of the entire list.
- `Down`: Select the next row, wrapping from the last row to the first and scrolling automatically; with no selection, select the first row of the entire list.
- `Right`: Enter the selected process's thread list and clear selection; with no selected process or already in the thread list, do nothing.
- `Left`: Clear selection first; with no selection in the thread list, return to the process list and select the original process if it still exists. With no selection in the process list, do nothing.
- `q`: Quit and restore the terminal.

## License
[](https://github.com/Yeatol/freebsd-stund#license)
This project is licensed under the BSD 2-Clause "Simplified" License. See the `LICENSE` file for full terms and conditions.
