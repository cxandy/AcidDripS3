# M0 硬件接线与首次上电

> 适用于当前 M0 基线（`firmware/AcidBox`，AcidBox v.1.5.0 S3，未改音序部分）
> 目标板：ESP32-S3 DevKitC-1 / ESP32-S3-DevKitC / 同类 S3 开发板

M0 阶段**只验证上游原样能不能跑通**，所以接线只有一条 DAC 线 + USB，没有屏幕、没有按键、没有电位器。

---

## 1. 硬件清单

| 器件 | 要求 | 说明 |
|---|---|---|
| ESP32-S3 开发板 | **OPI PSRAM，PSRAM ≥ 4 MB** | 见下方红字，这是最硬的约束 |
| I2S DAC | PCM5102A 模块（推荐） | S3 没有内置 DAC，必须外挂 |
| 功放 | 任意立体声功放 / 有源音箱 / 耳机功放 | PCM5102 是线路电平，**不能直接带喇叭** |
| USB 线 | 1–2 根 | 取决于板子上有几个 USB 口，见 §4 |

### 必须确认的两件事

**（1）PSRAM 必须是 OPI 的，且容量够**

`firmware/AcidBox/config.h`：

```c
#define PRELOAD_ALL                          // :108
#define PSRAM_SAMPLER_CACHE 3145728          // :109  = 3 MB
```

3 MB 采样缓存 + OPI PSRAM 的分配开销，**实际上要 4 MB 以上才稳**。
所以 FQBN 里 `PSRAM=opi` 不是可选项，`PSRAM=enabled`（QSPI）也不是。

常见型号：

| 型号 | Flash / PSRAM | 能否跑 |
|---|---|---|
| ESP32-S3-DevKitC-1 **N8R8** | 8 MB / 8 MB | ✅ 推荐 |
| ESP32-S3-DevKitC-1 **N16R8** | 16 MB / 8 MB | ✅（CI 就是按 16M Flash 编的） |
| ESP32-S3-DevKitC-1 **N8R2** | 8 MB / 2 MB | ❌ 2 MB 不够 |
| ESP32-S3-WROOM-1 N8R2 / N16R2 | 2 MB PSRAM | ❌ |

买到的是 R2（2 MB PSRAM）版本的话，只能退回 `#define NO_PSRAM` 那条老路——采样降级、混响砍掉、只有 8 个采样，M1 之后基本没法用。**先看清楚模组丝印。**

**（2）FlashSize 要和板子一致**

CI 里写死 `FlashSize=16M`。`noota_3g` 分区表总用到 `0x400000`（4 MB），所以 **8 MB / 16 MB 都能装下**，FlashSize 选项主要影响 esp-idf 生成的头部参数。如果你板子是 8 MB，把 CI 的 `FlashSize` 改成 `8M` 重跑一次更保险——但功能上两者等价。

---

## 2. 引脚表

全部来自 `firmware/AcidBox/config.h` 的 S3 分支：

```c
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define I2S_BCLK_PIN    5       // :38
#define I2S_DOUT_PIN    6       // :39   ← 这是 ESP32 的输出，接到 DAC 的 DIN
#define I2S_WCLK_PIN    7       // :40
const uint8_t POT_PINS[POT_NUM] = {15, 16, 17};   // :41
#endif
```

### 要接的 3 根信号线

| PCM5102A 丝印 | 常见别名 | ESP32-S3 |
|---|---|---|
| `BCK` | `SCK` / `CLK` / `BCL` | **GPIO 5** |
| `DIN` | `SD` / `DATA` / `D` | **GPIO 6** |
| `LRC` | `WS` / `WCLK` / `LCK` | **GPIO 7** |

`I2S_DOUT_PIN` 名字容易看反：它是 ESP32 → DAC 的方向，所以接 DAC 的 **`DIN`**（不是 `DOUT`）。DAC 的 `DOUT` 那边是音频输出，别接回去。

### 不用接的

| DAC 丝印 | 原因 |
|---|---|
| `MCLK` / `SCK2` | PCM5102 有内部 PLL，会从 BCK + LRC 恢复主时钟，外部 MCLK 可选，留空即可 |
| `GND` / `3V3` / `1V8`（模式选择脚） | 模块上一般已经固定或悬空，不要动 |

### 不要往上接东西的 GPIO

| GPIO | 为什么 |
|---|---|
| **15 / 16 / 17** | `POT_PINS`。虽然 `TEST_POTS` 关着、`readPots()` 不会被调用，但 `AcidBox.ino:267` 的 `pinMode()` **是无条件执行的**。这个版本别接电位器 |
| **0** | Strapping pin（BOOT 键），同时是 Jukebox 的 `PLAY_BUTTON`（`AcidBanger.ino:59`） |
| **19 / 20** | 原生 USB（USB-OTG），USB MIDI 走这里 |
| **26 – 37** | OPI PSRAM 在 WROOM 模组内部焊死占用 |
| **43 / 44** | UART0 TX/RX，如果要用硬件串口 |

### 按键也不接

`AcidBanger.ino:55-64` 把除 `PLAY_BUTTON` 外的所有按键都塞在 GPIO 23 上。
但 `config.h:9` 有 `#define JUKEBOX_PLAY_ON_START`，**上电就自动开始放**，不需要按键。
M0 阶段不接任何按键。

---

## 3. 接线表

### ESP32-S3 → PCM5102A

```
ESP32-S3                          PCM5102A
────────                          ─────────
3V3          ──────────────────►  VCC
GND          ──────────────────►  GND
GPIO5        ──────────────────►  BCK
GPIO6        ──────────────────►  DIN
GPIO7        ──────────────────►  LRC
(nc)         ──────────────────►  MCLK      ← 悬空
```

### PCM5102A → 功放

```
PCM5102A              功放 / 有源音箱
────────              ──────────────
L (或 OUT_L)     ──►  IN_L  (左声道输入)
R (或 OUT_R)     ──►  IN_R  (右声道输入)
GND              ──►  GND
```

如果是耳机：PCM5102 → 耳机功放模块（如 PAM8403 立体声版）→ 耳机。
如果是落地测试：PCM5102 → 有源音箱的 Line In。

### 供电

- DAC 用 **3V3**，不要用 5V。PCM5102A 本体是 3.3 V 器件，贵一点的模块板上带 LDO 能吃 5 V，便宜的不一定，3V3 最稳。
- **ESP32 和 DAC 必须共地**，用同一根 USB 供电最省事，能顺带消掉地环路嗡声。

---

## 4. USB：板子上有两个口

`config.h:6` 有 `#define BOARD_HAS_UART_CHIP`，于是 `:128-131`：

```c
#define MIDI_PORT_TYPE HardwareSerial
#define MIDI_PORT      Serial
#define DEBUG_PORT     Serial
```

`Serial` 在 S3 上就是 **UART0**，走板载 USB-UART 桥。而 `MIDI_USB_DEVICE` 走 TinyUSB，走**原生 USB 口**。所以两个口各干一件事：

| USB 口 | 芯片 | 干什么 | M0 里有没有用 |
|---|---|---|---|
| **USB / USB-OTG**（GPIO19/20） | ESP32-S3 内置 | USB MIDI（设备名 `AcidBox S3`，VID `1209` / PID `1305`） | ✅ 用来验 MIDI |
| **UART** | 板载 CH343 / CP2102 等 | 烧录（Serial/JTAG） | ✅ 烧录必需 |
| 原生 USB | — | `DEBUG_PORT` 输出 | ❌ M0 见下 |

> ESP32-S3-DevKitC-1 上两个口丝印是 `USB` 和 `UART`。别插错。

### ⚠️ 关于串口日志：M0 阶段是空的

`firmware/AcidBox/config.h:19` 把 `DEBUG_ON` 注释掉了（这是 M0 改动之一），
所以 `DEB()` / `DEBF()` / `DEBUG()` 三个宏全部展开为空。
全工程只有两处串口调用，都在宏后面（`AcidBox.ino:251`、`midi_handler.ino:4`）。

**结果：M0 上电后串口一个字节都不会输出。**
验收只能靠耳朵 + MIDI 活动监视器。要看日志的话把 `DEBUG_ON` 打开重编，但那会吃掉实时任务的 tick（作者自己的注释就这么警告），正式验收前记得关掉。

---

## 5. 烧录

### 5.1 进下载模式

**按住 `BOOT` → 点一下 `RST` → 松开 `BOOT`**。
之后 COM 口应该出现一个设备（Windows 上叫 `USB JTAG/serial debug unit` 之类）。

板子上如果只有一个 USB 口，只能用它烧录，USB MIDI 就用不了——那种板子要先解决 `USBMode` 的问题再说。

### 5.2 拿产物

从 GitHub Actions 最近一次 green 里下 **一个** artifact：

| artifact | 内容 | 什么时候用 |
|---|---|---|
| **`AcidDripS3-merged`** | **`merged.bin`（4 MiB，一个文件）+ `README.md`** | **平时就用它，见 §5.5** |
| `AcidDripS3-firmware` | `bootloader.bin`、`partitions.bin`、`AcidBox.bin`、`boot_app0.bin`、`flash-args.txt`、`SHA256SUMS.txt` | 要单独重刷某一块，或想在命令行里刷 |
| `AcidDripS3-littlefs` | `littlefs.bin`（鼓组音色原始镜像，见 §6） | 同上 |

`--export-binaries` 会把三个 `.bin` 放进构建目录；`boot_app0.bin` 来自 core 内部的
`tools/partitions/`，CI 里的*Assemble the flash bundle* 步骤负责把它复制进来。
四个偏移量也由那一步写进 `flash-args.txt`，**不用照抄任何网上的教程**（包括本文档）。

不想装 esptool 就走 §5.5 的浏览器烧写。

### 5.3 装 esptool

本机没有 Arduino IDE 的 esptool，有 Python 3.14：

```powershell
pip install esptool
```

esp32 core 3.3.12 自带的是 **esptool 5.3.1**，所以命令是**连字符**的 `write-flash`。
用 `python -m esptool` 调用就与脚本名无关，最稳。

### 5.4 烧录命令

artifact 里的 `flash-args.txt` 就是可以直接粘贴的那一整条命令，长这样：

```powershell
python -m esptool --chip esp32s3 --port COM5 --baud 921600 `
  --before default-reset --after hard-reset `
  write-flash --flash-mode keep --flash-freq keep --flash-size keep `
  0x0 bootloader.bin 0x8000 partitions.bin `
  0xe000 boot_app0.bin 0x10000 AcidBox.bin
```

应用固件叫 **`AcidBox.bin`**，不叫 `firmware.bin` —— arduino-cli 用**工程（sketch）名**
给产物命名，`firmware/AcidBox/AcidBox.ino` 就编出 `AcidBox.bin`。CI 不会去猜这个文件名：
它把构建目录里三个已知文件（`bootloader.bin` / `partitions.bin` / `boot_app0.bin`）
排除后剩下的那个 `.bin` 就是应用，名字自动填进 `flash-args.txt`。改工程名也不会坏。

对应的偏移表（来自 core 的 `platform.txt:349` 上传配方 + `noota_3g.csv`）：

| 地址 | 文件 | 大小上限 |
|---|---|---|
| `0x0` | `bootloader.bin` | — |
| `0x8000` | `partitions.bin` | — |
| `0xe000` | `boot_app0.bin` | 8 KB |
| `0x10000` | `AcidBox.bin` | **1 MB（`upload.maximum_size=1048576`）** |

`--baud` 460800 也很稳，线不好就降。

> ⚠️ `boot_app0.bin` 的地址是 `0xe000`，不是常见教程里的 `0xe0000`。
> `noota_3g.csv` 把 nsv 分成 0x5000（默认 0x4000），把 otadata 从 `0xd000` 顶到了 `0xe000`。
> 写错就等于把 otadata 写进了空隙里。

当前 `AcidBox.bin` 是 616,724 字节，对 1 MB 的 app0 分区还剩 **431,852 字节**。
（GitHub Actions 页面上 `AcidDripS3-firmware` artifact 显示的 806 KB 是**压缩包**大小，不是固件大小。）
M1 之后会陆续吃掉这个余量，CI 的 run 页面会一直显示这两个余量。

### 5.5 完全不装软件：浏览器烧写

**只下一个文件：`AcidDripS3-merged` 里的 `merged.bin`，地址填 `0x0`。**

CI 把 bootloader、分区表、otadata、app、LittleFS 鼓组按 `noota_3g` 的实际布局
拼成**一个 4 MiB 的镜像**了。原因是这个分区表在 `0x0` 到 `0x400000` 之间
**完全连续，中间没有任何空洞**：

```
bootloader  0x000000
partitions  0x008000
nvs         0x009000
otadata     0x00E000
app0        0x010000    }  4,194,304 字节 = 4 MiB
spiffs      0x110000    }  ← 鼓组就在这里
coredump    0x3F0000
```

所以五行表格可以塌成一行：一次下载、一个地址、一次点击。
缝隙全部填 `0xFF`，也就是"已擦除"的 flash 状态，和刚出厂一样。

偏移量不是写死在 CI 里的，是**从 core 自己的 `noota_3g.csv` 解析出来的**
（`PARTITION_SCHEME` 这个环境变量同时喂给 FQBN，两边不会打架）。
换分区方案它会自己跟着变——`0xe000` / `0xe0000` 那种坑就是这么躲掉的。

| Flash Address | File |
|---|---|
| `0x0` | `merged.bin` |

**步骤：**

1. Chrome 或 Edge 打开<https://espressif.github.io/esptool-js/>（Safari 不支持）
2. 按住 `BOOT` → 点 `RST` → 松开 `BOOT`
3. Baudrate 选 `921600` → 点 **Connect** → 确认认出 `ESP32-S3`
4. Flash Mode **keep** / Flash Freq **keep** / Flash Size **keep**
5. **Add File** 加一行：`0x0` + `merged.bin`
6. 点 **Program**（4 MiB 在 921600 下大概半分钟）

三个 Flash 选项都选 **keep**：arduino-cli 编译时已经按 `FlashSize=16M` 把
flash size / mode / freq 写进镜像头里了，工具再改一遍只会引入偏差。

> 想核对下载的东西：`AcidDripS3-merged` 里还有 `README.md`，
> 上面有五个分块的地址、大小和 sha256 前 16 位，可以逐个对。
>
> ```
> Get-FileHash merged.bin -Algorithm SHA256
> ```

<details>
<summary>原来的五行分刷（只在需要单独重刷某一块时用）</summary>

| Flash Address | File | 来自 |
|---|---|---|
| `0x0` | `bootloader.bin` | `AcidDripS3-firmware` |
| `0x8000` | `partitions.bin` | `AcidDripS3-firmware` |
| `0xe000` | `boot_app0.bin` | `AcidDripS3-firmware` |
| `0x10000` | `AcidBox.bin` | `AcidDripS3-firmware` |
| `0x110000` | `littlefs.bin` | `AcidDripS3-littlefs` |

命令行版本见 §5.4 的 `flash-args.txt` 和 §6 的第二条命令。
**分刷时 LittleFS 那一次要么和固件同一次刷完，要么一次都别刷**——
只刷固件就上电，鼓组会被自动格式化掉（`FORMAT_LITTLEFS_IF_FAILED true`），
出来的是 8-bit fallback，还很容易误判成"刷成功了"。
`merged.bin` 根本没有这个坑，这也是它存在的理由。

</details>

<details>
<summary>为什么不用 web.esphome.io</summary>

它是 **ESPHome 设备向导**：写 YAML → 交给它的构建服务器编译 → 刷它编出来的**那一个**固件。
对着它发布的 `app.*.js` 数关键字：`littlefs`、`spiffs`、`write_flash`、
`boot_app0`、`0x8000`、`0xe000` 出现次数**全是 0**。
它内部传给烧录引擎的确实是个 `{address, data}` 数组，但界面上没有任何地方
让你往里放自己的文件。

所以它**刷不了 `littlefs.bin`**，鼓组还是得用 esptool 烧，
变成"浏览器刷固件 + 命令行刷鼓组"两套流程，还得保证两边是同一次 CI 的产物。

（顺带：它的 bundle 里有按 `VID 0x303A / PID 0x1001`、`0x1002` 识别芯片的代码，
也就是**明确支持 S3 原生 USB 的 ROM 下载模式**——所以"原生 USB 能不能刷"本身没问题，
问题只在它没有放文件系统镜像的地方。）

</details>

**关键：必须插原生 USB 口（USB OTG，GPIO19/20），不能插 UART 桥。**

CH340 / CP2102 / FTDI 这类桥片芯片 Chrome 的 Web Serial **认不出来**。
（ESP Web Tools 页面顶上那个 `WebUSB (CH340)` 勾选框是另一条路：走 WebUSB 直通，
但在 Windows / macOS / Linux 上 usbserial 内核驱动会先占住那个接口，
`claimInterface` 直接失败——页面上那段小字说的就是这件事。
只有 Android / Chrome OTG 才稳。我们是桌面机，别走这条。）

原生 USB 口在 S3 上是**固定功能**的 USB-Serial-JTAG 外设，GPIO19/20，
和固件无关。所以按住 BOOT 进 ROM 下载模式时它一定会枚举出来。

**其他注意：**

- **别点 `Erase Flash`。** 不需要，而且点完再只刷一部分，就把前面写的擦了。
- 刷完之后原生 USB 口变成 **TinyUSB MIDI 设备**（`AcidBox S3`），不再是串口。
  要再刷就重新 BOOT+RST 进下载模式——正常现象，不是坏了。
- CI 里那一步会把拼出来的镜像**读回来逐字节比对**每个分块的偏移；
  固件超过 1 MB 或 LittleFS 装不下会直接让 build 变红，
  而不是产出一个"看着能刷"的镜像。

## 6. LittleFS 鼓组音色

鼓组不在固件里，在 flash 的 LittleFS 分区上。

```
config.h:97   #define FORMAT_LITTLEFS_IF_FAILED true
sampler.ino:119  if ( !LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) { ... return; }
```

`FORMAT_LITTLEFS_IF_FAILED true` 意味着**挂载失败会自动格式化**。
所以镜像和固件必须配套刷，中途只刷一半就上电，镜像就没了。

数据规模（上游 `AcidBox/data/`，8 套鼓组 × 12 个 wav）：

| 鼓组 | 0 | 1 | 2 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| 大小 | 0.4 | 0.5 | 0.2 | 0.1 | 0.2 | 0.4 | 0.4 | 0.3 MB |

对齐到 4 KB 块之后的真实占用：

```
原始 96 个文件         2,673,230 字节
块对齐后               2,863,104 字节  （块对齐本身吃掉 189,874 字节）
分区                   3,014,656 字节  (0x2E0000)
名义余量                 151,552 字节  ← 还没扣 LittleFS 元数据
```

**余量只有约 148 KB，而且 LittleFS 的 inode 表 + 目录块大概要吃掉其中一部分到全部。**
能不能装下要看 CI 的实际输出——`mklittlefs` 装不下会直接返回非零，build 就红。
这正是 CI 里那一步写成"装不下就 fail 而不是产出一个纸面上能用的镜像"的原因。

万一装不下，选项（按优先级）：

1. **删掉一个鼓组**。`config.h:113` 的 `DEFAULT_DRUMKIT` 是 `0`，而 kit 1（0.5 MB，光 `101_BD8.wav` 就 175 KB）最大。砍它能腾出约 550 KB。
2. **换分区方案**。如果板子是 16 MB flash，`PartitionScheme=custom` 配一份自己写的 CSV，给固件 1 MB、文件系统 6 MB，问题就没了。代价是要动 CI 的 FQBN 并自己维护 CSV。

块大小必须和固件一致，否则镜像挂不上：arduino-esp32 的 `LittleFSFS::begin()`
只设了 `grow_on_mount = true`，块大小走 IDF 的默认 **4096**，
所以 CI 里 `mklittlefs -p 4096 -b 4096`。

### 上传镜像

分区表里那块标签是 **`spiffs`**（不是 `littlefs`），类型 `data, spiffs`，
所以工具要给 `--partition spiffs`。镜像本身由 CI 从上游 `data/` 构建好了：

```
AcidDripS3-littlefs / littlefs.bin
```

用 core 自带的 `esptool` 烧（core 里 `tools/flasher.py` 的等价物），偏移是分区表里的 `0x110000`。
**不过平时直接走 §5.5 刷 `merged.bin` 就够了**——鼓组已经在里面了，
不用管这一条命令：

```powershell
python -m esptool --chip esp32s3 --port COM5 --baud 921600 `
  --before default-reset --after hard-reset `
  write-flash --flash-mode keep --flash-freq keep --flash-size keep `
  0x110000 littlefs.bin
```

> 上游 `data/` 里是 kit `0 1 2 4 5 6 7 8`，**没有 3**。`DEFAULT_DRUMKIT` 是 `0`，不冲突；
> 但后面要按 kit 编号选的话，得记住 3 号是空的。

镜像没刷会怎样：固件格式化 → `sampler.ino:136` 调 `CreateDefaultSamples()` → 只写 `samples.h` 里那一套内嵌 fallback（256 KB 的 8-bit 素材）。**能出声，但不是 808 采样**，别误判成 LittleFS 装好了。

---

## 7. M0 验收清单

没有串口日志，全靠听 + MIDI：

| # | 动作 | 期望 |
|---|---|---|
| 1 | 上电，不按任何键 | 立刻出声（`JUKEBOX_PLAY_ON_START`），酸味贝斯 + 808 鼓，自动 pattern |
| 2 | 听音色 | 鼓是**采样的 808**，不是 8-bit 噪声 → 说明 LittleFS 挂上了 |
| 3 | Windows 设备管理器 | 原生 USB 枚举出 MIDI 设备，名字 `AcidBox S3`，VID `1209` PID `1305` |
| 4 | DAW / MIDI-Ox 监视 | 能看到 2 个 303 通道 + 1 个鼓通道的 Note On/CC |
| 5 | 听混响 | 鼓和合成器有 send 混响（`Reverb` 初始化了，说明 PSRAM 认到了） |
| 6 | 听失真 | 没有咔哒声、没有周期性爆音、没有明显丢音 |
| 7 | 听走音 | `PUK_CLIPPING` / 步进音高，是 acid 的滑音+重音，不是死板的单音 |

如果第 1 步没出声，按这个顺序排：

1. 功放/音箱有没有静音、音量旋钮是不是拧到底
2. `BCK / DIN / LRC` 三根线有没有接错位（尤其 `DIN` ↔ `DOUT` 接反）
3. 有没有共地
4. DAC 的 `VCC` 是不是 3V3
5. 波特率/采样率：外部 DAC 走 `SAMPLE_RATE 44100`、16 bit、stereo（`config.h:55`）

---

## 8. M0 之后：core-0 余量测量

M0 的真正目标不是"能出声"，而是量出 core-0 的占用率。
`firmware/AcidBox/AcidBox.ino:298` 那里把两个音频任务的优先级从 1 提到 5：

```c
// M0: priority was 1, equal to Arduino's loopTask (which runs regular_checks()
xTaskCreatePinnedToCore(&renderSample, "audio", 4096, NULL, 5, &SynthTask, 0);
```

原因是这里的 `loop()` **不是空闲的**——`AcidBox.ino:333` 一直在跑 `regular_checks()`。
M3 要往 `loop()` 里塞 TFT 重绘和音序器，不测就没法知道还剩多少空间。

下一步要做的是加一个 `BENCH_AUDIO_HEADROOM` 开关，用 `esp_get_minimum_free_heap_size()` 加上任务运行时间采样，把 core-0 的占用百分比打到串口。这个开关和测量方法会在 M0 收尾时加进来。

---

## 附：一张图

```
                     USB-C (USB-OTG)  ───────────►  PC  [USB MIDI]
                        │
                        │  GPIO19/20
                     ┌──┴──────────────┐
   GPIO5 ── BCK ─────►│                 │
   GPIO6 ── DIN ─────►│   ESP32-S3      │         (GPIO0  = BOOT)
   GPIO7 ── LRC ─────►│   DevKitC-1     │         (GPIO15/16/17 留空)
                     │   N8R8 / N16R8   │         (GPIO23 留空，不接按键)
   3V3  ────────────►│                 │
   GND  ──┬─────────►│   USB-UART 桥    │
          │          └──┬──────────────┘
          │             │
          │       USB-C (UART) ────────►  PC  [烧录 / 串口(空)]
          │             │
          │      ┌──────┴───────┐
          └──────┤  PCM5102A    │
                 │  3V3  GND    │
                 │  BCK  DIN    │
                 │  LRC  MCLK◄──┘ 悬空
                 │  L    R      │
                 └──┬─────┬─────┘
                    │     │
                 ┌──┴──┐┌┴───────┐
                 │ L 功放││ R 功放  │──► 喇叭 / 耳机
                 └──────┘└────────┘
```
