# dslcap

DreamSourceLab DSLogic 逻辑分析仪的命令行采集工具。上游 sigrok 不支持 DSLogic U3Pro32，
本工具直接编译 DSView 源码里的 `libsigrok4DSL` 来驱动设备，输出原始二进制，再用 sigrok-cli 离线解码。

## 依赖

```bash
sudo apt-get install libglib2.0-dev zlib1g-dev libusb-1.0-0-dev pkg-config sigrok-cli
# 非 root 访问设备（当前用户需在 plugdev 组）
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="2a0e", MODE="0660", GROUP="plugdev"' |
  sudo tee /etc/udev/rules.d/60-dreamsourcelab.rules
sudo udevadm control --reload-rules
```

DSView 源码默认取 `~/Downloads/DSView-1.3.2`（`-DDSVIEW_DIR=` 可改），FPGA 固件从其中的
`DSView/res` 加载（`--fw-dir` 可改）。U3Pro32 的 FX3 固件在设备 flash 里，不需要 `.fw` 文件。

## 构建与测试

```bash
cmake -S . -B build -G Ninja && ninja -C build
ctest --test-dir build        # 解析与数据格式转换的单元测试
```

## 用法

```bash
./build/dslcap info                                     # 列出各模式的通道数、采样率范围、每通道深度
./build/dslcap capture -o cap.bin -c 0-15 -r 100M -n 1M # buffer 模式采 1M 点
./build/dslcap capture -o cap.bin -c 0-7 -r 50M -t 20ms --trigger 3:f --pos 10
```

- `-c` 选分析仪通道；`--vth` 阈值电压，默认 1.6 V（3.3 V 逻辑）。
- `--trigger CH:TYPE`：`r` 上升沿、`f` 下降沿、`e` 任意边沿、`0`/`1` 电平；`--pos` 为触发前所占百分比。
- 默认 buffer 模式（先存设备内存再上传，不丢数据）；`--stream` 为流模式，速率受 USB 带宽限制。
- 超时默认是"采集时长 + 10 s"，等触发时用 `--timeout` 加长；Ctrl-C 可中止。
- 退出码：0 成功，1 参数错误，2 找不到设备，3 采集失败（含数据错误、溢出、样本不足）。

## 输出与解码

`cap.bin` 按样本顺序存放，每个样本 `unitsize` 字节（小端），bit k 就是分析仪通道 k；
`cap.bin.json` 记录采样率、样本数、通道、触发位置。sigrok-cli 的通道名是数字 `0..N-1`：

```bash
sigrok-cli -i cap.bin -I binary:numchannels=16:samplerate=100000000 \
  -P uart:rx=3:baudrate=2000000 -A uart=rx-data
sigrok-cli -i cap.bin -I binary:numchannels=16:samplerate=100000000 \
  -P spi:clk=1:mosi=2:miso=3:cs=0 -A spi=mosi-data
sigrok-cli -i cap.bin -I binary:numchannels=16:samplerate=100000000 -o cap.sr  # 给 PulseView 看
```

`numchannels` 取 `unitsize × 8`（工具结束时会打印完整命令）。

## 限制

- 只支持 DSLogic 系列的逻辑分析模式（按设备名匹配 "DSLogic"）。
- U3Pro32 接在 USB 2.0 口上时 stream 模式降为 10 MHz×32 … 100 MHz×3；buffer 模式不受影响。
- 设备需要可靠供电：经多级 USB 2.0 hub 连接时出现过设置配置超时（`can't set config #1, error -110`）后掉线，
  应直接接电脑的 USB 3.0 口。

## 许可证

GPL-3.0-or-later，全文见 `LICENSE`。本仓库只包含 dslcap 自己的代码；构建时编译的 DSView `libsigrok4DSL`
以 GPLv2+/GPLv3+ 发布，链接后的程序按 GPLv3 或更高版本分发。
