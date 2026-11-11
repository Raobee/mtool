# mtool — 跨平台 PCIe BAR / MMIO 读写工具

`mtool` 复刻了测试平台上那套 `mtool` 的用法：保留 `r.l` / `w.l`
这种命令形式，同时补上 PCI BAR 解析、PCI 设备枚举、sysfs BAR 访问，
并提供 **x86_64 / aarch64 / armv7 的静态链接构建**。

代码是单文件 C99，只使用 POSIX/Linux 系统调用，不含架构汇编，也不依赖
libpci。目标场景是：把静态二进制拷到一台没有编译器、甚至没有 glibc 的
Linux 测试机上，直接读 FPGA 的 PCIe BAR 或 `/dev/mem`。

---

## 1. 目录结构

```text
mtool/
├── mtool.c                  # 单文件实现
├── Makefile                 # 本机构建 / 测试入口
├── scripts/
│   └── build-static.sh      # x86_64 + aarch64 + armv7 静态构建
├── tests/
│   └── test_mtool.sh        # 文件后端测试，不需要 root / 不需要硬件
├── .gitlab-ci.yml           # GitLab CI：三架构静态编译
├── .gitignore
├── LICENSE                  # MIT
└── README.md
```

`dist/` 是构建产物目录，已被 `.gitignore` 忽略；CI 的 artifact 也从这里取。

---

## 2. 快速开始

本机编译（动态链接，开发调试用）：

```sh
make
./mtool --help
```

静态编译（要拷到别的机器时用这个）：

```sh
sh scripts/build-static.sh all
```

要求本机有：

- `musl-gcc`（x86_64，用 musl 彻底不依赖 glibc）
- `aarch64-linux-gnu-gcc`
- `arm-linux-gnueabihf-gcc`

Debian/Ubuntu 上安装：

```sh
sudo apt-get install -y build-essential musl-tools \
    gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf \
    libc6-dev-arm64-cross libc6-dev-armhf-cross
```

生成：

```text
dist/mtool.x86_64       # x86_64，musl 静态链接
dist/mtool.aarch64      # ARM64，静态链接
dist/mtool.armv7        # ARM32 hard-float，静态链接
dist/*.sha256
```

也可以只编一个：

```sh
sh scripts/build-static.sh x86_64
sh scripts/build-static.sh aarch64
sh scripts/build-static.sh armv7
```

自定义交叉编译器：

```sh
CC_AARCH64=/opt/toolchain/bin/aarch64-linux-gnu-gcc \
CC_ARM32=/opt/toolchain/bin/arm-linux-gnueabihf-gcc \
sh scripts/build-static.sh all
```

---

## 3. 命令语法

```text
mtool r.{b|w|l|q} ADDR [COUNT] [options]
mtool w.{b|w|l|q} ADDR VALUE [VALUE ...] [options]
mtool pci [BDF] [--enable]
mtool bar BDF [INDEX] [--enable]
mtool help | version
```

| 写法 | 宽度 | 一行打印 |
|---|---|---|
| `r.b` / `w.b` | 8-bit | 16 个字节 |
| `r.w` / `w.w` | 16-bit | 8 个字 |
| `r.l` / `w.l` | 32-bit | 4 个字（照片里的格式） |
| `r.q` / `w.q` | 64-bit | 2 个字 |

- `ADDR` 支持 `0x...` 和十进制。
- 读命令的 `COUNT` 是“元素个数”，不是字节数：`r.l ... 100` 读 400 字节。
- 写命令默认静默，和照片一致；加 `-v` 才打印写入确认。
- 读命令输出 16 字节一行、右侧带 ASCII 列；`--no-ascii` 可关闭。
- `q.*` 默认拆成两个 32-bit MMIO 访问（对 32-bit BAR / AXI-Lite 更稳）；
  确实需要单次 64-bit 访问时加 `--qword-atomic`。

常用例子：

```sh
# 照片里的两条
./mtool r.l 0x404040000 100
./mtool w.l 0x404040088 0xb600

# 列出所有 PCI 设备
./mtool pci

# 查看 FPGA 的 BAR
./mtool pci 01:00.0

# 通过 sysfs resource0 读 BAR0 前 0x100 字节（推荐）
./mtool r.l --bar 01:00.0:0 0 0x100

# 绝对地址 + 先使能该设备的 Memory Space
./mtool r.l 0x4fb00000 0x100 --pci 01:00.0

# 不知道地址属于哪个 BAR 时，让工具自己找并使能
./mtool r.l 0x4fb00000 0x100 --auto-enable

# 测试文件后端，不需要 root
./mtool w.l --file /tmp/mem.bin 0x10 0x11223344
./mtool r.l --file /tmp/mem.bin 0x10 1
```

输出示例：

```text
000000004fb00000: 00000000 00000000 00000000 00000000  ................
000000004fb00010: 00000000 00000000 00000000 00000000  ................
000000004fb00020: 00000000 00000000 20215021 00000000  ........!P! ....
000000004fb00030: 00000000 00000000 00000000 52454754  ............TGER
```

---

## 4. 两种访问后端

### 4.1 `/dev/mem`（保留原 mtool 的用法）

```sh
./mtool r.l 0x404040000 100
```

- 需要 root。
- 地址是**物理地址**。
- 受 `CONFIG_STRICT_DEVMEM`、`CONFIG_IO_STRICT_DEVMEM`、lockdown 影响。
- 适合已知 BAR 物理地址、或没有 PCI sysfs 的平台。

### 4.2 sysfs `resourceN`（推荐用于 PCI BAR）

```sh
./mtool r.l --bar 01:00.0:0 0 0x100
```

- 只需要 BDF + BAR 号，不需要知道物理地址。
- BAR 大小、64-bit 地址由内核处理。
- 不依赖 `/dev/mem` 对 RAM/MMIO 的放行策略。
- 读取时如果 Memory Space 没开，工具会自动使能（`--no-enable` 可关闭）。
- 有 IOMMU / 地址转换的平台优先用这个；BAR 的 CPU 物理地址可能不等于
  PCI 总线地址。

---

## 5. 为什么要先打开 PCI Memory Space

照片里的板子能直接读出数据，是因为它的 BAR 已经被驱动或固件使能了。
本次测试平台上的 FPGA 初始状态是：

```text
Control: I/O- Mem- BusMaster-
```

也就是 PCI COMMAND 寄存器的 Memory Space Enable（bit 1）为 0。
这时用 `/dev/mem` 或 `resource0` 读，都会得到 `0xffffffff`。

`mtool` 不只看 `/sys/.../enable`，而是读真正的 PCI 配置空间 COMMAND
寄存器（offset 4）：

1. COMMAND bit 1 已为 1：直接访问。
2. 否则先写 `/sys/bus/pci/devices/<BDF>/enable`。
3. 如果 `enable` 这个软件引用计数已经非 0（例如被 `setpci` 改过），
   写 `enable` 不会真正改 COMMAND；此时工具直接写配置空间，把 bit 1 置 1。
4. 再读 COMMAND 确认。

这一点在本机实际踩到过：`enable=1` 但 `COMMAND=0000`，只看 `enable`
会误判。

---

## 6. 本机 `/dev/mem` 是否可用

测试平台：`root@192.168.131.133`（Ubuntu 22.04.5，
kernel 5.15.0-174-generic，x86_64）。

内核配置：

```text
CONFIG_DEVMEM=y
CONFIG_STRICT_DEVMEM=y
# CONFIG_IO_STRICT_DEVMEM is not set
CONFIG_LOCK_DOWN_KERNEL_FORCE_NONE=y
```

其他状态：

```text
/dev/mem                        crw-r----- root kmem
/proc/cmdline                   没有 iomem=relaxed
/sys/kernel/security/lockdown   [none] integrity confidentiality
```

实测：

| 地址 | 结果 | 说明 |
|---|---|---|
| `0x00100000`（System RAM） | `mmap: Operation not permitted` | 被 `CONFIG_STRICT_DEVMEM` 拦截 |
| `0xc0100000`（PCI ECAM） | `701810ee 20100002 ...` | MMIO 可读，返回 01:00.0 的配置空间 |
| `0xfec00000`（IOAPIC） | `00000030 ...` | MMIO 可读 |
| `0x4fb00000`（FPGA BAR0） | 使能 Memory Space 后有数据 | BAR 可读 |

结论：

- **当前机器不需要重新编译内核，也不需要加 `iomem=relaxed`，就能通过
  `/dev/mem` 读 PCI BAR。**
- `CONFIG_STRICT_DEVMEM=y` 在 x86 上主要限制 System RAM；PCI/MMIO 区间
  通常仍然放行。真正被拦的是 `0x100000` 这种 RAM 地址。
- 如果确实要读 System RAM，才需要考虑：
  - x86 启动参数加 `iomem=relaxed`（降低限制，有风险）；
  - 或重新编译内核，设 `CONFIG_STRICT_DEVMEM=n` /
    `CONFIG_IO_STRICT_DEVMEM=n`。
- `CONFIG_IO_STRICT_DEVMEM` 未设置，没有额外的 I/O 资源独占限制。
- 内核 lockdown 为 `none`，没有被 Secure Boot 锁死。
- 在 arm64 / armv7 上配置项和行为可能不同；先看
  `/proc/config.gz` 或 `/boot/config-$(uname -r)`，并优先用 `--bar`。

> `CONFIG_STRICT_DEVMEM` 是编译期选项，不是模块参数。
> `iomem=relaxed` 只是 x86 的启动参数，不是所有架构都支持。

---

## 7. 本次在 192.168.131.133 上的实测

FPGA 设备：

```text
01:00.0 Memory controller: Xilinx Corporation Device 7018 [10ee:7018]
Region 0: Memory at 0x4fb00000 (32-bit, non-prefetchable) [size=1K]
```

`mtool pci 01:00.0`：

```text
0000:01:00.0  10ee:7018  class=0x058000
  driver: -
  memory decoding: off
  link: 5.0 GT/s PCIe x1
  BAR0: phys=0x000000004fb00000 size=0x400 flags=0x40200 [mem]
        /sys/bus/pci/devices/0000:01:00.0/resource0
  ...
```

初始 `COMMAND=0000`，读 BAR 返回全 `ff`。执行：

```sh
/root/mtool/mtool r.l --bar 0000:01:00.0:0 0 0x40
```

工具自动使能 Memory Space 后，读到：

```text
000000004fb00000: 00000000 00000000 00000000 00000000  ................
000000004fb00010: 00000000 00000000 00000000 00000000  ................
000000004fb00020: 00000000 00000000 20215021 00000000  ........!P! ....
000000004fb00030: 00000000 00000000 00000000 52454754  ............TGER
```

完整 1KB 的规律是：每 0x80 字节重复一次，在偏移 `0x28` 和 `0x3c`
出现同样的两个 32-bit 值；其余为 0。这看起来是当前 FPGA 码流里的
测试/状态数据，不是 PCI 配置空间。

这块 BAR 只支持 32-bit 访问：`r.q` 用默认的“两次 32-bit”方式能读出
`0000000020215021`，而加 `--qword-atomic` 做单次 64-bit 访问会得到
`ffffffffffffffff`。所以默认拆开更稳妥；如果设计确实有 64-bit 寄存器，
再用 `--qword-atomic`。

用 `/dev/mem` 的绝对地址方式读同一段，结果与 `--bar` 完全一致：

```sh
/root/mtool/mtool r.l 0x4fb00000 0x40 --pci 0000:01:00.0
```

照片里的数据是 `00002024 00011111 00001721 00000001`，和这块板子的
当前码流不同，属于不同设计/不同 bitstream，不是工具差异。

> 当前 BAR 只有 1KB，且没有内核驱动。如果换成带 XDMA 或大 BAR 的设计，
> `mtool pci <BDF>` 会按 `resource` 文件显示实际大小。

本次测试只做了**读**；除了 PCI COMMAND 的使能/恢复，没有向 FPGA 的
BAR 写入任何数据。

---

## 8. 静态链接与 GitLab CI

### 8.1 为什么是静态

工具可能被拷到没有编译器、没有 glibc、甚至没有包管理器的测试机。
动态链接的二进制会依赖目标机的 `/lib/ld-linux-*.so` 和 libc 版本；
静态链接后只需要 Linux 内核。

三个产物的链接方式：

| 文件 | 架构 | 链接 |
|---|---|---|
| `dist/mtool.x86_64` | x86_64 | musl 静态，完全不依赖 glibc |
| `dist/mtool.aarch64` | ARM64 | glibc 静态，libc 已包含在 ELF 内 |
| `dist/mtool.armv7` | ARM32 hard-float | glibc 静态，libc 已包含在 ELF 内 |

验证：

```sh
file dist/mtool.x86_64
# ELF 64-bit LSB executable, x86-64, statically linked

ldd dist/mtool.x86_64
# not a dynamic executable

readelf -d dist/mtool.x86_64
# There is no dynamic section in this file.
```

ARM 两个文件同理；`readelf -d` 没有 `NEEDED` 项。

> x86_64 用 musl 是为了得到“完全不依赖 glibc”的最小静态二进制。
> ARM 用 Debian/Ubuntu 的交叉 gcc 加 `-static`；虽然构建时用的是 glibc，
> 但 libc 已经被链进 ELF，目标机不需要再装 glibc。工具不调用 NSS/DNS，
> 不会在运行时去找 `/etc/nsswitch.conf`。

### 8.2 本地构建

```sh
sh scripts/build-static.sh all
```

脚本会检查每个产物是否真的没有 `NEEDED`，并生成 `.sha256`。

### 8.3 GitLab CI

仓库自带 `.gitlab-ci.yml`：

- `image: debian:bookworm-slim`，只装编译工具，不装模拟器/虚拟机；
- 安装前先把 Debian apt 源切到国内镜像（默认
  `mirrors.tuna.tsinghua.edu.cn`，可用 CI 变量 `APT_MIRROR` 覆盖），
  避免 Docker 内直接走国外 apt 源；
- 安装 `musl-tools`、`gcc-aarch64-linux-gnu`、`gcc-arm-linux-gnueabihf`、
  对应的 `libc6-dev-*-cross`；
- `parallel:matrix` 三个 `ARCH`：`x86_64`、`aarch64`、`armv7`；
- 每个 job：
  1. `sh scripts/build-static.sh "$ARCH"`；
  2. x86_64 直接跑文件测试；
  3. aarch64/armv7 用 `file` + `readelf -h` 校验 ELF 架构，
     静态链接检查已由构建脚本完成；
  4. 上传 `dist/` artifact，保留一周。

CI 只负责编译和静态链接校验，不执行 ARM 二进制；ARM 版本的运行验证
需要在真实 ARM 机器上进行（或开发阶段在本地临时使用用户态模拟器）。

### 8.4 本地测试

```sh
make check                     # 动态版本，文件后端
make check-static              # dist/mtool.x86_64
MTOOL_TEST_PCI=1 make check    # 额外测试 pci 枚举（需要 /sys）
```

CI 中默认关闭 `pci` 枚举测试，因为最小容器的 `/sys` 可能不完整；
文件后端测试覆盖了读、写、对齐、越界、宽度和 ASCII 输出。

---

## 9. 跨平台说明

- 源码只依赖 C99 + POSIX（`open`/`mmap`/`pread`/`pwrite`/`readlink`/
  `msync`），没有 x86/ARM 汇编。
- 定义了 `_FILE_OFFSET_BITS=64`，32 位系统也能映射 4GB 以上的 BAR。
- PCIe/MMIO 寄存器按小端解释；x86、arm、aarch64 的 Linux 常见配置都是小端。
- `q.*` 默认拆成两次 32-bit 访问；在 32 位架构上不会生成单次 64-bit
  访问，避免 `ldrd` 对齐问题。
- 写路径按 little-endian PCIe 设备处理；这也是绝大多数 FPGA 板卡的实际情况。
- 静态二进制仍需要目标内核提供 `/dev/mem` 或 PCI sysfs；这是内核接口，
  不是链接方式能解决的。

---

## 10. 安全提醒

1. `w.*` 会直接写 MMIO。写错地址可能挂死总线、复位板卡或损坏设计。
2. 先 `r.*` 读、确认地址和值，再写。
3. 有些寄存器是 read-clear / FIFO pop，读一下就会改变状态；读之前先看设计文档。
4. `--bar` 默认会自动打开 Memory Space，并在进程退出后保持打开；
   不想改变状态就用 `--no-enable`，并自己确保已使能。
5. `mtool pci` 默认只读；只有 `--enable` 会改 PCI 配置。
6. `--read-only` 只限制 BAR/文件的数据访问，不阻止 `--bar` 为读取而使能
   Memory Space；要严格零写入，先手动使能，再加 `--no-enable`。
7. 不要在正在被驱动使用的设备上随意写 BAR；先确认没有驱动绑定，或先解绑驱动。

---

## 11. 故障排查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| `mmap ... Operation not permitted` | `CONFIG_STRICT_DEVMEM` 拦了 RAM | 读 BAR 用 `--bar`；读 RAM 才需要 `iomem=relaxed` 或改内核 |
| 读 BAR 全是 `ff` | Memory Space 没开，或 BAR 没实现，或 FPGA 没配置 | `mtool pci <BDF>` 看 `memory decoding`；`setpci -s <BDF> COMMAND`；`lspci -vv` |
| `open /dev/mem: Permission denied` | 不是 root / 权限不足 | `sudo` 或 root |
| `--bar` 打不开 `resourceN` | 不是 root，或 BAR 未分配 | 检查 `resource` 文件；确认 BAR 地址非 0 |
| `BARn is not assigned` | 该 BAR 没被 BIOS/内核分配 | 检查 `lspci -vv`、`/proc/iomem`、PCIe 桥窗口 |
| `pci` 看不到设备 | 没有 PCI sysfs / 设备没枚举 | `lspci`、`dmesg`、检查插槽和供电 |
| `enable` 写了但 COMMAND 还是 0 | `enable` 是引用计数，可能已非 0 | 本工具会自动回退到直接写 COMMAND；也可 `setpci -s <BDF> COMMAND=0x2` |
| `r.q` 读到全 `ff`，`r.l` 正常 | BAR 只支持 32-bit 访问 | 用默认 `r.q`（两次 32-bit）或两次 `r.l`；`--qword-atomic` 仅用于支持 64-bit 的设计 |

---

## 12. 与照片用法的对应关系

| 照片 | 本工具 |
|---|---|
| `mtool w.1 0x40404001c 0x1` | 应为 `mtool w.l 0x40404001c 0x1`（`l` 是 32-bit） |
| `mtool r.l 0x404040000 100` | `./mtool r.l 0x404040000 100` |
| `mtool w.l 0x404040088 0xb600` | `./mtool w.l 0x404040088 0xb600` |
| 写命令无输出 | 默认静默，`-v` 才输出 |
| 16 字节一行、右侧 ASCII | 相同；`--no-ascii` 可关闭 |
| 只有物理地址 | 追加 `--bar BDF:N` 和 `pci` 子命令 |

如果照片里的 `w.1` 确实是数字 `1` 而不是字母 `l`，那它可能是原工具
另一种“1 字节”写法；本工具用 `b` 表示 8-bit（`w.b`），用 `l` 表示
32-bit，避免和数字 `1` 混淆。