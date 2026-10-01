# 交接状态

面向"换一台机器接着干"或者"隔一段时间回来"。

设计决策在 `ESP32S3_FUSION_IMPLEMENTATION.md`，本文**不重复**那些，只写四样别处
没有的东西：验证过的事实、踩过的坑、当前的阻塞、下一步。

最后更新：M0，`c8a8719`，CI 全绿（run #14）。

---

## 1. 一句话状态

软件侧 M0 全部完成并通过 CI 验证。**M0 剩下的全部是物理工作，不接板子无法验收。**

---

## 2. 新机器上怎么把环境恢复出来

### 2.1 必须手动搬的一样东西：SSH 私钥

仓库是私有的，远程是 `git@github.com:cxandy/AcidDripS3.git`（SSH，非 HTTPS）。
私钥在 `~/.ssh/zlyb_id_rsa`，**不在 git 里，也不会在任何 artifact 里**。换机必须
自己带过去，并且 `~/.ssh/config` 要有：

```
Host github.com
    HostName github.com
    User git
    IdentityFile ~/.ssh/zlyb_id_rsa
    IdentitiesOnly yes
```

没有这把钥匙就只有两条路：重新在 GitHub 上加一把 deploy key，或者改用 HTTPS +
PAT。前者更省事，因为 remote 已经写死了 SSH 地址。

### 2.2 仓库

```powershell
git clone git@github.com:cxandy/AcidDripS3.git
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

| 阻塞 | 说明 |
|---|---|
| **没有 ESP32-S3 板子** | M0 剩余项全部卡在这里。必须 **N8R8 或 N16R8**——R2 跑不了（`PSRAM_SAMPLER_CACHE` 是 3 MB） |
| 本机不能编译 | 见 §2.4。不是问题，是既定事实 |

设备管理器里 `USB JTAG/serial debug unit`、`COM5`（CH340）、
`VID_303A&PID_1001` 全是 `Status = Unknown`，即残留记录。目前只有 `COM1` 是活的。

---

## 6. 接下来做什么

**按顺序：**

1. **接板子**（N8R8 / N16R8）→ 接 PCM5102A：
   BCLK=5 / DIN=6 / WCLR=7 + 3V3 + 共地 → 进功放或有源音箱。
   **GPIO 15/16/17 必须空着**（`AcidBox.ino:267` 无条件 `pinMode`）。
   **DAC 从 3V3 供电，不要 5V。**
2. **浏览器刷 `merged.bin` @ `0x0`**（§4.2）
3. **M0 验收**：只能靠耳朵 + MIDI 活动监视器，因为 `DEBUG_ON` 关着，**串口零输出**：
   - 808 个 *sampled* 鼓 → LittleFS 挂上了
   - `AcidBox S3` / VID `1209` PID `1305` → USB MIDI 枚举成功
   - 没爆音 → 任务优先级 1→5 有余量
4. **补上 `BENCH_AUDIO_HEADROOM`**，量出 core-0 的实际余量。
   **建议在 M1 之前做**——否则 M3 的音序器和 TFT 是踩在一个"我记得好像够"的
   基线上，而不是一个量出来的数字上。
5. **确认两个悬而未决的硬件问题**：Arduino 默认 `USBMode`（TinyUSB）和
   `MIDIUSB_ESP32.h` 的关系；板子真实的 `FlashSize`。

**然后进 M1**：`engine_iface.h` / `.cpp`，**重新实现** `Acid_Drip` 的行为
（它的代码不能进仓库，见 §2.3）。M2 才关掉 `JUKEBOX`。

设计文档里有完整的里程碑和依赖顺序，关键路径 M0 → M1 → M2 → M2.5 → M3 → M4。

---

## 7. 文件地图

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
