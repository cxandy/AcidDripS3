# 交接状态

面向"换一台机器接着干"或者"隔一段时间回来"。

设计决策在 `ESP32S3_FUSION_IMPLEMENTATION.md`，本文**不重复**那些，只写四样别处
没有的东西：验证过的事实、踩过的坑、当前的阻塞、下一步。

最后更新：`dbd9993`，CI 全绿。**M0 结束**：上电爆音+持续噪音已定位并修复，
`M0_DIAG` 已置 0，诊断开关原样保留。见 §7。

---

## 1. 一句话状态

板子已接上、已刷机、**噪音已消失**——`dbd9993` 是出货构建：`M0_DIAG 0`（无强制静音、
无峰值跟踪），诊断工具链原样保留在开关后面，`DEBUG_ON` 开着。
软件侧没有已知阻塞；下一步是 §6 的四项。

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

1. **补上 `BENCH_AUDIO_HEADROOM`**，量出 core-0 的实际余量。
   **必须在 M1 之前做**——否则 M3 的音序器和 TFT 是踩在一个"我记得好像够"的
   基线上，而不是一个量出来的数字上。这是从 M0 换来的教训：这里原本是拍脑袋写的
   优先级 1→5，代价是 `loop()` 被饿死一整轮排查（§7）。
2. **确认板子真实的 `FlashSize`**，以及 `MIDIUSB_ESP32.h` 与 Arduino 默认
   `USBMode`（TinyUSB）的关系。
3. **修 `HARDWARE_SETUP.md` 的 `:149` 和 `:171`**（§5 表里那两行），顺手删掉
   `config.h` 里已移除的 `M0_REVERB_TOGGLE_PIN` 相关残留说明。
4. 决定 remote 要不要换 SSH（§2.1），不影响功能。

**然后进 M1**：`engine_iface.h` / `.cpp`，**重新实现** `Acid_Drip` 的行为
（它的代码不能进仓库，见 §2.3）。M2 才关掉 `JUKEBOX`。

设计文档里有完整的里程碑和依赖顺序，关键路径 M0 → M1 → M2 → M2.5 → M3 → M4。

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
