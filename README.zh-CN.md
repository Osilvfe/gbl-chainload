# gbl-chainload

[English](README.md) | 简体中文

面向支持 Qualcomm GBL / EFISP 加载机制的设备的 AArch64 UEFI 链式加载程序。本项目在内存中修改 ABL、安装必要的协议拦截，再交接给修改后的 ABL 启动 Android；同时提供 GBL FastbootLib，以及可选的 UniXNU EFI 启动能力。

本仓库在 [1vivy/gbl-chainload](https://github.com/1vivy/gbl-chainload) 的基础上继续维护。固件使用仓库固定版本的 CLO edk2 核心，主机工具与补丁引擎使用 Rust。

## 使用前必读

> **这是实验性启动链工具，不是通用刷机包，也不保证防砖。** 使用不匹配的 ABL、vbmeta 或固件，可能导致启动失败、900E、无法进入 fastboot，甚至数据丢失。请先备份并确认恢复手段。

- 首次验证优先使用 `fastboot stage` 临时加载，不要直接刷写启动链分区，也不要通过真正重新上锁来验证 fakelock。
- 临时加载只表示不把该 EFI 写入分区，**不代表整个启动过程只读或无风险**。后续 ABL、系统及 fastboot 命令仍可能修改设备状态。
- mode-1 / mode-2 不等同于真正重新上锁，也不保证通过所有应用或硬件证明检查。
- patch12/13 必须在 GBL 已运行、且修改后的 ABL 被加载后才能生效，不能解决 GBL 运行之前的启动故障。
- 静态补丁命中、主机测试通过，不等于对应机型已完成实机启动验证。

本文按当前脚本与代码说明用法；历史实验、设备差异和逆向结论见 [项目文档](docs/project/README.md)。版本号以 [VERSION](VERSION) 为准。

## 工作原理与适用范围

正常 Android 启动路径：

```text
设备 UEFI / GBL 加载入口
  └─ gbl-chainload EFI + GBLP1 载荷
       ├─ 优先使用载荷内已打补丁的缓存 ABL
       ├─ 缓存不可用时，尝试读取活动槽 ABL 并动态打补丁
       ├─ 根据载荷 manifest 安装协议拦截
       └─ 交接给修改后的 ABL → Android
```

ABL 提取、必要补丁或交接失败时，程序会尝试返回 GBL FastbootLib 作为恢复入口。这不是对所有故障都能恢复的承诺。

项目主要面向 OnePlus / Oppo 等 Oplus 设备。当前 ABL 测试池包含一加 15（infiniti）、一加 15T（fairlady）、一加 Ace 6T（macan）及部分小米 / Redmi 样本，共 13 份去重镜像。**这是补丁回归覆盖范围，不是通用兼容机型名单**；还需要设备本身具有可用的 GBL / EFISP 加载入口。

样本来源、版本证据和测试用途见 [测试镜像说明](tests/images/README.md)。不要把测试池内其他机型的 ABL 用于自己的设备。

## 运行模式

**同一个基础 EFI 支持所有模式**。mode 由拼接时写入 GBLP1 的 manifest 决定，不再为每个 mode 单独编译固件。

| 模式 | 行为 |
| --- | --- |
| mode-0 | 不启用 fakelock 或 profile-spoof；仍保留通用保护策略及打包时选择的 ABL 补丁。 |
| mode-1 | 协议层临时 fakelock，向相关接口提供锁定视图；抑制相关锁态持久化路径，避免把伪装状态提交到 RPMB。 |
| mode-2 | 面向自定义 ROM，在 QSEE / SPSS 边界修改相关 TA 请求载荷；通过 vbmeta 派生的 profile 提供对应数据，ABL 内部仍保持真实解锁状态。 |

mode-0 不是“完全无修改”，mode-1 / mode-2 也不是对任意混合固件组合的兼容保证。更换 ROM 或 OTA 后，应重新检查 ABL、vbmeta 和 mode-2 profile 的对应关系。

mode-2 在加载或动态修改 ABL 前校验 profile。profile 缺失或无效时中止 chainload，返回 GBL fastboot，不再以“locked/green 交接但没有 TA profile”的状态继续启动。每次尝试都会清除遗留 profile 和警告，交接失败也会清除 profile。QSEE profile 改写只接受成功启动 `keymaster` / `keymaster64` 后确认的 handle，不凭命令号猜测授权；未确认或其他 TA 的请求原样转发。这些检查不改变 mode-1 的持久化策略。

### 补丁与品牌选择

模式选择和 OEM 选择互相独立：

| 补丁 | 范围 | 作用 |
| --- | --- | --- |
| patch6 / patch10 | 通用补丁组 | 修改 ABL 的 fastboot 锁态限制及 libavb 校验结果处理路径。 |
| patch11（含 11a / 11b / 11c） | 仅 mode-2，跨品牌 | 向外传递 locked / green 状态，并在 verification-disabled vbmeta 下保留必要的 vbmeta 摘要参数。 |
| patch7 | Oplus，主机打包时启用 | 跳过 ABL 的解锁警告画面及相应等待。 |
| patch12 | Oplus | 保留 A/B 重试计数与切槽，定向绕过已分析的拒绝进入 fastboot、重置和死循环分支。 |
| patch13 | Oplus | 让存储的 unbootable 标记不影响本次 ABL 的相关可启动性判断；不改 GPT 写入路径，也不保证清除磁盘上的标记。 |

- 给 Oplus 设备打包时，使用 `--oem oplus` 启用 patch7/12/13。
- 不指定 `--oem`，或使用 `--oem none`，主机工具完全跳过这三个补丁，而不是尝试后记为 `MISS`。
- 小米 / Redmi 使用 `--oem none` 或省略该参数。`oneplus` 仅作为 `oplus` 的旧名称兼容保留。
- 设备端从活动槽 ABL 动态回退时，只有同时检测到对应 Oplus 专有字符串，才启用 patch12/13；patch7 仍只在主机端应用。
- patch12/13 保持可选且支持重复应用。mode-2 打包则会强制检查 patch11 的三个部分，任一未命中都会终止生成。
- patch11a/11b/11c 只在 mode-2 一起启用。mode-0 / mode-1 完全跳过，mode-1 使用自身的协议拦截而非 patch11。直接运行 `gbl patch` 默认是 mode-0，使用 `gbl patch --mode 2` 才选择 patch11；设备端回退在打补丁前读取 manifest，并以 `WantProfileSpoof` 控制。

### 通用保护与 mode-1 持久化策略

所有模式都安装通用保护：抑制已处理的三类 TZ soft-fuse / rollback-update SCM 调用，并吞掉已匹配的 `oplusreserve1` / `opporeserve1` 分区写入，包括 token 清零写入。这些是针对特定接口和分区的拦截，不是全局只读策略，也不保证降级固件安全。

mode-1 下，VB `WRITE_CONFIG` 是**先修正为 unlocked，再转发**，并非简单丢弃。VB device reset、OplusSec `write_rpmb_boot_info`（`0x0A`）和 KeyMaster `WRITE_KM_DEVICE_STATE`（`0x203`）则在对应拦截路径被抑制，目标是让 fakelock 保持临时状态，而非把伪装的锁定记录持久化。该策略不是按 SoC 设置的白名单；设备观测和仍存在的不确定性见 [逆向结论](docs/project/re-findings.md)。

不要为了验证这些拦截而重新上锁或切槽。已有日志中的 `write-swallow`、`token-zero-write`、`healed->unlocked`、`WRITE_KM_DEVICE_STATE` 可作为检查线索，但单独出现这些事件不构成完整的安全证明。

### patch11c 与 vbmeta 交接一致性

原厂 libavb 在 verification-disabled 路径可能省略 `androidboot.vbmeta.{hash_alg,size,digest}`，依赖用户空间看到 orange 状态。patch11a/11b 改为报告 locked / green 后，缺少这些参数可能导致第一阶段挂载失败。patch11c 移除该禁用分支，让正常交接路径继续提供这些参数。

`--experimental-vbmeta-cmdline` 已是废弃的空操作，因为 11c 已并入仅 mode-2 启用的 patch11；mode-2 拼接自动使用 `--require-patch11`，mode-0 / mode-1 显式要求此参数会被拒绝。这不能让不匹配的原厂 / 自定义 AVB 链变得有效；11c 还会运行原禁用路径跳过的 managed-verity helper。应验证完整镜像组合，不能把补丁命中视为适配保证。

## 从源码构建

### Linux 本机构建

需要 Rust / Cargo（1.85 或更新版本），以及提供 `aarch64-linux-gnu-gcc` 和对应 binutils 的 AArch64 GNU 交叉工具链。Debian / Ubuntu 可先安装本机依赖：

```bash
sudo apt-get install git build-essential python3 python3-setuptools uuid-dev nasm acpica-tools gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

```bash
git clone https://github.com/Osilvfe/gbl-chainload.git
cd gbl-chainload
git submodule update --init edk2
rustup target add aarch64-unknown-none

cargo build --release -p gbl
scripts/build-native.sh
```

默认输出：

- `target/release/gbl`：主机端多功能工具，包含 `unwrap`、`patch`、`pack`、`inspect`、`mode2`、`avb`、`commit` 子命令。
- `dist/gbl-chainload-debug-gcc5.efi`：带调试日志的基础 EFI，**尚未拼接设备专用载荷**。

需要关闭 GBL 调试输出时：

```bash
scripts/build-native.sh --release --output dist/gbl-chainload.efi
```

注意：本机构建脚本始终使用 EDK2 的 `RELEASE` 目标，默认的 debug 名称表示 `GBL_DEBUG=1`；`--release` 将其改为 `0`。不要仅凭文件名推断全部编译优化设置。不同工具链或日志配置的实机结果不能互相替代。

`cargo` 负责 Rust 工具和静态库；最终 EFI 仍需要 EDK2 链接，不能只运行一次 `cargo build` 就得到完整固件。

### 本机统一构建入口

所有阶段均使用本机安装的工具链，已移除 Docker 构建支持。准备好上述 Linux 依赖后，可以使用统一入口：

```bash
scripts/build.sh --debug --no-recovery
```

该示例生成 `dist/gbl-chainload-debug.efi` 和 `dist/host/gbl`。统一入口默认关闭调试日志，`--debug` 开启 GBL 调试日志；`--auto` 和 `--verbose` 分别控制启动策略和详细日志，**没有 `--mode` 构建参数**。使用 `--no-host` 可跳过主机工具。

需要同时构建 Android recovery 工具时，请安装 Android NDK（原环境使用 r27c），设置 `ANDROID_NDK_HOME` 为其目录，执行 `rustup target add aarch64-linux-android`，再去掉 `--no-recovery`。脚本会在固件编译前检查 recovery 前置条件。跨平台主机工具链要求见 [tools/README.md](tools/README.md#building)。

脚本保留 `Build/`，包括 UniXNU 源码缓存和组件。使用统一入口产物套用后面的拼接示例时，应把主机工具路径改为 `--bin-dir dist/host`，基础 EFI 改为 `--efi dist/gbl-chainload-debug.efi`，并通过 `dist/host/gbl` 执行 `inspect`。

## 拼接设备专用 EFISP

以下示例使用本机构建产物。请自行准备目标设备对应固件的 ABL 和用于派生 profile 的 vbmeta，不要跨机型复用。

以 Oplus 的 mode-2 为例：

```bash
python3 scripts/efisp-package.py \
  --bin-dir target/release \
  --abl /path/to/stock_abl.img \
  --mode 2 \
  --oem oplus \
  --stock-vbmeta /path/to/stock_vbmeta.img \
  --efi dist/gbl-chainload-debug-gcc5.efi \
  --out dist/efisp-payload/device-mode2-debug.efi

target/release/gbl inspect dist/efisp-payload/device-mode2-debug.efi
```

- mode-0 / mode-1：将 `--mode` 改为 `0` / `1`，并移除 `--stock-vbmeta`；Oplus 仍需保留 `--oem oplus` 才会包含专属补丁。
- mode-2 必须提供 `--stock-vbmeta`，并自动启用 `--require-patch11`。
- **拼接脚本**会把模式传给 `gbl patch --mode`，它不是基础固件构建参数。
- 脚本完成解包、补丁、profile 生成和拼接，**不会替你刷写设备**。
- `inspect` 检查容器结构与完整性，不验证该镜像是否适配硬件；使用前还要确认最终文件没有超过目标 EFISP 分区容量。

更多工具参数见 [主机工具文档](tools/README.md)。

已有 EFISP 可能包含旧策略下对所有模式应用 patch11 的缓存 ABL；本次修改不会自动撤销这些字节。要让 mode-0 / mode-1 使用新行为，需以原厂输入重新构建、拼接。

## 临时加载与两套 fastboot

**GBL FastbootLib 与原厂 ABL fastboot 是两套实现。** 能列出 `fastboot devices` 并不表示当前处于 GBL；`adb reboot bootloader` 也不能作为进入 GBL FastbootLib 的通用方法。

下面的临时加载流程要求设备**已经能够进入支持 `oem boot-efi` 的 GBL FastbootLib**。首次建立 GBL / EFISP 加载入口的方法取决于设备，不存在可直接套用到所有机型的刷写步骤。

先查询正在运行的构建，再加载候选镜像：

```bash
fastboot getvar gbl-chainload_build
fastboot stage dist/efisp-payload/device-mode2-debug.efi
fastboot oem boot-efi
```

基础 EFI 的默认按键策略：

| 构建策略 | 检测窗口超时 | 音量上键 |
| --- | --- | --- |
| 默认，不带 `--auto` | 继续加载修改后的 ABL，正常目标是 Android | 进入 GBL FastbootLib |
| 带 `--auto` 的主机门控构建 | 留在 GBL FastbootLib 等待主机命令 | 立即继续加载修改后的 ABL |

需要在新加载的候选镜像里继续操作 GBL fastboot 时，应按该镜像的策略进入 FastbootLib。`fastboot oem escape` 用于退出 GBL FastbootLib，继续走修改后的 ABL 路径。

**stage 载荷不会自动保存到 EFISP。** 重启后仍从设备已安装的固件 / EFISP 启动，不应把重启后的结果当成候选 stage 镜像仍在运行。

### 诊断查询

GBL 提供构建和日志配置元数据，以及轻量级 vbmeta 诊断：

```bash
fastboot getvar gbl-chainload_version
fastboot getvar gbl-chainload_date
fastboot getvar gbl-chainload_auto
fastboot getvar gbl-chainload_debug
fastboot getvar gbl-chainload_verbose
fastboot getvar unlocked
fastboot getvar oem-unlock-allowed
fastboot getvar vbmeta:capabilities
fastboot getvar vbmeta:slot
fastboot getvar vbmeta:warning
fastboot getvar vbmeta:boot:status
fastboot getvar vbmeta:boot:descriptor-type
```

按分区查询的格式为 `vbmeta:<partition>:status` / `vbmeta:<partition>:descriptor-type`。这些结果和菜单警告只提供诊断线索，不是完整的 AVB 或证明验证；原厂 ABL fastboot 不一定暴露这些变量。已知菜单警告及安装器限制见 [上游问题记录](docs/project/upstream-issues.md)；本文的试运行流程有意不包含修改锁态或写分区的命令。

### 临时启动 boot.img

在 GBL FastbootLib 中，普通命令保留 DTBO 处理路径：

```bash
fastboot boot /path/to/boot.img
```

需要跳过 DTBO 时，使用独立的设备端命令：

```bash
fastboot stage /path/to/boot.img
fastboot oem boot-nodtbo
```

`boot-nodtbo` 不读取 DTBO 分区或叠加其 overlay，也不传递 `androidboot.dtbo_idx`；基础 DTB 仍由 boot / vendor_boot 路径取得，AVB 仍会执行。这不是任意内核镜像都能启动的保证。

### 当前启动路径修复与历史工具

当前源码构建已跳过 GBL 临时启动路径的 verified-boot 警告 UI，通过 `BoardInit()` 初始化平台信息而非硬编码 fairlady ID，并包含 LibUfdt overlay 分配器的状态重置与释放修复。GBL 自身的启动路径与修改后的 ABL 路径相互独立：跳过警告 UI 不等于关闭 AVB，正常启动 Android 也不能证明所有 `fastboot boot` 镜像都能工作。

旧的 [二进制补丁工具](scripts/gbl-fastboot-boot-fix.py) 面向特定历史 EFI 布局，不是当前构建后的必需步骤，不应盲目用于新固件。历史偏移、观测和诊断轨迹保留在 [逆向结论](docs/project/re-findings.md) 中。

## 可选：集成 UniXNU

集成对象是 UniXNU EFI 应用及 DarwinBoot EFI 驱动，复用 GBL 的同一套 CLO edk2 核心；不需要再嵌套加载一份 Mu 固件或 FD。

从锁定版本的源码构建组件：

```bash
scripts/build-unixnu.sh
```

然后把两个组件一同拼接进 EFISP：

```bash
python3 scripts/efisp-package.py \
  --bin-dir target/release \
  --abl /path/to/stock_abl.img \
  --mode 2 --oem oplus \
  --stock-vbmeta /path/to/stock_vbmeta.img \
  --efi dist/gbl-chainload-debug-gcc5.efi \
  --unixnu-efi Build/unixnu/RELEASE/UniXNU.efi \
  --unixnu-loader-efi Build/unixnu/RELEASE/DarwinBoot.efi \
  --out dist/efisp-payload/device-unixnu-mode2-debug.efi
```

组件源码版本记录在 [unixnu-sources.json](scripts/unixnu-sources.json)，源码缓存和编译产物位于忽略的 `Build/unixnu/`。脚本不生成 XNU 内核或 FAT 载荷，仓库也不携带这项功能的预编译 EFI / 设备载荷。`scripts/build-unixnu.sh --debug` 选择 EDK2 的 `DEBUG` 目标并输出到 `Build/unixnu/DEBUG/`，拼接时需相应修改两个组件路径。

进入包含这些组件的 GBL FastbootLib 后：

```bash
fastboot stage /path/to/payload.fat
fastboot oem boot-unixnu
```

`payload.fat` 应为原始 FAT 文件系统镜像，包含 `EFI/UniXNU/unixnu.toml` 及配置引用的启动文件。GBL 将它暴露为只读 RAM 磁盘，再启动嵌入的 UniXNU；**默认启动、按键和超时不会自动选择 UniXNU，Android 仍是正常默认目标**。

fairlady 已有 Android 与 Darwin 启动反馈，但这不等于所有 Darwin 驱动或用户空间都已验证。实现细节与对应测试镜像记录见 [UniXNU 集成说明](docs/notes/unixnu-clo-r25.md)。

## 日志与故障定位

常用固件日志保存在 `logfs` 分区：

- `GblTrace.txt`：逐阶段追加并立即刷新，用来定位交接前停在哪一步；每次 LogFs 初始化都会重建，大小上限 48 KiB。
- `UefiLog1.txt`：平台 UEFI 日志。
- `UefiLogSaved*.txt`：轮转保留的历史日志，需要结合构建信息和启动顺序区分。

在已经取得 root 的 Android 中，确认实际分区路径和现有挂载情况后，可只读挂载并导出。若 `logfs` 已挂载，请使用已有挂载点，不要重复操作：

```bash
adb shell 'su -c "mkdir -p /data/local/tmp/gbl-logfs && mount -t vfat -o ro /dev/block/by-name/logfs /data/local/tmp/gbl-logfs"'
adb pull /data/local/tmp/gbl-logfs ./gbl-logfs
adb shell 'su -c "umount /data/local/tmp/gbl-logfs"'
```

尽快保存失败启动的日志，后续启动或再次初始化可能覆盖它。日志里的设备标识、分区信息等可能涉及隐私，公开前应检查。标准 UEFI 日志受平台缓冲和刷新时机影响，过早停滞可能没有最终记录，因此需要 `GblTrace.txt` 补充定位。

### 开发日志分级

使用 [GblLog.h](GblChainloadPkg/Include/Library/GblLog.h) 提供的接口：

| 接口 | 配置 | 用途与输出 |
| --- | --- | --- |
| `Print(L"...")` | 始终启用 | 必须让用户看到的提示或致命错误，显示到屏幕。 |
| `GBL_INFO("...")` | `GBL_DEBUG=0` | 通过平台状态码日志路径记录语义事件，通常不输出到屏幕。 |
| `GBL_INFO("...")` | `GBL_DEBUG=1` | 通过 `AsciiPrint` 输出相同语义事件，并显示到屏幕。 |
| `VERBOSE("...")` | `GBL_VERBOSE=0` | 编译时移除。 |
| `VERBOSE("...")` | `GBL_VERBOSE=1` | 通过 `AsciiPrint` 输出高频原始跟踪。 |

修改、吞掉操作、相关拦截命令的输出以及启动阶段应使用 `GBL_INFO`；透传查询、原始转储和扫描循环使用 `VERBOSE`，不要把正确性证据只放在 verbose 日志中。两层都应避免没有必要的协议内部封送跟踪；无条件错误和警告路径保留 `DEBUG_ERROR` / `DEBUG_WARN`。

`GBL_INFO` / `VERBOSE` 使用不带 `L` 前缀的 CHAR8 格式字符串。ASCII 字符串参数使用 `%a`，需要时显式转换 CHAR16 字符串。能否最终写入 UEFI 日志仍取决于平台支持、日志掩码与刷新时机，避免大量原始输出挤掉关键记录。

## 开发与回归测试

常用的定向检查：

```bash
cargo test -p patch-engine --features host
bash tests/host/088_patch7_multi_abl.sh
bash tests/host/097_patch11_multi_abl.sh
bash tests/host/098_patch12_patch13_oem.sh
bash tests/host/099_mode2_runtime.sh
bash tests/host/100_native_build.sh
bash tests/host/101_build_defines.sh
```

`098` 对所有样本同时验证主机与固件引擎的 mode-0 / mode-1 / mode-2：模式 0/1 没有 patch11，模式 2 才启用；同时检查 OEM 选择、品牌识别、patch12/13 的 6+3 处改写、重复应用及 GPT 写入位置不变。

`099` 使用模拟固件服务执行真实固件 C 启动流程与 QSEE 包装函数：profile 失败时在访问 ABL 前中止，重试清除遗留状态，且只有已确认的 KeyMaster handle 接受 mode-2 改写。这不代替设备测试。

`100` 使用模拟工具链检查本机构建调度、NDK 发现、跨平台产物路径、校验和及缓存保留；本机依赖齐全时，`010_build_smoke.sh` 实际构建固件，`084` 在对应工具链可用时验证真实跨平台二进制。

`101` 验证字符串宏经过 shell 与 EDK2 规范化后的 GCC 响应文件时的引号语义，覆盖干净 CI 构建触发的长命令路径。

全量测试需要额外准备：部分 recovery 测试镜像通过 Git LFS 保存，需使用 `git lfs pull` 获取真实内容；部分 mode-2 parity 测试还依赖 `082_mode2_profile_parity.sh` 生成的 golden 文件及包括 `avbtool` 在内的派生测试前置条件。应先取得真实样本并补齐参考文件再判断全量测试结果，不能把 LFS 指针当作镜像，也不能仅凭前置文件缺失就认定补丁回归。

### 仓库目录

| 路径 | 内容 |
| --- | --- |
| `GblChainloadPkg/` | EFI 入口、启动流程、协议拦截及载荷处理 |
| `GblChainloadPkg/Library/ProtocolHookLib/` | 通用 `UniversalBaseline.c`、mode-1 `FakelockOverlay.c`、mode-2 `ProfileOverlay.c` |
| `crates/patch-engine/` | Rust 补丁引擎；始终启用的跨品牌补丁在 `abl_permissive/`，patch11 在 `mode2/`，Oplus 补丁在 `oem/oplus/` |
| `tools/gbl/` | 主机 / recovery 多功能工具 |
| `edk2/` | 固定提交的 CLO edk2 派生子模块 |
| `scripts/` | 构建、拼接及开发辅助脚本 |
| `tests/images/` | 去重后的 ABL 等测试样本 |
| `docs/project/` | 当前状态、逆向结论、决策及上游问题记录 |

### 发布与文档维护

日常开发及 CI 使用 `main`。版本发布使用与 `VERSION` 一致的已有 `release-X.Y.Z` 标签，工作流构建对应提交并创建待审核草稿；历史 `v...` 标签不会触发当前发布流程。

中英文 README 使用相同的章节顺序和可执行示例；修改行为说明时应同步更新两版，详细历史实验保留在项目文档中，不再作为当前安装步骤呈现。

许可见 [LICENSE](LICENSE)；第三方组件遵循各自的许可。
