# ytop

[![FreeBSD port version](https://repology.org/badge/version-for-repo/freebsd/ytop.svg)](https://www.freshports.org/sysutils/ytop/)

一个使用 C++23 编写的 FreeBSD 极简进程监视器，适合观测 CPU 密集型服务端程序的运行状态。它使用 FreeBSD 基础系统编译器和 `libkvm`，内置 VT 终端模块。

## 依赖列表

- 支持 C++23 的编译器
- FreeBSD `make`
- `libkvm`

## 进程列表

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

- `PID`：进程 ID。
- `USER`：进程所属用户的用户名。
- `NICE`：分时进程的 nice 值，越小优先级越高；实时/空闲调度分别用 `r`/`i` 前缀，内核线程另加 `k`，固定优先级另加 `F`，不适用时显示 `-`。
- `SIZE`：进程虚拟内存大小，包括代码、数据和栈。
- `RES`：驻留内存大小。
- `TIME`：进程累计用户态和内核态 CPU 时间。
- `CPU`：未加权 CPU 使用率；100% 表示一个逻辑 CPU，多线程进程可超过 100%。
- `DOM`：进程域，`k` 为内核进程，`u` 为主机用户进程，`j<ID>` 为 jail 中的进程。
- `THR`：线程总数。
- `COMMAND`：命令名。

## 线程列表

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

- `TID`：线程 ID。
- `PRI`：线程当前调度优先级，值越小优先级越高。
- `TIME`：线程累计用户态和内核态 CPU 时间。
- `UCPU`：用户态 CPU 使用率。
- `KCPU`：内核态 CPU 使用率；内核线程仅显示 `KCPU`。
- `STATE`：线程当前状态；睡眠时显示等待事件，锁等待时显示以 `*` 开头的锁名。
- `NIVCSW`：非自愿上下文切换累计次数，`+rate` 为每秒增加次数。
- `NVCSW`：自愿上下文切换累计次数，`+rate` 为每秒增加次数。
- `A`：CPU 亲和性。
- `C`：当前或上次运行的 CPU 编号。
- `THREAD`：线程名。

## 鼠标交互

- 鼠标左键：选择行；再次点击已选进程进入线程列表；点击线程表头切换 CPU 网格。
- 鼠标右键：清除选择；无选择时从线程列表返回并选中原进程（若仍存在）。
- 鼠标滚轮：有选择时每步移动一行，无选择时每步滚动三行。

## 键盘交互

- `Up`：向上选择，自动滚动，不循环；无选择时选末个可见行。
- `Down`：向下选择，自动滚动，不循环；无选择时选首个可见行。
- `Space`：选中进程后进入线程列表并清除选择；在线程列表中返回并选中原进程（若仍存在）。
- `Esc`：清除选择；无选择时从线程列表返回并选中原进程（若仍存在）。
- `Tab`：切换线程列表的 CPU 网格，默认隐藏。
- `q`：退出并恢复终端。

## 许可证
[](https://github.com/Yeatol/freebsd-stund#license)
本项目采用 BSD 2-Clause “简化版”许可证。完整条款与条件请参阅 `LICENSE` 文件。
