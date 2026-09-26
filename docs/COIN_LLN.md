# 抛硬币、大数定律与测速

提供两种数据来源：**电脑独立模拟**，或者 **Mega 采集 + 电脑分析**。
Mega 运行 ADC 采样、Von Neumann 去偏、健康检查和混合器，发送随机字节和采集耗时；
正反面计数、误差计算、CSV、JSON 和绘图全部在电脑上完成。
每个字节从低位到高位使用：`1` 表示正面，`0` 表示反面，一个字节对应八次抛硬币。

## 电脑直接运行

需要 Python 3.10 或更新版本。仅模拟、CSV/JSON 输出不需要安装第三方包。
下面都是 PowerShell 可直接执行的命令：

```powershell
cd "E:\ZJU\乘浪\展示\Mega_TRNG"
python tools/coin_lln.py --simulate --flips 1000000 --seed 7
```

`--seed 7` 使用可复现的电脑伪随机序列。省略 `--seed` 使用操作系统随机源。
两者都明确标为电脑数据，不代表 Mega 的硬件数据。

安装串口/绘图依赖后生成演示结果：

```powershell
python -m pip install -r tools/requirements.txt
python tools/coin_lln.py --simulate --flips 1000000 --seed 7 --csv artifacts/coin_million.csv --json artifacts/coin_million.json --plot artifacts/coin_million.png
```

默认记录约两千个曲线点，包括准确的 `1、10、100、1000……` 和最终次数，
无需把一百万个单独的结果留在内存中。支持不是 8 的倍数的次数，例如 `--flips 1000003`；
末尾多余的位不参与统计。`--stride 1000` 可指定记录间隔。

## Mega 只负责采集

将 Mega2560 接到电脑，A0 接入已有的模拟噪声源。悬空 A0 可做实验，但噪声是否充足取决于电路和环境。
编译并上传专用采集固件：

```powershell
pio run -e megaatmega2560_coin
pio run -e megaatmega2560_coin -t upload
python tools/coin_lln.py --list-ports
```

将下面的 `COM7` 改成枚举出的 **Mega USB 串口**。关闭占用该串口的 PlatformIO monitor、
Arduino 串口监视器等程序，然后运行：

```powershell
python tools/coin_lln.py --serial COM7 --mode fast --flips 1000000 --capture artifacts/mega_fast.mlln --csv artifacts/mega_fast.csv --json artifacts/mega_fast.json --plot artifacts/mega_fast.png
```

默认波特率为 `1000000`。固件启动后等待电脑命令，串口监视器里不会自动出现可读文本。
电脑打开串口后默认等 2 秒让 Mega 自动复位，再开始采集；可用 `--boot-delay 3` 调整。
`Ctrl+C` 会结束本次采集；未完成的实验不会被写成成功报告，已校验的完整帧仍保留在捕获文件里。

| `--mode` | ADC 路径 | 用途 |
|---|---|---|
| `conservative` | `/16` 单次转换，LSB 去偏 | 与旧路径比较 |
| `fast`（默认） | `/16` 连续转换，LSB 去偏 | 日常实验 |
| `turbo` | `/2` 连续转换，四个位平面去偏 | 吞吐实验，平面相关性需实测 |

采集固件的三个模式均关闭可选 Timer1/WDT 扰动，便于公平比较；健康检查和混合器仍然运行。
`/16` 在 16 MHz CPU 下为 1 MHz ADC 时钟，**也高于数据手册针对完整 10 位精度推荐的 50～200 kHz**；
`/2` 则为 8 MHz。这些是采样速度配置，不能把转换周期当作已验证的熵速率。

切换 `--mode` 会发送新的启动命令，无需重新编译：

```powershell
python tools/coin_lln.py --serial COM7 --mode turbo --flips 1000000 --json artifacts/mega_turbo.json --plot artifacts/mega_turbo.png
```

如果 USB 串口适配器不支持 1 Mbaud，在 `megaatmega2560_coin` 的 `build_flags` 中加
`-DMEGATRNG_SERIAL_BAUD=250000UL`，重编译上传，并在电脑命令中加 `--baud 250000`。
更换 ADC 引脚可加 `-DMEGATRNG_ADC_CHANNEL=8`（A8）；帧中记录实际通道。

发生健康故障、提取超时、串口超时、CRC 错误或丢帧时，程序报错并返回非零退出码；
不会用电脑随机数填补 Mega 数据，也不会自动丢弃坏帧后把剩余样本当成完整实验。
健康故障后先检查噪声源；重新运行电脑命令才会重新启动采集。

## 重放采集数据

```powershell
python tools/coin_lln.py --input artifacts/mega_fast.mlln --flips 1000000 --plot artifacts/mega_replay.png
```

捕获文件包含完整帧，因此末帧可能包含超过指定次数的随机位。重放时使用相同 `--flips`，
可得到相同的正面数、检查点和曲线。这里保存的是**去偏和混合后的输出**，
不是原始 ADC 波形，不能替代原始熵源评估。

## 怎么看大数定律图和测速

在独立、同分布、公平硬币的假设下，正面比例随次数增加趋近 `0.5`，典型误差规模约为
`0.5 / sqrt(n)`。单次轨迹的误差可以暂时变大，不要求每一步都下降。
图上参考带为 `0.5 ± 1.96 × sqrt(0.25/n)`，是公平独立硬币下的**逐点正态近似范围**，
小样本下不准确，也不表示整条曲线应始终落在带内。
有限实验用于演示收敛趋势，不能证明数学定理，也不能证明 ADC 位相互独立。

同一次采集报告两种速度：

- `generation`：Mega 内部生成随机字节的速度，包含采样、去偏拒绝、健康检查和混合器，
  排除串口发送、CRC 封装和预热；每帧用 `micros()` 计时。
- `received`：电脑收到随机负载的速度，包含传输、校验和计数开销；
  最后一帧全部字节计入传输速率，统计仍只取指定次数。

重放文件仅显示帧内记录的板端生成速度，不会把读磁盘速度误报为串口速度。
`raw_samples` 统计软件实际消费的 ADC 结果，连续转换期间被覆盖的转换不计入；
`accepted_bits` 是去偏后的位数。

## 实现检查和提速证据

```powershell
python tools/check.py
pio run -e megaatmega2560 -e megaatmega2560_min -e megaatmega2560_coin
python tools/benchmark_avr.py --baseline-ref 11cb990
```

`check.py` 需要 `g++`（或用环境变量 `CXX` 指定）。它运行电脑端协议/计数测试，并将实际
`TRNG.cpp` 接入可控 ADC 寄存器替身，检查平面掩码、小预算读取、健康故障、
资源恢复和 A0～A15 的通道选择。这不模拟 ADC 的电气行为。

`benchmark_avr.py` 使用 PlatformIO 安装的 AVR 编译器和 GDB 指令模拟器，与旧提交
`11cb990` 比较同一份确定性 ADC 输入、256 个输出字节及相同编译参数。
它要求原始采样数、接受位数和输出校验值相同，报告**软件周期**变化。
模拟器不执行真实 ADC 等待、串口通信或异步中断，不能将其当作实板速率。
本次检查记录见 [VALIDATION.md](VALIDATION.md)。

## 二进制协议 v1

所有多字节整数均为小端。帧头为 12 字节：

| 偏移 | 长度 | 内容 |
|---:|---:|---|
| 0 | 4 | ASCII `MLLN` |
| 4 | 1 | 版本 `1` |
| 5 | 1 | 标志：`0` 数据，`1` 错误 |
| 6 | 2 | 负载长度：数据 `254`，错误 `1` |
| 8 | 4 | 帧序号，每次启动从 0 开始，按 uint32 回绕 |

数据负载前 14 字节按 `<IIIBB` 排列：生成耗时（µs）、本帧消费的 ADC 样本数、
接受位数、模式（0/1/2）、ADC 通道。随后是 240 个随机字节。
错误负载为一个错误码：1=启动/健康配置失败，2=运行中健康故障，
3=一个字节经过 4096 个新样本仍未就绪。

帧末为 2 字节 CRC-16/CCITT-FALSE（多项式 `0x1021`，初值 `0xffff`，
不反射、不异或终值），覆盖偏移 4 到负载末尾。校验向量 `"123456789"` 为 `0x29b1`。
电脑只在第一个有效帧前允许跳过启动杂字节，此后要求 CRC 和连续序号都正确。
电脑命令均为单字节：`C/F/T` 开始对应模式，`S` 停止。
