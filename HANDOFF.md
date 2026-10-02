# 交接状态

面向"换一台机器接着干"或者"隔一段时间回来"。

设计决策在 `ESP32S3_FUSION_IMPLEMENTATION.md`，本文**不重复**那些，只写四样别处
没有的东西：验证过的事实、踩过的坑、当前的阻塞、下一步。

最后更新：采样游标告警的区分已移出 `M0_DIAG`，CI 全绿。**M0 结束**，
**M1 代码完成**，**core-0 余量已直接测出**（`SHORTWRITES=0`，平均占用 82%）。
剩一次上板：`FlashSize` 和 M1 听感。见 §6.1。

---

## 1. 一句话状态

板子已接上、已刷机、**噪音已消失**。出货构建仍是 `dbd9993`（`M0_DIAG 0`，
`DEBUG_ON` 开着，诊断工具链全部保留在开关后面）。

M1 的事件接口层已实现并通过 CI（`432ae84`），**队列零溢出已实测确认**，
但**听感两次都没有人报告**。

**core-0 余量已定，而且不再是推论**：`SHORTWRITES=0`（65 个窗口）是直接测量——
DMA 每个 buffer 都拿到了完整数据，音频一次没掉。平均占用 82%，
M3 按 ≤60–80 µs（10%）规划。`fill` 只占 2.6%，开销全在生成器+MIX。

第一次真正触发采样游标告警，顺带查清了它**不是缺陷**：最坏越界 6 字节（3 帧），
M0 的 guard 正在拦它。顺带发现那个告警把"良性尾越界"和"游标跑出 RamCache"
混在一个数里上报，已修（§6.1）。

**下一步只有一件事：刷一次，抓冷启动前 20 行（`FlashSize`）并听一下。**
步骤在 §6.1。**然后直接进 M2**——core-0 余量不再是阻塞项。

---

## 2. 新机器上怎么把环境恢复出来

### 2.1 不再需要 SSH 私钥

仓库**已经改成公开**（`gh repo edit --visibility public`）。原因：GitHub Actions 对
私有仓库要收 billing，公开之后 CI 直接可用。

remote 仍然是 **HTTPS**：`https://github.com/cxandy/AcidDripS3`。
本节早先写的"私钥在 `~/.ssh/zlyb_id_rsa`、remote 写死 SSH"**已经不成立**——
那段是私有仓库时代的残留。HTTPS 免密钥，两条路都能用。

> 尚未决定：是否把 remote 换回 SSH。HANDOFF 旧版说 SSH，实际是 HTTPS。换不换都行，
> 现在不影响任何事。

### 2.2 仓库

```powershell
git clone https://github.com/cxandy/AcidDripS3.git
```

工作区干净，无 stash，无未推送提交。

### 2.3 两个上游参考克隆（gitignore，clone 下来不会有）

约 62 MB，重新克隆约 1 分钟。**commit 号必须对上**，否则"和上游比对"这件事就失效了：

```powershell
git clone --depth 1 --branch S3-regular https://github.com/copych/AcidBox.git
git clone https://github.com/lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth.git
```

| 目录 | 用途 | 锁定 |
|---|---|---|
| `AcidBox/` | 纯净上游，用来 diff `firmware/AcidBox/` 的改动 | `S3-regular` @ **`931753f`** |
| `Acid_Drip_Bassline_and_Drum_Synth/` | 音序器/UI 的**行为**参考。**它的代码不能进这个仓库**（无 LICENSE，见 `UPSTREAM.md`），只能读、只能重写 | `main` @ **`f88e64d`** |

`AcidBox/` 是 diff 基准，所以**不要在里面改任何东西**。要改的是 `firmware/AcidBox/`。

### 2.4 本机构建工具：装了也没用

本机**编译不了**，别在这上面浪费时间：

- `raw.githubusercontent.com` 在这个网络下连接被重置（curl 35）
- GitHub release CDN 同样被重置，所以 `mklittlefs` 也拉不下来
- 没有 Arduino IDE，没有 PlatformIO

**编译一律交给 GitHub Actions**。本机只需要 Python（跑合并脚本的测试）和 git。

唯一的例外是 `tools/test-merge-image.py`——它纯本地、约 2 秒、9 个用例，
不需要任何工具链。**改了合并逻辑就本地先跑一遍**，比等一次 3 分钟的 CI 快得多：

```powershell
python tools/test-merge-image.py
```

---

## 3. 已验证的事实（不是推测）

### 3.1 容量

| 项 | 数值 |
|---|---|
| `firmware.bin` | **616,880** 字节 |
| app0 分区 | 1,048,576 字节，剩余 **431,696** |
| LittleFS 分区已用 | 2,863,104 / 3,014,656，剩余 **151,552** |
| `merged.bin` | **4,194,304** 字节，地址 `0x0` |
| 固件烧录偏移 | `0x0` / `0x8000` / **`0xe000`** / `0x10000`，LittleFS 在 `0x110000` |

> **616,880 和 616,724 都是对的，但只有一个重要。** arduino-cli 报
> `Sketch uses 616724 bytes`，那是 sketch 大小；`.bin` 另带 156 字节的 image header
> 和段对齐填充。**能不能装进 app0 看的是 616,880。** CI 的余量表现在按文件实际大小算。

### 3.2 工具链版本（CI 里锁死的，不要随手升）

| | 版本 |
|---|---|
| arduino-cli | 1.5.1 |
| esp32 core | 3.3.12 |
| MIDI Library | **5.0.2（必须 5.x）** |
| mklittlefs | 4.1.0 |
| FQBN | `esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=noota_3g,FlashSize=16M` |

`PSRAM=opi` 不是可选项。`PartitionScheme=noota_3g` 也不是：core 3.x 里
`min_spiffs` 已经是完全不同的东西，照抄 2.x 教程会编译通过但分区是错的。

### 3.3 偏移量已经被工具链验证过

`noota_3g` 在 `0x0`–`0x400000` 之间是连续无空洞的，所以五个文件能塌成一个 4 MiB
镜像。CI 拿 arduino-cli **自己**产出的 16 MiB 合并镜像当基准，逐字节比对四段，
run #14 全部 `agrees`：

```
bootloader.bin @0x000000 agrees      boot_app0.bin @0x00e000 agrees
partitions.bin @0x008000 agrees      firmware.bin  @0x010000 agrees
```

**`0xe000` 是验证过的，不是从 CSV 读来就当真的。** 网上教程普遍写的 `0xe0000`
对这张分区表是错的（nvs 占 0x5000 而非默认 0x4000，把 otadata 顶了上去 4 KB）。

---

## 4. 三个会再咬人一次的坑

### 4.1 arduino-cli 的产物叫 `AcidBox.ino.bin`

不是 `AcidBox.bin`，也不是 `firmware.bin`。它按 `<工程文件名>.ino.<类型>.bin` 命名，
所以构建目录里同时有 `AcidBox.ino.bootloader.bin`、`AcidBox.ino.partitions.bin`、
`AcidBox.ino.merged.bin`。

**连着猜错两次之后才去读了目录。** 现在 CI 按后缀识别并统一改名成
`bootloader.bin` / `partitions.bin` / `firmware.bin`，文档和 `flash-args.txt` 里的
名字是真的躺在 artifact 里的名字。**改工程名不会坏，但不要再写死任何名字。**

### 4.2 浏览器烧写只有一个文件

`AcidDripS3-merged` 里的 `merged.bin`，地址 `0x0`，一个按钮。
用 <https://espressif.github.io/esptool-js/>（Chrome / Edge；Firefox 151+ 也支持
Web Serial）。**不要用 web.esphome.io**——它是 ESPHome 的设备向导，会把它自己服务器
编译的固件刷进去，**刷不了 `littlefs.bin`**，鼓组会挂不上（而且
`FORMAT_LITTLEFS_IF_FAILED true` 会静默格式化，听起来"能动"，其实已经不是
808 采样了）。

命令行刷法见 `HARDWARE_SETUP.md` §5.4，命令在 artifact 的 `flash-args.txt` 里，是
可以直接粘贴的。

### 4.3 分刷 LittleFS 要么和固件同一次刷完，要么一次都别刷

只刷固件就上电的后果就是上面那条静默格式化。

### 4.4 附带一条：怎么读 CI 日志

GitHub 新的 Actions 列表页不给 href，run 页面 URL 拼不出来。路径是：
**commit 页面 → checks 状态徽章 → Details**。拿到 run 页面后
`/actions/runs/<id>/job/<id>` 的页面里日志是展开的。

另外，CI 里所有失败诊断都必须打到 **stdout**。GitHub 只把 stdout 上的
`::error::` 变成 commit 页面上的 annotation；同样的文字在 stderr 上就只是
"Process completed with exit code 1"。这个坑让 run #11–#13 白查了很久。

---

## 5. 当前的阻塞

**没有硬阻塞。** 板子在手、CI 全绿、噪音已修。剩下的是待办和待确认项：

| 项 | 状态 |
|---|---|
| 本机不能编译 | 见 §2.4。不是问题，是既定事实 |
| `BENCH_AUDIO_HEADROOM` 未做 | core-0 实际余量还是"我记得好像够"。**建议在 M1 之前量** |
| remote 用 HTTPS 还是 SSH | 无所谓，见 §2.1 的说明 |
| `HARDWARE_SETUP.md` 两处过期 | `:149` 说 `DEBUG_PORT` 走原生 USB（错，是 UART0）；`:171` 还写着 USB MIDI 不可用。两次问过没回，先留着 |
| USB MIDI | 按约定暂时关闭（`MIDI_USB_DEVICE` 关，FQBN 保持 `USBMode=hwcdc,CDCOnBoot=cdc`）。**注意**：重新打开会按作者自己的 guard 再次关掉 `DEBUG_ON`，日志就没了——这是预期行为，不是 bug |

设备管理器里的残留记录（`USB JTAG/serial debug unit`、`COM5` CH340、
`VID_303A&PID_1001` 全是 `Status = Unknown`）与本项目无关，USB-OTG 走的是
`VID_303A&PID_1001`，但它作为独立设备出现。**串口日志走 USB-OTG，不走 UART0。**

---

## 6. 接下来做什么

**按顺序：**

1. ~~**补上 `BENCH_AUDIO_HEADROOM`**，量出 core-0 的实际余量。~~
   **已量完，不再是阻塞项**：`SHORTWRITES=0`（65 窗口，直接测量），平均占用 **82%**，
   M3 按 ≤60–80 µs（10%）规划。详见 §6.1。
   这是在 M1 之前做的——否则 M3 的音序器和 TFT 会踩在一个"我记得好像够"的
   基线上，而不是一个量出来的数字上。这是从 M0 换来的教训：这里原本是拍脑袋写的
   优先级 1→5，代价是 `loop()` 被饿死一整轮排查（§7）。
   **门的位置改了**：查过代码之后发现引擎完全跑在 core 1，M1 加的是 core 0 零负载，
   所以这个数字真正卡的是 **M3 决定"引擎要不要挪到 core 0"** 那一步，不是 M1。
   M1 和本项是并行的。理由和证据见 §6.2。
2. **确认板子真实的 `FlashSize`**（FQBN 现在写的是 `FlashSize=16M`，**是声明，不是实测**）。
   注意"能启动"证明不了：16 MB 的 `noota_3g` 分区表最大一个分区到 ~3.4 MB，
   整张表结束在 6.2 MB 以内，所以 8 MB 的片子启动行为**完全一样**。
   真正的确认在 bootloader 的 flash size 那一行，而它会镜像到 USB-OTG
   （`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG 1`），所以**冷启动日志的前 20 行**就有答案。
3. ~~修 `HARDWARE_SETUP.md` 的 `:149` 和 `:171`~~ —— **已完成**。两处都错了，
   而且不止两处：§4 整节是按"板子上有两个口"写的，实际只有一个能用。
   详见 §5 表最后一行。
   `M0_REVERB_TOGGLE_PIN` 的残留**查过了，没有**：全仓库只剩两处提到 GPIO23
   的注释（`config.h:89`、`AcidBox.ino:614`），两处都是在解释"为什么放弃了它"，
   内容正确，留着。
4. remote 保持 **HTTPS**，不换 SSH。公开仓库用 HTTPS 免密钥，换过去没有收益。
   已确认 `HANDOFF` 旧版"remote 是 SSH"的说法从来就不成立。
5. ~~**M1**（`engine_iface.h` / `engine_iface.ino`）~~ —— **已实现**，`432ae84`，
   CI 36942629138 通过。设计上的坑和偏离文档之处记在 §6.3。
   **M2 才关掉 `JUKEBOX`**，这一步没动。

**然后进 M1**：`engine_iface.h` / `.cpp`，**重新实现** `Acid_Drip` 的行为
（它的代码不能进仓库，见 §2.3）。M2 才关掉 `JUKEBOX`。

设计文档里有完整的里程碑和依赖顺序，关键路径 M0 → M1 → M2 → M2.5 → M3 → M4。

---

## 6.1 上板量 core-0 余量（已量，见下；还差 `SHORTWRITES` 一次）

### 第一次实测用的构建

构建 36942629138 / 提交 `432ae84`（**注意：是 M1 那个构建，不是 `b885d7a`**）。
`BENCH_AUDIO_HEADROOM` 在 M1 里没动，还是 `1`，所以这一个固件同时带
`[BAH]` 仪表**和** M1 的事件层——一次刷机把三件事一起验了：

| 这次刷机验什么 | 看哪一行 / 怎么验 | 结果 |
|---|---|---|
| core-0 余量 | `[BAH]` 的 `mean cpu=...%` | ✅ 82%，见下 |
| **M1 的事件层** | MIDI 出声、jukebox 照常播 | ⚠️ **队列零溢出已确认；听感没人报告** |
| 板子真实 `FlashSize` | 冷启动日志前 20 行 | ❌ **日志里没有冷启动前言，没验成** |

`dbd9993` 那个出货构建不带 `[BAH]`，别刷错。

sketch 561,192 B（53%），全局变量 60,704 B。

`merged.bin` 4,194,304 B，
sha256 `006791F979D365FA1136F8EA40BAE3406A25CA3A4FCDEDA32385FDFC7D9ABD6F`

### 烧录

<https://espressif.github.io/esptool-js/>，`merged.bin` 偏移 **`0x0`**，
不要用 web.esphome.io。LittleFS **已经在 merged.bin 里**（`noota_3g` 方案
offset `0x110000`），所以**不需要**单独烧 littlefs，也就不存在
`FORMAT_LITTLEFS_IF_FAILED` 那个坑。

### 要抓什么

串口监视器开到 **115200**，只连 **USB-OTG** 那个口（UART0 没接出来）。
**从复位那一刻就开始抓**，别等应用出第一行——上次就是从 `[M0] log port:` 开始的，
而 bootloader 的 flash size 那行在它**之前**，所以 `FlashSize` 没验成。

1. **冷启动日志前 20 行** —— 解决 `FlashSize`（§6 第 2 项**仍未完成**）。
   前两次都是从 `[M0] log port:` 开始的，bootloader 那行在它之前，两次都没抓到。
2. **顺手听一下** —— 接 MIDI 键盘弹几个音，再等 30 秒让 jukebox 播一段。
   这是 M1 的验收标准（§6.3），**两次都没有人报告听感**。
3. **在日志里搜 `LEFT RAMCACHE`** —— 出现了就是真故障（游标跑出 RamCache）。
   `sampler cursor hit the end of its own sample` 是良性的，见上面的实测表。

> **core-0 余量已经不用再测了**，`SHORTWRITES=0` 是直接测量，结论已定。
> `BENCH_AUDIO_HEADROOM` 保持 `1` 到下次刷机，之后可以关掉收进 `0`。

### 实测结果（2026-10-02，第一次上板，29 个完整窗口）

构建 `432ae84`，29 个窗口，日志完整无 `[WARN] event queue overflow`。

| 量 | 实测 | 占 725 µs 周期 |
|---|---|---|
| `mean cpu` | 584–611 µs（典型 **594**） | **80.6–84.3%，典型 82%** |
| `worst cpu` | 654–781 µs | 90.2–**107.7%** |
| `gen+mix` | 648–775 µs | 89–107% |
| `fill` | **17–20 µs** | **仅 2.4–2.8%** |
| `block max` | 1408–1725 µs | — |
| `overruns` | 29 窗共 **10** 次 | ≈ 每 4000 个 buffer 一次 |
| `rate check` | 44064–44128 Hz | 期望 44100 ✓ |

**结论：core-0 平均占用 82%，M3 的余量是 18%（131 µs/buffer）。**
`block max` 每个窗口都在 1.5 ms 左右，说明写路径**确实在阻塞**，
也就是任务整体跑在 DMA 前面、有累积余量——这是好消息，不是坏消息。

**`fill` 只占 2.6%，所以别去优化 float→int16 转换循环**，开销全在
`gen+mix`（synth1+synth2+drums+mixer）。要省就省生成器那边。

M3 如果要在 core 0 上放**每 buffer 固定**的开销，**绝对上限 131 µs**，
而那是把余量全部用光、连尾部波动都吸收不掉。**实际按 ≤60–80 µs（10%）规划。**

### ⚠️ 一次自我更正：`overruns` 不是故障信号

这一节原先写的是"`overruns` 应当恒为 0，非 0 就是实打实的 underrun"。**那是错的。**

实测 10 次 overrun，**噪音仍然是修好的状态**。因为一个 buffer 超时会被**后面 buffer 的
余量补回来**：平均每 buffer 富余 131 µs，而最大单次超出只有 56 µs，
发生率约 1/4000。这个量级不可能累积成持续饥饿。

所以 `overruns` 是**预算告警**，不是故障。真正的故障信号是 `SHORTWRITES`——
`I2S.write()` 的返回值短于一个完整 buffer，那才是 DMA 真的没拿到数据、
音频真的掉了。**这个数在第一次实测里根本没有，所以严格讲，
上面 82% 这个结论是"有很强的间接证据"，不是"直接测到了"**——
证据是均值低于周期、加上 block 一直在阻塞（说明有余量）。

### ✅ 第二次实测（2026-10-02，`d484a1e`）：`SHORTWRITES=0`

65 个窗口，**`SHORTWRITES=0 (0 bytes dropped, since boot)` 一个窗口都没破例。**
这是直接测量，不是推论——`I2S.write()` 返回了完整 buffer，DMA 每一块都拿到了数据，
**core-0 没有欠过周期，音频一次都没掉。**

`mean cpu` 577–611 µs（典型 ~595），和第一次的 594 一致 → **82% 复现。**
`overruns` 29 次 / ~89,600 buffer（0.032%），和第一次 0.025% 同量级。
`block min=5` 每个窗口都一样——`I2S.write()` 偶尔不阻塞直接返回，最快的一次 5 µs。

**core-0 余量的结论定了：平均 82%，M3 按 ≤60–80 µs（10%）规划。**

### ⚠️ 第二次实测抓到一个新东西：`[WARN] sampler play cursor`

65 秒里出现 7 次。这是 M0 修的三个越界之一第一次**真的被触发**（之前 `regular_checks()`
被饿死，告警根本没机会打出来）。查清楚了，**结论是 M0 的修复在正常工作，不是新缺陷**：

离线复现了 `Sampler::Process()` 的游标算术，跑遍 84 个已加载采样：

| CC pitch | pitch | 触发数 | **最大越界** |
|---|---|---|---|
| 0–62 | 0.25–0.97 | **0** | 0 |
| 64（默认） | 1.011 | **2 / 84** | 2 字节 |
| 96 | 2.03 | 47 / 84 | 4 字节 |
| 127 | 4.00 | 67 / 84 | **6 字节** |

默认 pitch 下只有 2 个采样能触发（`012_CL9.wav` / `712_CL9.wav`，各 2212 字节）。
**越界最多 6 字节 = 3 帧**，而且只发生在采样播完的那一刻。
上游会读进 RamCache 里**下一个采样的头 2 字节**——一声 tick，仅此而已。
guard 拦住了，所以现在连这 6 字节都没有。

**但原来的告警文案是错的**，这次日志暴露了：它写"left the sample or the cache"，
把两件**严重程度完全不同**的事混成一个数。`oobSample`（采样尾）是上面这种良性越界，
`oobCache`（游标跑出整个 RamCache）才是真故障——**后者从未出现过**。
M0 其实早就写好了这个区分，但只放在 `#if M0_DIAG` 里，
**而出货构建里那个区分根本不存在**，所以只能报一个含混的数。

已提交修复：区分**移出 `M0_DIAG`，改成无条件计数**，存在 `Sampler` 上
（`oobSample` / `oobCache`），`[WARN]` 分成两条，cache 那条带 `***` 前缀。
代价是每个活跃声部每 buffer 多一次比较，对着 594 µs 的生成器开销可以忽略。

### 还差：`FlashSize` 和 M1 听感

构建 `d484a1e`（CI 36948246652）加上了直接测量：

- `bahShortWrites` / `bahShortBytes`：**开机以来累计，不按窗口清零**。
  三分钟前掉过一次，那也是掉过，按窗口清零会把唯一的故障证据抹掉。
- `bahMinBlockUs`：反压的尾部。稳态是工作 594 µs、阻塞约 131 µs，
  所以正常应该在 131 附近；读到 **0** 表示 `I2S.write()` 一次都没等。
  它仍然是推论，所以**和 `SHORTWRITES` 一起报，不是替代它**。

sketch 561,628 B（53%）。`merged.bin` sha256
`604CBC432B31C71FD144BE19468FD2CD9C0602A11E63C097489C8B01303FDF7C`

**再刷一次就齐了**，115200 / USB-OTG。启动第一秒会打
`no buffers completed in 1000 ms`——**这是正常的**，84 个采样的 PSRAM 预加载
超过了第一个窗口，仪表正确区分了"任务没跑"和"任务跑了但零耗时"。
第二秒起就有数了。

### 日志怎么读（现在的版本）

```
[BAH] worst cpu=N us = NN.NN% (gen+mix N / fill N)  mean cpu=N us = NN.NN%  block min/max=N/N us  overruns=N of NNNN buffers  SHORTWRITES=0 (0 bytes dropped, since boot)
```

- **`SHORTWRITES=0` 是唯一必须为 0 的数。** 非 0 = 音频真的掉了，
  上面所有时间数字都要重新审视。
- `overruns` 非 0 **是正常的**，只看趋势——从偶发变连续才是新信息。
- `block min` = 0 值得留意，但单独出现不说明问题；和 `SHORTWRITES` 一起看。
- `rate check` 应当 44100 附近。对不上就是 `micros()` 读数不可信，全部数字作废。
- `mean` **是这次该看重的数**（不是原先说的"只是陪衬"）——
  因为它决定还有多少余量；`worst` 决定余量够不够吸收尾部波动。两个都要。

---

## 6.2 M1 的设计决定（已查证并实现，见下节）

先查了代码，M1 有几件事和设计文档写的不一样。**都不是障碍，是必须提前知道的。**

下面记的是**动手之前查出来、和设计文档不一样的地方**，全部已按查证结果实现。
下次接着干先读这一节，别再照着文档的字面写。

### M1 不需要等 §6.1 的数字——把门开在 M3 上

设计文档的验收标准里没有提 core 归属，但查下来引擎**完全跑在 core 1**：

```
loop()                      AcidBox.ino:441，注释写明 "running on the Core1"
  └─ regular_checks()       AcidBox.ino:446
       └─ MIDI.read()       → MIDI Library 5.0.2 的回调
            └─ handleNoteOn / handleCC / handlePitchBend   midi_handler.ino:54-119
                 └─ Synth1 / Synth2 / Drums
```

`audio_task1`（core 0）和 `audio_task2`（core 1）**只读引擎状态，从不调用这些入口**。
所以 `engine_iface` 只要把队列的消费点放在 `regular_checks()` 里，
**core 0 的负载一点都不会变**——实测构建证实了这一点（M1 只多了 1,336 B 闪存，
全局变量 +16 B）。

→ **§6.1 量出来的数字是 M3 决定引擎放哪个核时需要的，不是 M1 需要的。**
M1 和 §6.1 是并行的。之前把门卡在 M1 上是保守过头了：真正会加 core-0 负载的
是"把引擎挪到 core 0"这个决定，而那个决定属于 M3。

> 这不等于 M1 可以乱来。队列深度、事件结构大小、`eng_setParam` 的实现方式
> 都会影响 RAM 和 core 1 的延迟——M1 自己要把这些量清楚。

### `Ch` 和 MIDI 通道是干净的一一对应（已核对）

| `Ch` | 通道 | 常量 | 出声 |
|---|---|---|---|
| `Acid` | 1 | `SYNTH1_MIDI_CHAN`（`config.h:231`） | `Synth1` |
| `Second` | 2 | `SYNTH2_MIDI_CHAN`（`:232`） | `Synth2` |
| `Drums` | 10 | `DRUM_MIDI_CHAN`（`:234`） | `Drums` |

注意不是 1/2/3。**别按 1/2/3 推。**

### 重音门限 80 是真的（已核对）

`synthvoice.ino:223`：

```cpp
mva_alloc(note, (velocity >= 80));
```

所以设计文档的 `accent ? 127 : 79` 是对的：**79 落在门限下，127 落在门限上**，
两边都留了余量。照抄即可。

### 接口要抹平的两处不对称（这是 M1 真正存在的理由）

1. **`Drums.NoteOff(note)` 不收力度，两个合成器的 `on_midi_noteOFF(note, vel)` 收。**
   `eng_noteOff(Ch, note)` 得替两边补上。
2. **`SetProgram` 只有 `Drums` 有**（`midi_handler.ino:113`）。
   `eng_selectProgram(prog)` 对合成器只能是空操作——**但不能静默**，
   必须留一条 `DEBF()` 警告，否则将来调用方会以为程序切换生效了。

### `CC_ANY_*` 的语义必须原样保留（最容易做错的一处）

`handleCC`（`midi_handler.ino:71-110`）的结构是**先按 CC 号全局匹配，
再按通道路由**：

```cpp
switch (cc_number) {          // ← 不看 inChannel！
    case CC_ANY_COMPRESSOR: ...
    case CC_ANY_DELAY_TIME:  ...
    case CC_ANY_RESET_CCS: case CC_ANY_NOTES_OFF: case CC_ANY_SOUND_OFF: ...
    case CC_ANY_REVERB_TIME: case CC_ANY_REVERB_LVL: ...
    default:                 // ← 只有落到这里才按通道分
      if (inChannel == DRUM_MIDI_CHAN) ...
```

设计文档给的签名是 `eng_setParam(Ch ch, uint8_t cc, uint8_t val)`，通道受限。
**照字面实现就会把上面这一整组全局 CC 全部丢掉**——压缩器、delay、reverb、
notes-off 就此从音序器侧不可达，而且不会有任何报错。

**决定：`eng_setParam` 复用同一段逻辑**，全局 CC 在哪个通道上都生效，和 MIDI 走
进来时行为完全一致。`ch` 对全局 CC 只是提示性参数。这一条要写进 `engine_iface.h`
的注释里，否则下一个读代码的人会"顺手清理"掉它。

### 队列的真实价值：让引擎变成单写者

设计文档说是"解耦"。查下来今天已经有一个隐含的耦合问题：
MIDI 回调在改引擎参数，M2 的音序器也会在改，而两者是不同的调用源。
把消费点收敛到 `regular_checks()` 里唯一一处，**引擎参数就只有一个写者**——
这才是队列要解决的问题，也是它值得那几百字节 RAM 的理由。

事件结构按设计文档 `struct Ev { uint8_t op, ch, a, b; }`（4 字节）。
**消费点放在 `regular_checks()` 里 `MIDI.read()` 之后**，同一次 tick 内排空，
这样 MIDI 来的和音序器来的事件按到达顺序处理，行为可复现。

---

## 6.3 M1：事件接口层（已实现，待上板听）

**提交 `432ae84`，CI 36942629138 通过。** sketch 561,192 B（53%），比 §6.1 那个
构建多 1,336 B；全局变量 +16 B。队列本身 128×4 = 512 B 在 `eng_init()` 时从堆上分配，
对 266,976 B 的剩余空间可以忽略。

下面记的是**动手之前查出来、和设计文档不一样的地方**，全部已按查证结果实现。
下次接着干先读这一节，别再照着文档的字面写。

### 用了 `.ino` 而不是文档要求的 `.cpp`

两个原因，第二个是决定性的：

1. 这个 sketch **一个 `.cpp` 都没有**，全部是 `.ino`，被 arduino-cli 拼成单一编译单元。
   写 `.cpp` 就是本 sketch 第一个独立编译单元，立刻需要为 `Synth1`/`Synth2`/`Drums`/
   `Comp`/`Delay`/`Reverb` 六者写 `extern`——它们全在 `AcidBox.ino:173-184` 实例化，
   在 `.ino` 拼接内部，别处看不见。为了一个文件凭空造出一条链接面。
2. **`midi_handler.ino` 必须反向依赖这一层**。字母序 `"engine_iface" < "midi_handler"`，
   所以在拼接结果里本文件在前，`midi_handler.ino` 看得见这里定义的一切；反过来就是
   前向引用。单一编译单元在这里不是方便，是让依赖方向正确的唯一办法。

`AcidBanger.ino` 和 `AcidBox.ino` 都排在 `engine_iface.ino` 前面，所以两个文件都显式
`#include "engine_iface.h"`。**不要靠字母序决定一个文件能看见什么。**

### MIDI 也走队列——这是本层值得存在的理由

文档的验收标准是"外部代码经此接口出声，与纯 MIDI 模式听感无差异"。如果 MIDI 留在旧路径
上，同一个 dispatch 就还是两份实现，验收就只能靠耳朵去听；而现在两条路走的是同一段代码，
这条标准是**结构性成立**的，不用验。

代价是零延迟：`eng_poll()` 在同一个 `regular_checks()` 轮里、排在该轮入队之后执行，
note-on 在它到达的那一轮就被应用了。**别把 `eng_poll()` 挪到别处**，一挪 MIDI 就多一个
调度周期的延迟，快速乐句上听得出来，而且很容易被误判成音序器的问题。

### `accent` 是覆盖，不是替换（文档字面写法是个回归）

文档写 `accent ? 127 : 79`，完全忽略 `vel`。当时只有音序器会调它，音序器本来就没有自己的
力度，所以丢掉无所谓。**但现在 MIDI 也走这个函数**，丢掉 `vel` 意味着键盘的真实力度被扔、
每个重音都固定成 127。所以现在是 `vel` 为基准、`accent` 抬到 127，
M2 的音序器传 `ENG_VEL_PLAIN` 当基准，得到的正是文档描述的行为。

### 队列的三条设计，都是为了不重蹈覆辙

- **`eng_setParam()` 按 CC 号先匹配 `CC_ANY_*`，不看通道**——和 `handleCC` 一直以来的
  行为一致。压缩器、delay 四个参数、reverb 两个参数、notes-off 全在这一组里。
  按通道路由会让它们对**任何带通道的调用方**不可达，而且**不报任何错**，参数只是不响应了。
- **每轮排空上限 32 个事件。** 无上限排空不是无害的：生产者跑赢它时它就不返回了，
  `loop()` 停住，音序器跟着停——**那就是 M0 的噪音本身**（`audio_task2` 饿死 `loop()`，
  `loop()` 下面的一切静音）。一个能饿死自己调用者的排空，会把刚花一个里程碑关掉的
  bug 原样请回来。
- **队列满了丢弃并计数，不就地执行。** 就地执行会让 `eng_apply()` 不再是引擎状态的
  唯一写者，而且恰好在本来就出事的时候破坏这个不变量。丢一个 note 是能听见的洞，
  插在半排队的队列后面乱序执行是听不见的 bug。128 槽 + 每轮 32 的排空速率，
  要填满需要**一个 `loop()` 轮次内进来 128 个事件**，本固件做不到——所以那个计数器
  是用来**证明**这件事的，不是用来兜底的。

### 审计时发现另外两个写者（所以本提交的范围超出文档的 M1 描述）

队列的存在理由是"引擎参数只有一个写者"。查这个的时候发现还有两个，不改的话
**这次提交的核心说法就是假的**：

- `AcidBanger.ino:511` 的 `Drums.SetProgram()`（jukebox 换鼓组，`flip(30)` 命中就换）
  是**活的**，直接写引擎状态。现已走 `eng_selectProgram()`。顺序不变：上面几行的
  crash note-on 仍然先对旧鼓组发声——队列是 FIFO，两个事件在同一次调用里入队，顺序一致。
- `paramChange()` 里五处 `Synth2.ParseCC()`。**没有任何地方调用 `paramChange()`**
  （唯一调用点在 `AcidBox.ino:470`，注释掉了），所以今天它不是写者——但仍然改了。
  因为"只有一个写者"这个不变量不该在某个死函数被复活的那天悄悄失效；而且那里
  把 `float` 传给 `uint8_t` 形参是隐式截断，现在在边界上变成显式 cast。

jukebox 的音符触发**不用改**：`send_midi_noteon`/`send_midi_noteoff` 本来就调
`handleNoteOn`/`handleNoteOff`（`AcidBanger.ino:315`、`:327`），自动就流进新层了。

`handlePitchBend` **故意保留直接调引擎**：M1 的 API 里没有 pitch bend，
加一个属于发明契约没规定的接口。它是唯一剩下的直接写者，
将来要加"单一写者"断言时，它就是那条断言会抓到的东西。

---

## 7. M0 的噪音问题：结论、修复、以及两次自我更正

症状：上电一声爆音，然后持续噪音。**已修复**，出货构建 `dbd9993`。

### 7.1 根因

`audio_task2` 的 `taskYIELD()`。`taskYIELD()` **只会让给同级或更高优先级的任务**，
所以一个 pin 在 core 1、优先级 5 的任务，在同为 core 1、优先级 1 的 Arduino
`loopTask` 面前根本让不掉——`loop()` 从 `setup()` 之后**再也没跑过**。
`loop()` 才是 `regular_checks()` → `jukebox_tick()` → `run_tick()` → 音序器
那条链的入口。一个 note 调度器从来不 tick 的音序机，"上电一声、然后一直噪音"
完全说得通。

修法：循环末尾 `vTaskDelay(1)`，取代循环开头的 `taskYIELD()`。

**归属问题（别当成已证实的结论）**：我**从没问过** `e2a24a0` 那一版还有没有噪音，
只问了日志里的数字，而 `e2a24a0` 里已经有这个修复了。所以修复可能早一版就生效了。
饥饿修复在证据上仍是更可能的那个——它是唯一能解释"只出一声、之后毫无结构"的改动
——但"更可能"不是"已证实"。

### 7.2 一并修掉的另外三个真实缺陷

1. **`GROUP_HATS` 越界写**：`sampler.ino` 用原始 MIDI 音高 `note±1` 索引
   `samplePlayer`（84 项），`note` 是 uint8_t 可达 127 → 越界最多 44 项。
   改用同函数上一行就算好的安全索引 `j`，并双向做边界检查。
   **如实说明：这版并未证明它在触发**——音序器走 `current_drumkit + drum_note`，
   `current_drumkit` 上限 72，音高不到 83，两种写法碰巧都在范围内。
2. **采样播放游标越界**：上游只拿 `sampleSize` 卡上界，而 `sampleSize` 直接来自
   WAV 头；游标本身由 `pitch` 通过浮点 `samplePosF` 推进，**负浮点转 `uint32`
   会变成巨大正数而不是负索引**。现在同时对采样本身和真实分配卡界。
   实测约每 2 秒触发一次——那是 `pitch` 小数累积造成的**例行**末尾越冲，不是故障，
   所以它是 `[WARN]` 不是 `[ERROR]`。
3. **`Sampler::Init` 会读出 `RamCache`**：现在在边界处停下。实测缓存用
   2,338,738 / 3,145,728（74%），84 个采样，没有触发。

### 7.3 被数据排除的假设（都是量的，不是猜的）

- **synth2 卡音 —— 不存在。** 我上一轮把重复出现的 0.376 读成"卡住的振荡器"，错了。
  `on`/`off` 平衡（s2 on=1..4, off=2..4），`env` 全程 0↔1 跳变。音符在正常分配和释放。
- **削波 —— 没有。** `out` 峰值 0.95（在 `0.25f` 增益和 `fast_shape` **之后**测的）。
  之前看到的 2.5568 是我把探针放在了增益之前。
- **delay 发散 —— 没有。** 它从 0 爬到 1.1464 又落回来，像发散；但
  `delayFeedback = 0.2`（`fx_delay.h:56`），`y[n] = x[n] + 0.2·y[n-D]` 是稳定的，
  数据也一致：空线填满后稳定在 0.2–0.6。
- **reverb** —— mode 1（旁通）下 `reverb=0.0000` 而 `out` 仍在 mode 0 范围内。
- **数值爆炸** —— 全部 67 个窗口 `bad=0`。
- **坏 WAV / pitch** —— 从未出现 `BAD HEADER`。
- **数字信号本身**：所有总线健康，`out` 0.42–0.95，`mixer()` 精确 44.1 kHz。
  **数字信号里不存在能造成持续噪音的缺陷**，所以才把嫌疑推到 DAC 写出侧，
  也就是 mode 2 那个"送确知全零缓冲区、听噪音是否还在"的实验。

### 7.4 两次自我更正，都要记住

- **假排除**：我曾说采样器越界读"已排除"。错。那条消息当时**根本不可能出现**——
  `regular_checks()` 被饿死了，计数器到不了阈值，等于**从来没测过**。
  "没看到证据"和"证据表明没问题"是两回事。
- **预处理指令写了但不生效，且失败是静默的**：`.ino` 按字母序拼接，
  `AcidBanger.ino` 排在 `AcidBox.ino` **前面**，所以 config.h 的符号不保证可见。
  `#if M0_DIAG` 里一个未声明的标识符求值为 0，编译照过，计数器永不递增，
  日志打出一堆看着合理的 0。同一个坑咬了两次。
  同类事故还有第三个：我自己写的校验脚本扫到 `AcidBanger.ino:4` 注释里那句
  字面量 `#if M0_DIAG`（那行正是在解释这个坑），于是开了一个幽灵块、吞掉 1300 行，
  diff 看起来完全合理但全错。**校验工具本身也会撒谎，要让它在不平衡时非零退出。**

---

## 8. 文件地图

| 路径 | 是什么 |
|---|---|
| `ESP32S3_FUSION_IMPLEMENTATION.md` | 设计文档，12 节，D1–D8 决策、风险表、里程碑 |
| `HARDWARE_SETUP.md` | 接线、BOM、GPIO 表、烧录流程、验收清单 |
| `UPSTREAM.md` | 两个上游的许可处理和锁定版本 |
| `firmware/AcidBox/` | DSP 层（vendored，MIT），**要改的是这里** |
| `tools/merge-image.py` | 合并成 4 MiB 镜像，偏移量从 core 的 CSV 解析 |
| `tools/test-merge-image.py` | 9 个用例，纯本地 2 秒 |
| `.github/workflows/build.yml` | 唯一的构建入口 |
| `AcidBox/` | 纯净上游，diff 基准，**不要改** |
| `Acid_Drip_Bassline_and_Drum_Synth/` | 行为参考，**代码不可提交** |
