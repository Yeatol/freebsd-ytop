# ytop

[![FreeBSD port version](https://repology.org/badge/version-for-repo/freebsd/ytop.svg)](https://www.freshports.org/sysutils/ytop/)

A minimal process monitor for FreeBSD with built-in support for VT-compatible terminals.

## Dependencies

- C++23-capable compiler
- FreeBSD `make`
- `libprocstat`

## Process list

```text
  PID USER    NICE  SIZE   RES  FDS   TIME       CPU  DOM  THR COMMAND
    0 root       -    0K    5M    0   0:07     0.04%    k  300 kernel
    1 root       0   15M    3M    3   0:00     0.00%    u    1 init
   11 root    ki31    0K  256K    0    54h  1686.88%    k   16 idle
   12 root       -    0K   64K    0   0:00     0.00%    k    4 intr
 1700 nobody     0   82M   30M   35   0:00     0.00%    u   17 stund
 1800 root       0  256M  128M   10   0:12    12.50%   j1    1 c++
 1801 root       0  256M  128M   10   0:24    25.00%   j2    1 c++
 1802 root       0  256M  128M   10   0:36    37.50%   j3    1 c++
```

- `PID`: Process ID.
- `USER`: Username associated with the process's real user ID.
- `NICE`: Nice value for time-sharing processes.
- `SIZE`: Process virtual memory size, including text, data, and stack.
- `RES`: Resident memory size.
- `FDS`: Number of open file descriptors, `-` means the count could not be retrieved.
- `TIME`: Accumulated user and system CPU time for the process.
- `CPU`: Unweighted CPU percentage.
- `DOM`: Process domain: `k` for kernel processes, `u` for host user processes, and `j<ID>` for processes in a jail.
- `THR`: Total thread count.
- `COMMAND`: Command name.

## Thread list

```text
   TID PRI   TIME      UCPU      KCPU STATE     NIVCSW    NVCSW    SOFTFLT HARDFLT    A   C THREAD
101161  59   0:00     0.00%     0.00% sigwai    0         4        12      0          -   6 main
101200  59   0:00     0.00%     0.00% kqread    1         1        8       0          0   0 thread0
101201  59   0:00     0.00%     0.00% kqread    0         2        5       0          1   1 thread1
101202  59   0:00     0.00%     0.00% kqread    1         2        4       0          2   2 thread2
101203  59   0:00     0.00%     0.00% kqread    1         5        6       0          3   3 thread3
101204  59   0:00     0.00%     0.00% kqread    0         3        4       0          4   4 thread4
101205  59   0:00     0.00%     0.00% kqread    1         3        3       0          5   5 thread5
101206  59   0:00     0.00%     0.00% kqread    1         3        6       0          6   6 thread6
101207  59   0:00     0.00%     0.00% kqread    1         2        5       0          7   7 thread7
101208  59   0:00     0.00%     0.00% kqread    1         3        8       0          8   8 thread8
101209  59   0:00     0.00%     0.00% kqread    1         2        7       0          9   9 thread9
101210  59   0:00     0.00%     0.00% kqread    0         3        5       0         10  10 thread10
101211  59   0:00     0.00%     0.00% kqread    1         3        6       0         11  11 thread11
101212  59   0:00     0.00%     0.00% kqread    0         4        4       0         12  12 thread12
101213  59   0:00     0.00%     0.00% kqread    1         2        7       0         13  13 thread13
101214  59   0:00     0.00%     0.00% kqread    1         2        6       0         14  14 thread14
101215  59   0:00     0.00%     0.00% kqread    1         3        5       0         15  15 thread15
```

- `TID`: Thread ID.
- `PRI`: Current thread scheduling priority.
- `TIME`: Accumulated user and system CPU time for the thread.
- `UCPU`: User CPU percentage.
- `KCPU`: System CPU percentage; kernel threads show only `KCPU`.
- `STATE`: Current thread state.
- `NIVCSW`: Cumulative involuntary context-switch count; `+rate` is the number of switches per second.
- `NVCSW`: Cumulative voluntary context-switch count; `+rate` is the number of switches per second.
- `SOFTFLT`: Cumulative minor page faults; `+rate` is the number of faults per second.
- `HARDFLT`: Cumulative major page faults; `+rate` is the number of faults per second.
- `A`: CPU affinity.
- `C`: CPU ID on which the thread is running or last ran.
- `THREAD`: Thread name.

## Mouse interaction

- Left-click: Select a row; click the selected process again to enter the thread list.
- Right-click: Clear selection, return to the process list.
- Mouse wheel: Scroll three rows per step.

## Keyboard interaction

- `Up`: Select the previous row.
- `Down`: Select the next row.
- `Right`: Enter the selected process's thread list.
- `Left`: Return to the process list.
- `q`: Quit.

## License
[](https://github.com/Yeatol/freebsd-stund#license)
This project is licensed under the BSD 2-Clause "Simplified" License. See the `LICENSE` file for full terms and conditions.
