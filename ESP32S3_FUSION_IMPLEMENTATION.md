# Acid Drip × AcidBox — ESP32-S3 融合实施文档

> 版本 v1.0 · 2026-10-01
> 目标平台：ESP32-S3（OPI PSRAM）
> 代码基线：`copych/AcidBox` @ `S3-regular` 分支 `931753f`（v.1.5.0 S3）
> 参考实现：`lonesoulsurfer/Acid_Drip_Bassline_and_Drum_Synth` @ `Acid_Drip_Drum_Acid_Drift_V5`（V5，约 9578 行）

---

## 1. 结论摘要

**不移植 Mozzi。** 把 AcidBox 保留为音频引擎，把 Acid_Drip 保留为交互与音序大脑，两者通过一层事件接口咬合。

核心依据：两者的外部接口本来就是同一种语言。AcidBox 的引擎只暴露事件（`on_midi_noteON` / `ParseCC` / `NoteOn`），Acid_Drip 的音序器本质上就是一个 MIDI 发生器。我们要做的是**让音序器直接调用引擎函数，省掉中间的 MIDI 线缆**，而不是把 303 的 DSP 重新实现一遍。

这样做的收益：

| 维度 | 纯移植 Mozzi（否决） | 融合（本方案） |
|---|---|---|
| 采样率 | 16.384 kHz | **44.1 kHz**（AcidBox） |
| 位深 | 8-bit PWM | **16-bit I2S** |
| 振荡器 | 朴素 | **MipMap 抗混叠** |
| 鼓采样 | 8 个内嵌 | **84 个 PSRAM 预载**（8 套音色） |
| 效果器 | 无 | **Reverb + Delay + 鼓侧链压缩** |
| 滤波器调音 | — | **20 年听测补偿矩阵**（`norm1_tbl`/`norm2_tbl`） |
| 交互 | 16 pad + TFT | 同（Acid_Drip 原样保留） |

---

## 2. 架构决策

本节为引擎层的四项决策（1-4）。**交互层的四项决策（5-8）见第 11 节**，含完整利弊分析。

### 决策 1：DRIFT 引擎 → 第二台 303（采纳）

**理由**：AcidBox 的 `SynthVoice Synth2(1)` 是完全独立的实例。已核查 `synthvoice.ino` 全文，仅 4 处 `static` 匹配且全为调试残留；`ParseCC()` 全部写成员变量（`_reso` / `_filter_freq` / `_pan` / `_slideMs` / `_tuning`），无共享状态。`getSample()`（synthvoice.ino:64）是纯成员函数。

AcidBox 的第二路 303 拥有 DRIFT 没有的：独立调音表（`CC_303_TUNING` → `tuning[128]`，synthvoice.h:207-210）、`CC_303_SATURATOR`、失真/过载双通道、增益补偿矩阵。

**保留 V5 的部分**：MIX EDIT（软件混音）、编辑焦点切换（`ch2SynthMode`）、双引擎并发的交互逻辑——这些与引擎实现无关，全部保留。

**放弃**：`ch2RenderSample()` 的 16 个子引擎（Drift.ino，633 行）。

### 决策 2：鼓机音源用 AcidBox 的 LittleFS 套件（采纳）

`data/` 下 8 套音色（`0`~`8`）共约 100 个 WAV，44.1 kHz。配合 `S3-regular` 默认开启的 `PRELOAD_ALL`（config.h:108，注释注明 *"recommended for OPI PSRAM of ESP32S3"*），`PSRAM_SAMPLER_CACHE` 3 MB（config.h:109）可实时切换无 glitch。

Acid_Drip 的 8 个内嵌采样是 PWM 时代产物，喂给 44.1 kHz 浮点引擎是浪费。

**代价**：需走 LittleFS 上传流程（[esp32-littlefs-upload](https://github.com/earlephilhower/esp32-littlefs-upload)）。

### 决策 3：MIDI 通路保留，不删（采纳）

`MIDI_USB_DEVICE` 在 `S3-regular` 的 config.h:28 默认开启。融合后 **pad 演奏与外部 MIDI 控参并存**——`midi_handler.ino` 的分发逻辑（按 channel 路由到 Synth1/Synth2/Drums）完全不动，新增的只是同一批函数的另一个调用入口。

**这是白捡的功能**：融合产物既能当独立乐器，也能挂进 DAW。

### 决策 4（延后）：V5 鼓机效果链暂不移植

V5 的 `bmRenderDrumSample()`（BeatMachine2.ino:2240+）是**纯整数定点**实现——TPT state-variable 滤波器积分器 `bmFiltIc1`/`bmFiltIc2`、`bmDriveSat()`、`>>8` 移位，为 8-bit PWM 总线设计。AcidBox 的 `Sampler` 是浮点、读 LittleFS。

V5 的 DJ filter + drive 饱和比 AcidBox 的 `FxFilterCrusher` 更有表现力，但重建定点链约需 2-3 天并需重新调参。**第一版用 AcidBox 原生 crusher**，此项列为可选增量。

---

## 3. 目标架构

```
┌──────────────────── CORE 0 ────────────────────┐
│  audio_task1   优先级 5（原为 1，须提高）        │
│  ┌──────────────────────────────────────────┐  │
│  │ 每块循环：                                 │  │
│  │   1. 消费事件队列（块边界）                 │  │
│  │   2. synth1_generate()  → Synth1 303      │  │
│  │   3. synth2_generate()  → Synth2 303      │  │
│  │   4. drums_generate()   → Sampler (PSRAM) │  │
│  │   5. mixer()  Delay + Reverb + Comp       │  │
│  │   6. i2s_output()  非阻塞 → PCM5102        │  │
│  └──────────────────────────────────────────┘  │
│  32 样本块 · 44.1 kHz · 双缓冲 · 0.73 ms/块     │
└────────────────────────────────────────────────┘
                        ↑ FreeRTOS Queue
                        │ 事件：note / noteOff / param
┌──────────────────── CORE 1 ────────────────────┐
│  ui_task   优先级 1                               │
│  ┌──────────────────────────────────────────┐  │
│  │ · 16 pad 扫描 + 消抖 + 手势识别            │  │
│  │ · 3 电位器读取（12-bit ADC1）             │  │
│  │ · TFT 渲染（分带清屏 bmFillScreenFed）     │  │
│  │ · 音序器时钟（micros 基准）                │  │
│  │ · NVS 存读                                 │  │
│  └──────────────────────────────────────────┘  │
│  Arduino loopTask 保留给 MIDI 轮询              │
└────────────────────────────────────────────────┘
```

**唯一跨界通道是事件队列。** 沿用 V5「core 0 永不碰 SPI」的纪律——该纪律源于 RP2040 的 XIP 争用教训，在 S3 上虽成因不同（优先级 vs 实时性），结论相同。

### 引脚预算（软件侧先行，硬件后续）

| 功能 | 数量 | 说明 |
|---|---|---|
| 16 pad | 16 | GPIO |
| 3 电位器 | 3 | **必须走 ADC1**（GPIO 1-10）；ADC2 与 WiFi 冲突，不可用 |
| TFT ILI9341 | 5 | SCK / MOSI / MISO / CS / DC / RST 中的 5 根 |
| I2S 输出 | 3 | BCLK / WCLK / DOUT |
| SYNC IN/OUT | 1 | 可与 I2S DOUT 复用（原板即如此） |
| **合计** | **28** | S3 可用约 35（扣 strapping 0/3/45/46 与 OPI PSRAM 占用的 26-32） |

---

## 4. 关键事实核对

以下均已逐条验证，标注了文件与行号。

### 4.1 AcidBox 侧

| 事实 | 位置 |
|---|---|
| 采样率 44100（非内置 DAC 路径） | config.h:56 |
| `DMA_BUF_LEN 32` → 块时长 0.726 ms | config.h:96 |
| `SAMPLECNT (7*12)` = 84 个采样位 | config.h:110 |
| `PRELOAD_ALL` 已开启，注明推荐 OPI PSRAM | config.h:108 |
| `MIDI_USB_DEVICE` 默认开启 | config.h:28 |
| `DEBUG_ON` 默认开启（**必须关**） | config.h:19 |
| S3 I2S 引脚 BCLK=5 DOUT=6 WCLK=7 | config.h:38-40 |
| S3 电位器引脚 {15,16,17} | config.h:41 |
| 音频任务优先级 **1**（与 loopTask 平级，须提） | AcidBox.ino:298-299 |
| `loop()` 目前近乎空闲，可放 UI 代码 | AcidBox.ino:333-340 |
| `mixer()` 已含 Delay + Reverb + 鼓侧链 Comp + `fast_shape` | AcidBox.ino:440-496 |
| `handleNoteOn` 按 channel 路由 Synth1/Synth2/Drums | midi_handler.ino:54-62 |
| MVA 重音判定：`velocity >= 80` | synthvoice.h:223 |
| 独立调音表 `tuning[128]` 经 `CC_303_TUNING`(104) | synthvoice.ino:207-210 |
| 滑音为**逐采样**插值（`_slideMs` + period 追赶） | synthvoice.h:120-133, synthvoice.ino:221-229 |

### 4.2 Acid_Drip 侧

| 事实 | 位置 |
|---|---|
| `Step` 结构仅 5 字节：`note/active/accent/glide/effect` | 主 sketch:792-798 |
| `Sequencer` 时基为**微秒**（`interval`/`lastUs`），非采样计数 | 主 sketch:800-817, 1318 |
| `advanceStep()` 是唯一时间推进点 | 主 sketch:2786 |
| `updateControl()` @ 256 Hz（`MOZZI_CONTROL_RATE`） | 主 sketch:113, 5105 |
| `updateAudio()` @ 16.384 kHz，**从不反向调用音序器** | 主 sketch:6472-6551 |
| 三条触发接缝：`triggerNote()` / `bmTriggerStep()` / `triggerCh2Pulse()` | 2120 / BeatMachine2:934 / — |
| `MOZZI_CONTROL_RATE` 的**唯一**依赖在滑音 tick 换算 | 主 sketch:2127 |
| V5 accent = `gEnvCutNorm * 2`（cutoff 扫描加倍） | 主 sketch:2147 |
| 8 种 step 级效果（OctUp/Retrig/Stutter/MajStep/MinStep/Dom7/DimStep） | 主 sketch:2115, 2926 |
| 鼓机为整数定点（TPT SVF，`bmFiltIc1/2`） | BeatMachine2.ino:2240-2281 |
| TFT 24 MHz、rotation 3 | 主 sketch:6566-6570 |
| `analogRead >> 2`（RP2040 10-bit）→ S3 需 `>> 4` | 主 sketch:5016, BeatMachine2:1259 |
| `bmFillScreenFed()` 分带清屏（防阻塞） | 主 sketch:218-226 |
| `BENCH_AUDIO_HEADROOM` 仪表（可复用） | 主 sketch:143-192 |
| `__not_in_flash_func` / `rp2040.fifo` / `rp2040.wdt_reset` 需删除 | 全局（S3 无 XIP） |

### 4.3 最重要的发现

**音序器时基已经是微秒制**（`seq.interval = bpm2us(seq.tempo)`，主 sketch:1318；`seq.lastUs` 为 micros）。这意味着音序器**本就不依赖采样率**，移植时无需重做时基——这消除了我此前判断的主要风险。

`MOZZI_CONTROL_RATE` 的唯一引用在滑音换算（主 sketch:2127）。而 AcidBox 的滑音是逐采样插值，**本项目在这一项上是升级而非移植**。

---

## 5. 里程碑

### M0 — 基线验证（0.5 天）

**目的**：建立可信基准线。没有这一步，后续无法区分「融合的效果」与「基线本来就不好」。

任务：
1. `git clone --depth 1 --branch S3-regular https://github.com/copych/AcidBox.git`
2. 烧录，确认出声（外部 DAC 或先用内置 DAC）
3. `config.h:19` 关闭 `DEBUG_ON`（作者注释：*"debugging eats ticks initially belonging to real-time tasks, so sound output will be spoiled in most cases"*）
4. `config.h:30` 注释掉 `MIDI_VIA_SERIAL2`（若未用）
5. 音频任务优先级 `1` → `5`（AcidBox.ino:298-299）
6. `i2s_write(..., portMAX_DELAY)` 改非阻塞（i2s_setup.ino:74, 80）
7. 移植 `BENCH_AUDIO_HEADROOM` 仪表，实测 core 0 余量

**验收**：稳定出声，无周期性爆音，串口打印 core 0 占用率 < 50%。

#### M0 实际执行结果（截至 2026-10-01）

源码已 vendored 进仓库 `firmware/AcidBox/`，CI 编译通道打通。**编译侧全部通过；上板验收仍待硬件。**

**已完成的代码改动（3 处，均带 `M0:` 注释）**

| # | 位置 | 改动 | 理由 |
|---|---|---|---|
| 1 | `config.h:19` | 注释掉 `DEBUG_ON` | 作者自己建议关闭 |
| 2 | `config.h` 约 147 行 | 重写 `#undef DEBUG_ON` 的守卫 | 见下 |
| 3 | `AcidBox.ino:301-302` | 任务优先级 `1` → `5` | 为后续 TFT/音序器留余量 |

**发现的两个上游缺陷**

1. **`config.h` 的守卫是无效的。** 原文：

   ```c
   #ifdef MIDI_VIA_SERIAL || MIDI_USB_DEVICE
     #undef DEBUG_ON
   #endif
   ```

   `#ifdef` 只接受一个标识符，GCC 会对 `|| MIDI_USB_DEVICE` 报 *extra tokens at end of #ifdef directive* 并丢弃。也就是说这个守卫**只测过 `MIDI_VIA_SERIAL`**——而它默认就是关的。结果是：即便开着 `MIDI_USB_DEVICE`，`DEBUG_ON` 依然生效，作者为它加的自动关闭机制从来没工作过。已按 `AcidBox.ino:36` 的正确写法重写。

2. **`i2s_write(..., portMAX_DELAY)` 在 core 3.x 上是死代码。** 那两行位于 `#if ESP_ARDUINO_VERSION_MAJOR < 3` 分支内。3.x 走 `I2S.write()`，默认超时为 0，本来就是非阻塞。M0 第 6 步因此**无需改动**，不是因为已经改了，而是因为那段代码不会被编译。

**此前一个错误推断已更正**：`JUKEBOX` 开着并不会造成 `setup()`/`loop()` 重复定义。`AcidBanger.ino` 里那两个函数（:1190 / :1294）位于 `/* ... */` 块注释中，该文件不定义任何符号。因此 M0 保留 `JUKEBOX` 开启以维持真正的上游基线；关闭它属于 M2（音序器接管之后）。

**构建通道：为什么是 CI 而不是本地**

本机无法安装 ESP32 core。`arduino-cli` 的工具链（xtensa-esp-elf 等，约 1.5 GB）从 `raw.githubusercontent.com` 拉取，而该地址在本网络被重置。改用 GitHub Actions，见 `.github/workflows/build.yml`。

**三个静默踩坑，均已修正并写入 workflow 注释**

| 坑 | 后果 |
|---|---|
| 上游 README 的 "No OTA (1MB APP/3MB SPIFFS)" 在 core 3.x 里叫 `noota_3g`，不是 2.x 的 `min_spiffs` | 照抄 2.x 教程会**编译通过但分区错误**，2.5 MB 音色装不下。已对 `boards.txt` @ `3.3.12` 核实 |
| `MIDI Library` 必须用 **5.x** | 4.x 的 `midi::MidiType` 只有 `SystemExclusive`，没有 `SystemExclusiveStart`/`End`，而 AcidBox vendored 的 `src/usbmidi` 依赖这两个名字，4.x **无法编译**。5.0.0 由 `lathoub`（vendored 传输层作者）共同署名，即其目标版本 |
| Library Manager 里的名字是 `MIDI Library`，不是仓库名 `arduino_midi_library` | 直接写仓库名会装不上 |

**已确认的三件事**

- **TinyUSB MIDI 是开启的。** `MIDIUSB_ESP32.h` 整个包在 `#if CONFIG_TINYUSB_MIDI_ENABLED` 里，为 0 时该类不存在，而 `USB-MIDI.h:84` 在 ESP32 上无条件调用 `MidiUSB.begin()`——编译能过，就证明该宏为 1。这条原本列为风险，现已闭环。
- **固件 616,880 字节，装得进 1 MB 分区。** 剩余 431,696 字节。M1–M4 的增量（音序器 + TFT + FX）需要留意这个余量。
  （此前这里写的"806 KB / 剩 223 KB"是错的：806 KB 是 GitHub **artifact 压缩包**的大小，不是固件大小。）
  （直接用 arduino-cli 的 `Sketch uses 616724 bytes (58%)` 也不对：那是 sketch 大小，`.bin` 另带 156 字节的 image header 和段对齐填充。**能不能装下看的是 `.bin` 的大小**，CI 的余量表现在按实际文件算，并把 arduino 那个数并排列出。）
- **8 套鼓组（2.55 MB）装得进 3 MB LittleFS 分区。** 原本只是估计装得下，现在 CI 已经从锁定的上游 commit 构建出 `littlefs.bin` 并通过。这解除了 R4 的一半。

**固件产物：CI 现在交付完整烧录包**

早先的 artifact **缺 `boot_app0.bin`**——`--export-binaries` 只导出 bootloader/partitions/firmware，而 `boot_app0.bin` 住在 core 的 `tools/partitions/` 里，不会被复制。而 otadata 分区不初始化，加上网上教程普遍写的 `0xe0000` 对本分区表是错的（`noota_3g.csv` 把 nvs 分成 0x5000 而非默认 0x4000，otadata 因此从 `0xd000` 上移到 `0xe000`），这个坑迟早要踩。现在 CI 会：

- 从 core 里复制出 `boot_app0.bin`，四个偏移量按 `platform.txt:349` 的上传配方写入 `flash-args.txt`（偏移量取自 platform.txt，不取自本文档或任何教程）
- 用 `mklittlefs` 从上游 `data/` 构建 `littlefs.bin`（core 本身不带这个工具），装不下就让 build 变红
- 把 app 分区与 LittleFS 分区两个余量写进 run 页面
- **再把四块内容拼成一个 4 MiB 的 `merged.bin`，地址 `0x0`**，浏览器烧写从此只有一个文件、一行、一个按钮

**`merged.bin` 为什么成立，以及它怎么被验证的**

`noota_3g` 在 `0x0` 到 `0x400000` 之间是**一段连续无空洞**的布局：

```
bootloader 0x000000 │ partitions 0x008000 │ nvs 0x009000 │ otadata 0x00E000
app0 0x010000 │ spiffs 0x110000 │ coredump 0x3F0000 → 结束于 0x400000
```

所以"五个文件五个地址"可以塌成"一个文件一个地址"。偏移量**从 core 自己的
`noota_3g.csv` 解析**（方案名取自 FQBN 的 `PartitionScheme=`，也就是 arduino-cli 真正编译用的值），
不写死——`0xe000` / `0xe0000` 这种坑正是靠这个躲掉的。

关键在于**验证**：arduino-cli 1.5.1 自己会额外产出一个整片 16 MiB 的
`AcidBox.ino.merged.bin`。那是工具链对同一个问题的答案，所以合并步骤拿它当基准，
逐字节比对我们放在 CSV 偏移上的 bootloader / 分区表 / otadata / app 四段，不一致就让
构建失败。run #14 已经通过（`0x0`、`0x8000`、`0xe000`、`0x10000` 四段全部 `agrees`）。
这把"CSV 里的偏移就是构建用的偏移"从假设变成了检查，而这是本项目最容易被教程带偏的一个数。

> 这个偏移交叉验证还顺带暴露了一件更基础的事：arduino-cli 的产物叫
> **`AcidBox.ino.bin`**，不是 `AcidBox.bin`，也不是 `firmware.bin`——它按
> `<工程文件名>.ino.<类型>.bin` 命名。三个名字连着猜错两次，才终于去读了构建目录。
> 现在按后缀识别（排除 `*.bootloader.bin` / `*.partitions.bin` / `*.merged.bin` /
> `boot_app0.bin`），并统一改名成 `bootloader.bin` / `partitions.bin` / `firmware.bin`，
> 让文档和 `flash-args.txt` 里的名字是真的躺在 artifact 里的名字。
> `tools/merge-image.py` 有 9 个用例的本地测试（`python tools/test-merge-image.py`），
> 其中一个专门把基准镜像的某个字节改坏，用来证明这个交叉检查真的会咬人、
> 而不是"因为什么都没比所以通过"。

块大小 4096 不是随便填的：`LittleFSFS::begin()` 只设了 `grow_on_mount = true`，块大小走 IDF 默认值。不匹配的后果特别恶劣——`FORMAT_LITTLEFS_IF_FAILED` 是 `true`，镜像挂不上不会报错，而是**静默格式化分区**，鼓组退回 `samples.h` 里那套 8-bit 内嵌采样，听起来"能动"，但已经不是 808 采样了。

LittleFS 的实际余量很紧，值得单独记一笔：

```
96 个文件，原始         2,673,230 字节
4 KB 块对齐后          2,863,104 字节  （块对齐本身吃掉 189,874 字节）
分区                   3,014,656 字节
名义余量                 151,552 字节
```

余量只剩约 148 KB，而且还没扣 LittleFS 的 inode 表和目录块。实测能装下，但**后面每加一套鼓组都要重新跑一次 CI 确认**。真装不下时按这个顺序处理：先砍鼓组（kit 1 最大，0.5 MB，光 `101_BD8.wav` 就 175 KB），再考虑换 16 MB flash + 自定义分区表。

**编译配置**

```
esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=noota_3g,FlashSize=16M
```

`PSRAM=opi` 是硬需求：`PRELOAD_ALL` 的 `PSRAM_SAMPLER_CACHE` 为 3 MB，QSPI 不够。`FlashSize=16M` 是推测值——不过 `noota_3g` 只用到 `0x400000`（4 MB），所以 8 MB flash 同样装得下，这个选项主要影响 esp-idf 写进镜像头的参数，功能上两者等价。`USBMode`/`CDCOnBoot` 保持 Arduino 默认不指定，原因见 `firmware/README.md`。

**硬件接线已定稿**：见 `HARDWARE_SETUP.md`。I2S 是 GPIO 5/6/7；PSRAM 必须是 OPI 且 ≥ 4 MB（`PSRAM_SAMPLER_CACHE` 3 MB，**R2 版本的板子直接跑不起来**）；M0 不需要接任何按键或电位器。

**仍未完成（阻塞于无硬件）**

- 烧录与出声验证
- `BENCH_AUDIO_HEADROOM` 仪表移植与 core 0 余量实测
- 上板确认 USB MIDI 实际枚举，以及板子真实的 `FlashSize`

本机无 ESP32-S3 在位：所有相关 PnP 条目（`VID_303A&PID_1001`、`USB-SERIAL CH340 (COM5)` 等）状态均为 Unknown，属残留记录，实际只有主板的 `COM1`。

---

### M1 — 事件接口层（1-2 天）

新建 `engine_iface.h` / `engine_iface.cpp`，把引擎调用包成语义接口：

```cpp
enum class Ch { Acid, Second, Drums };

void eng_noteOn(Ch ch, uint8_t note, uint8_t vel, bool accent);
void eng_noteOff(Ch ch, uint8_t note);
void eng_setParam(Ch ch, uint8_t cc, uint8_t val);
void eng_selectProgram(uint8_t prog);
void eng_allNotesOff();
```

实现要点：
- `eng_noteOn` 把 `accent` 布尔量映射为力度：`accent ? 127 : 79`（须 ≥ `mva_alloc` 的 80 门限，synthvoice.h:223）
- `midi_handler.ino` 保持不动，两个入口并存
- 队列用 FreeRTOS `Queue`，元素为小 POD（建议 `struct Ev { uint8_t op, ch, a, b; }`，4 字节）

**验收**：外部代码经此接口出声，与纯 MIDI 模式听感无差异。

---

### M2 — 音序器解耦（2 天）

**风险已降至低位**（见 4.3）。工作不是「解开耦合」，而是「把三个触发落点换成投递事件」。

任务：
1. 从 V5 提取 `Step` / `Sequencer` 结构与 `advanceStep()` / `nextPatStep()`（主 sketch:2713-2784）
2. 建立 `seq_clock`，跑在 ui_task，时基沿用 `micros()`
3. 三个接缝改造：
   - `triggerNote()`（2120）→ `eng_noteOn(Ch::Acid, ...)`
   - `bmTriggerStep()`（BeatMachine2:934）→ `eng_noteOn(Ch::Drums, ...)`
   - `triggerCh2Pulse()` → `eng_noteOn(Ch::Second, ...)`
4. **滑音改造**：V5 的 `gGlideStep` 每 3.9 ms 走一步（2124-2138）换成 AcidBox 的 `CC_303_PORTAMENTO`(65) 开关 + `CC_303_PORTATIME`(5) 时间。`gPortaSpeed` 1-8 映射到 slide time 建议 `{200,150,110,80,60,45,30,20}` ms
5. **accent 语义对齐**（需决策）：V5 的「重音 = cutoff 扫描加倍」（2147）在 AcidBox 里对应 `CC_303_ENVMOD_LVL`；V5 的「重音 = 衰减更长」（`gAccentActive`）对应 `CC_303_ACCENT_LVL`。建议保留力度门控（AcidBox 机制），深度交给 `CC_303_ACCENT_LVL`
6. `updateControl()` 256 Hz → 改由 ui_task 自由调度（不再受 `MOZZI_CONTROL_RATE` 约束）

**验收**：16 步音序器跑通，连续播放 10 分钟无音高漂移、无时序抖动累积。

---

### M2.5 — Step 效果移植（1 天）

V5 的 8 种 step 级效果是**纯逻辑代码**，不依赖任何硬件 DSP，可直接搬到事件层。在音序器触发前算出实际音高/时值，映射成 note on/off 序列：

| 效果 | 映射 |
|---|---|
| None | 单次触发 |
| OctUp | 触发 note 与 note+12 |
| Retrigger | 短时值内重复触发 |
| Stutter | 按步长重复若干次（时值细分） |
| MajStep / MinStep / Dom7Step / DimStep | 根音 + 和弦音偏移（音程表见主 sketch:2176-2210） |

**验收**：8 种效果在音序器中可用，与 V5 行为一致。

---

### M3 — UI 层（3-4 天）

任务：
1. **pad 扫描**：16 GPIO 消抖 + 手势识别（短按 / 长按 ×1 / 长按 ×2 / 和弦）。逻辑为纯 C，可原样复用
2. **TFT 渲染**：移植全部绘制路径，**必须用 `bmFillScreenFed()` 分带清屏**。V5 注释记录了一次 `fillScreen()` 原子阻塞 ~103 ms 的教训（主 sketch:222-223）
3. **电位器**：`analogRead >> 2` → `>> 4`（S3 为 12-bit）。主 sketch:5016、BeatMachine2:1259
4. **删除平台耦合**：
   - 所有 `__not_in_flash_func` → 改 `IRAM_ATTR`（S3 为 Harvard 架构，无 XIP 争用）
   - `rp2040.fifo` → FreeRTOS Queue / task notification
   - `rp2040.wdt_reset()` → `esp_task_wdt_reset()` 或改用定时喂狗
   - `#include "pico/time.h"` → `<esp_timer.h>`，`time_us_32()` → `esp_timer_get_time()`
5. **任务编排**：ui_task 优先级 1，独立栈（建议 8 KB），永不与音频任务同核

**验收**：V5 全部手势可用（PLAY/PLAY 长按恢复出厂、FUNC 切换、FX 分配、ACCENT EDIT、ACID WALKS）。

---

### M4 — 持久化（1-2 天）

`EEPROM` → `NVS`。

V5 用的持久化点（主 sketch内 `EEPROM.put/get/commit`）：

| 数据 | 结构 | 位置 |
|---|---|---|
| Acid 图案 4 槽 | `Patch` | 957-962 |
| 重音设置 | `AccentSettings` | 973-978 |
| Ch2 设置 | `Ch2Settings` | 988-995 |
| Ch2 扩展 | `Ch2SettingsX` | 1015-1028 |
| 欧几里得槽 | `Ch2EucSlots` | 1070-1075 |
| MIX | `MixSettings` | 1090-1095 |
| 滤波 | `FilterSettings` | 1104-1109 |

要点：
- `EEPROM.commit()` 在 RP2040 上是整扇区擦除，会冻结双核。V5 已用 `saveCommit` 标志把写入推迟到 core 1（主 sketch:3178, 3365）。**这个纪律必须保留**，NVS commit 同样不能发生在音频任务
- NVS 单次写入有磨损限制，合并写入而非每次变更都提交

**验收**：4 槽存取正常、pattern chaining 正常、ACID WALKS 持久、掉电恢复出厂设置可用。

---

### M5 — 鼓机效果链（可选，2-3 天）

重建 V5 的定点效果链到浮点域：TPT SVF DJ filter、drive 饱和、bitcrush 融合进 `mixer()`。需重新调参。

---

## 6. 风险登记册

### R1 — 任务优先级地雷（高，必须最先解决）

AcidBox 现为 `xTaskCreatePinnedToCore(audio_task1, ..., 1, &SynthTask1, 0)`（AcidBox.ino:298），**优先级 1，与 Arduino `loopTask` 平级**。当前 `loop()` 为空所以无碍；一旦塞入 TFT 全屏重绘（SPI 逐像素，单次数十 ms），将与音频任务时间片互抢，直接 underrun 爆音。

**对策**：M0 即处理。音频任务提至优先级 5，UI 任务降至 1，`i2s_write` 去 `portMAX_DELAY`。

### R2 — 块边界量化误差（中，可接受）

事件在块边界生效，`32 / 44100 = 0.726 ms`。

| 场景 | 时长 | 相对误差 |
|---|---|---|
| 130 BPM 16 分音符 | 115 ms | 0.63% |
| 32 分音符踩镲 | 58 ms | 1.26% |

**对策**：若实测偏紧，将 `DMA_BUF_LEN` 降至 16。代价是 `mixer()` 的 per-block 开销翻倍（`fast_shape` 查表、`Delay.Process`），需实测后再定。

### R3 — 电位器必须用 ADC1（高，硬件相关）

ESP32 的 ADC2 与 WiFi 冲突，ADC3 已被 WiFi 占用。**只能用 ADC1（GPIO 1-10）**。3 个电位器须落在 GPIO 1-10 内。

### R4 — LittleFS 上传流程（低，镜像已自动化）

分区方案 `noota_3g`（No OTA, 1 MB APP / 3 MB SPIFFS）与 `PSRAM_SAMPLER_CACHE` 的 3 MB 相配。

镜像构建**已自动化**：CI 从锁定的上游 commit 浅克隆 `data/`，用 `mklittlefs` 4.1.0 构建 `littlefs.bin` 并作为独立 artifact `AcidDripS3-littlefs` 交付。core 本身不带 `mklittlefs`，这一步要自己下载预编译二进制。

剩下的只有"上板刷镜像"这一下人工操作，偏移 `0x110000`。三个注意点：

1. 分区表标签是 **`spiffs`**，不是 `littlefs`，工具参数要给对。
2. 块大小必须 4096，与 `LittleFSFS::begin()` 的 IDF 默认值一致。
3. `FORMAT_LITTLEFS_IF_FAILED` 是 `true`：**镜像挂不上不会报错，会静默格式化分区**。所以固件和镜像必须配套刷，症状是"鼓变 8-bit 了"而不是任何错误提示。

### R5 — 鼓机效果缺失（低，已接受）

见决策 4。第一版用 AcidBox 原生 `FxFilterCrusher`。

### R6 — 原 PCB 模拟混音电位器失效（硬件相关）

原板将 GP15(acid) 与 GP2(drums) 走硬件混合到一颗 pot。改单路 I2S 后该 pot 失去作用。

**建议改派为第 4 个旋钮**（推荐 filter env mod 或第二引擎音量）——V5 已有 MIX EDIT 软件混音，该 pot 的原始功能已被覆盖。

---

## 7. 参数映射表

V5 旋钮 / 设置 → AcidBox CC（GM_MIDI，midi_config.h:9-25）：

| V5 | 目标 | CC | 值域 |
|---|---|---|---|
| CUT pot | Acid cutoff | 74 | 0-127 |
| RES pot | Acid reso | 71 | 0-127 |
| DECY pot | Acid env decay | 72 | 0-127 |
| accent 深度 | Acid accent level | 76 | 0-127 |
| 滑音开关 | Portamento on/off | 65 | ≥64 开 |
| 滑音时间 1-8 | Portamento time | 5 | 0-127 (ms) |
| SOUND pad | 波形（方波/锯） | 70 | ≥64 锯 |
| KEY pad | 独立调音 | 104 | 调音表索引 |
| 鼓音量 | Drum volume | 7 | 0-127 |
| 鼓 BD 音色 | BD tone | 21 | 0-127 |
| 鼓 SD 音色 | SD tone | 25 | 0-127 |
| 鼓 CH 调音 | CH tune | 61 | 0-127 |
| 鼓 OH 调音 | OH tune | 80 | 0-127 |
| Delay 发送 | Delay send | 92 | 0-127 |
| Reverb 发送 | Reverb send | 91 | 0-127 |
| 压缩比 | Compressor | 93 | 0-127 |
| — | 失真 | 94 | 0-127 |
| — | 过载 | 95 | 0-127 |
| — | Saturator | 128 | 0-127 |
| — | 声像 | 10 | 0-127 |

通道约定（config.h:104-106）：`SYNTH1_MIDI_CHAN 1` / `SYNTH2_MIDI_CHAN 2` / `DRUM_MIDI_CHAN 10`。

---

## 8. 明确不做的事

1. **不移植 Mozzi**。不重建 ESP32 版 Mozzi 兼容层。
2. **不移植 DRIFT 的 16 个子引擎**（Drift.ino，633 行）。
3. **第一版不做鼓机效果链**（DJ filter / drive 饱和）。
4. **不动 PCB**（本轮范围仅软件）。
5. **不删 MIDI 通路**，两个输入源并存。
6. **不改 `mixer()` 的总线拓扑**（Delay / Reverb / 鼓侧链压缩保持 AcidBox 原样）。
7. **删除 MIX EDIT 的 Q8 后置配平**（决策 6）：`mixAcidGainQ8` / `mixDriftGainQ8` / `mixDrumGainQ8` 全部机制，含主 sketch:6497-6507 的软削波补偿分支。
8. **删除 SYNC 模式整体**（决策 7）：`syncMode` 分支、主 sketch:6541-6550 的三引擎折混音、`bmStartAudio()` 跳过逻辑、开机 pad-14 模式检测。SYNC IN/OUT 保留为常驻功能。
9. **不实现第 4 个旋钮**（决策 8）：软件侧只预留 `paramChange()` 的 `case 4`，待硬件轮次。

---

## 9. 风险与工作量汇总

| 里程碑 | 工作量 | 风险 | 依赖 |
|---|---|---|---|
| M0 基线 | 0.5 天 | 低 | — |
| M1 事件层 | 1-2 天 | 低 | M0 |
| M2 音序器解耦 | 2 天 | **低**（4.3 已降级） | M1 |
| M2.5 Step 效果 | 1 天 | 低 | M2 |
| M3 UI | 3-4 天 | 中（TFT/ADC1） | M2 |
| M4 NVS | 1-2 天 | 低 | M3 |
| M5 鼓机效果（可选） | 2-3 天 | 中（需调参） | M4 |
| **软件合计** | **8-12 天** | | |

关键路径：M0 → M1 → M2 → M2.5 → M3 → M4。M5 不在关键路径上。

---

## 10. 附录：预期改动清单

### 新增文件
```
engine_iface.h / engine_iface.cpp    事件接口层
seq_core.h / seq_core.cpp            音序器状态机（自 V5 提取）
ui_pads.cpp                          pad 扫描 + 手势
ui_tft.cpp                           TFT 渲染（分带清屏）
storage_nvs.cpp                      NVS 封装
step_fx.cpp                          8 种 step 效果
```

### 需修改的 AcidBox 文件
```
config.h         关闭 DEBUG_ON(line 19)；确认 MIDI 通路配置
AcidBox.ino      任务优先级 1→5 (line 298-299)；新增 ui_task 创建与事件队列消费
i2s_setup.ino    i2s_write 去掉 portMAX_DELAY (line 74, 80)
```

### 需删除的 V5 平台耦合
```
所有 __not_in_flash_func      → IRAM_ATTR
rp2040.fifo                    → FreeRTOS Queue
rp2040.wdt_reset()             → esp_task_wdt_reset()
#include "pico/time.h"          → <esp_timer.h>
time_us_32()                   → esp_timer_get_time()
analogRead(...) >> 2           → >> 4
MozziConfigValues.h / Mozzi.h  → 全部删除
```

### 需删除的 V5 逻辑（决策 6、7）
```
syncMode 标志与所有分支                    决策 7
updateAudio() 的三引擎折混音 6541-6550     决策 7  → 简化为单一 303 输出
bmStartAudio() 的 sync 跳过路径 6649       决策 7
开机 SYNC 模式检测（pad 14）              决策 7  → 改运行时切换
mixAcidGainQ8 / mixDriftGainQ8             决策 6  → 改写 per-voice volume CC
mixDrumGainQ8 及其软削波补偿 6497-6507     决策 6  → 删除
MixSettings 结构 1090-1095                 决策 6  → 改为 3 个 7-bit 值
accent 的 resonance 扫描加强项 2148        决策 5  → 由 CC_303_RESO 承担
gGlideStep / gTarget / gFreqFP 2134-2143   决策 5  → 改用 CC_303_PORTAMENTO(65) + CC_303_PORTATIME(5)
```

---

## 11. 设计决策（已定）

以下四项经分析后确定，不再作为待办。

---

### 决策 5：accent 语义 — 直接用力度门控 + `CC_303_ACCENT_LVL`，零翻译层

#### 事实核对

V5 的 accent 一次性做四件事（主 sketch:2146-2149）：

| V5 行为 | 代码 |
|---|---|
| 重音音量高于满量程（正常则微降） | `gVolSub = accent ? -60 : 30` |
| cutoff 扫描加倍 | `gEnvCutoff = accent ? min(gEnvCutNorm*2, 255) : gEnvCutNorm` |
| resonance 扫描加强 | `gEnvRes = accent ? ACCENT_RES_FIXED : ENV_RES_NORMAL` |
| 滤波包络衰减变慢（303 wah） | `gAccentActive = accent` |

AcidBox 的 accent 实现（synthvoice.ino:318-345）：

```cpp
if (_accent) {
  _accentation = _accentLevel;                     // cutoff 扫描深度
  AmpEnv.setReleaseTimeMs(_ampReleaseMs * 50.0f);  // 释放延长 50×
  FltEnv.setDecayTimeMs(_filterAccentDecayMs);     // 专用 accent 衰减
  FltEnv.setAttackTimeMs(_filterAccentAttackMs);   // 专用 accent 起音
} else {
  _accentation = 0.0f;
  ...
}
_k_acc = (1.0f + 0.6f * _accentation);              // 幅度提升
```

而 `_accentation` 在 `getSample()`（synthvoice.ino:76）进入滤波扫描乘数：

```cpp
final_cut = _filter_freq_cut * (0.8f + (_envMod+0.1f) * (3*filtEnv - 0.3f) * (_accentation + 0.2f));
```

#### 对照

| V5 语义 | AcidBox 对应 | 覆盖度 |
|---|---|---|
| cutoff 扫描 ×2 | `(_accentation + 0.2f)`：0.2 → 1.2，**6× 深度** | 超出 |
| 衰减变慢 | `_filterAccentDecayMs` 独立参数 | 1:1 |
| 音量提升 | `_k_acc = 1 + 0.6 × _accentation`（最高 1.6×） | 1:1 |
| resonance 扫描加强 | 无独立参数（`CC_303_RESO` 是常态 reso） | **缺失** |

**结论：AcidBox 的 accent 是 V5 的超集**，除「resonance 扫描加强」外全部覆盖，且幅度更大。

#### 决策

- 力度映射：`accent ? 127 : 79`（须跨过 `mva_alloc` 的 `velocity >= 80` 门限，synthvoice.ino:223）
- 深度：accent 深度交给 `CC_303_ACCENT_LVL`(76)，由 ACCENT EDIT 模式的 CUT/RES/DCY 三旋钮写入（沿用 V5 的交互）
- **V5 的 resonance 扫描加强项放弃**，改由 `CC_303_RESO`(71) 常态 reso 承担。理由：Accent Edit 模式的 RES 旋钮改为直接调 `CC_303_ACCENT_LVL` 的深度，语义更清晰；且 303 原厂也没有「重音时额外加谐振」这一档
- **不写任何 accent 翻译层**——直接调力度

#### 利弊

- 利：省掉一层映射代码；AcidBox 的 6× 扫描深度比 V5 的 2× 更有 303 味；释放 50× 延长是原厂行为
- 弊：失去「重音时额外谐振」这一档；`_accentation` 上限由 `CC_303_ACCENT_LVL` 的 0-127 映射决定，映射曲线需实测调校

---

### 决策 6：MIX EDIT — 改为控制 per-voice volume CC，删除 Q8 trim 机制

#### 事实核对

V5 的 MIX EDIT 是三个 Q8 后置配平（应用在混音之后）：

```cpp
mixAcidGainQ8   // 6497-6505：乘完还要过软削波（6505-6506），否则 boost 会 buzz
mixDriftGainQ8  // 6542
mixDrumGainQ8   // BeatMachine2:2279
```

之所以要软削波，注释写得很清楚：gain > 1.0 除了硬削波无处可去，而硬削波会 buzz（主 sketch:6499-6504）。

AcidBox 的 `mixer()`（AcidBox.ino:440-496）已有：

| 能力 | 接口 |
|---|---|
| per-voice 音量 | `CC_303_VOLUME`(7) / `CC_808_VOLUME`(7) |
| per-voice 声像 | `CC_303_PAN`(10) / `CC_808_NOTE_PAN`(8) |
| 总线效果发送 | `CC_303_DELAY_SEND`(92) / `CC_303_REVERB_SEND`(91) |

**这些是混音前的独立增益，不需要后置软削波。**

#### 决策

- MIX EDIT 改为写三路 volume CC：Acid(7) / Second(7 on ch2) / Drums(7 on ch10)
- **删除** `mixAcidGainQ8` / `mixDriftGainQ8` / `mixDrumGainQ8` 全部机制，含 6497-6507 的软削波补偿分支
- MIX EDIT 界面（模式、槽位、持久化）**全部保留**，只换落点

#### 利弊

- 利：删掉约 30 行含移位和分支的定点代码；消除「boost 时 buzz」这个 V5 自己记录在案的缺陷（V5 的软削波是妥协方案）；混音前的 gain 更干净
- 弊：per-voice volume 在 `mixer()` 里作用于该 voice 的输出增益，与「鼓侧链压缩」（`Comp.Process(drums_out_l*0.25f)`，AcidBox.ino:482）的作用点不同，平衡时的听感会与 V5 略有差异，需试听校准
- 弊：MIX EDIT 的持久化结构（`MixSettings`，主 sketch:1090-1095）需改为存 3 个 7-bit 值

---

### 决策 7：SYNC 模式 — 删除折混音，保留 IN/OUT 作为常驻功能

#### 事实核对

V5 的 SYNC 模式（主 sketch:6521-6550）本质是**引脚冲突的产物**：

- GP2 同时承担「drums+DRIFT 的 PWM 音频输出」和「SYNC 时钟信号」
- 模式开启时 `bmStartAudio()` 被跳过（6649）
- 于是 `updateAudio()` 把 DRIFT + drums 折到 GP15 上（6541-6544），并让鼓触发改在 core 0 跑（注释见 6522-6528）

**换到 AcidBox 后，这条约束完全消失**：单路 I2S 输出，无引脚竞争，无双核分工问题。

但要注意：**SYNC IN 本身是个有用的功能**（外部设备时钟驱动音序器），`advanceStep()` 由外部脉冲而非内部时钟推进，这段逻辑与平台无关。原 SYNC OUT（`digitalWrite` 输出每步门脉冲，主 sketch:1382-1396）同样与平台无关。

#### 决策

- **删除**：SYNC 模式整体（`syncMode` 分支、6541-6550 的三引擎折混音、`bmStartAudio()` 跳过逻辑、开机 pad 检测）
- **保留**：SYNC IN（切换音序器时钟源）与 SYNC OUT（每步门脉冲输出）改为**常驻功能**，不需要模式开关
- 开机不再有「按住 pad 14 进入 SYNC 菜单」，SYNC 源改为运行时切换（FUNC 页内一项，或长按某 pad）

#### 利弊

- 利：删掉一整条折混音分支（`updateAudio()` 简化成一个 303 输出）；不需要开机 pad 检测；两路 SYNC 变成随时可用
- 弊：丢失 SYNC IN/OUT 的方向选择界面，需在 FUNC 页新增一个选项（原本在开机菜单里）
- 弊：SYNC OUT 需占 1 个 GPIO（原本复用 GP2，现在要独立分配）

---

### 决策 8：第 4 个旋钮 → Filter Env Mod（`CC_303_ENVMOD_LVL`, CC 75）

#### 事实核对

原 PCB 的这颗 pot 是硬件混合 GP15(acid) 与 GP2(drums) 的模拟节点（主 sketch:1402-1404）。改单路 I2S 后其功能被 MIX EDIT（决策 6）完全覆盖，故闲置。

`config.h:41` 现定 `POT_NUM 3`，`POT_PINS[3] = {15, 16, 17}`，对应 `paramChange()`（AcidBox.ino:363-388）里的 4 个 case。**`CC_303_ENVMOD_LVL` 当前完全没有旋钮入口**，只能靠 MIDI。

#### 决策

- 第 4 个旋钮 = Acid 引擎的 filter env mod（CC 75），作用 `Synth1`
- 接线约束：必须落在 **ADC1（GPIO 1-10）** 内，见风险 R3

#### 候选对比

| 候选 | 评价 |
|---|---|
| **Filter env mod (75)** | ✅ 采纳。303 里仅次于 cutoff 的表现力参数，当前无旋钮入口，且不与现有 3 颗重复 |
| Second 引擎音量 | ❌ 与 MIX EDIT（决策 6）功能重叠 |
| Master reverb/delay 发送 | ⚠️ AcidBox 只有 per-voice send（91/92），无 master send，需改 `mixer()` 才能加 |
| Delay 时间 (CC_ANY_DELAY_TIME, 84) | ⚠️ 可行但不如 env mod 常用 |

#### 利弊

- 利：补上一个高频参数的直接控制；`paramChange()` 加一个 case 即可（`case 4:` 已预留，AcidBox.ino:383-385）
- 弊：依赖硬件轮次（引脚数、PCB 是否加电位器），本轮软件侧预留但不实现
- 弊：env mod 拉高会显著改变音色，与 CUT 旋钮存在耦合（V5 注释 2147 已指出：*"303 cutoff sweep (needs CUT pot low)"*），两个旋钮需配合提示

---

## 12. 决策汇总

| # | 决策 | 状态 | 关键收益 |
|---|---|---|---|
| 1 | DRIFT → 第二台 303 | 已定 | 零成本，性能更优 |
| 2 | 鼓机用 LittleFS 套件 | 已定 | 84 采样 / 44.1kHz |
| 3 | 保留 MIDI 通路 | 已定 | 白捡 DAW 接入 |
| 4 | 鼓机效果链延后 | 已定 | 避免 2-3 天重调参 |
| 5 | accent 用力度门控 | **本轮定** | 零翻译层，6× 扫描深度 |
| 6 | MIX EDIT → per-voice CC | **本轮定** | 删 30 行定点 + 消除 boost buzz |
| 7 | 删 SYNC 模式，留 IN/OUT | **本轮定** | 简化 `updateAudio()` |
| 8 | 第 4 旋钮 = env mod | **本轮定** | 补高频参数（待硬件轮次） |

**决策 5-7 净删除约 60-80 行含定点移位与分支的代码**，且消除了两处 V5 自己记录在案的缺陷（accent boost buzz、SYNC 折混音的跨核约束）。
