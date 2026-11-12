# RISC-V Hummingbird E203 SoC 总线架构与制作设计说明书

本项目开源了基于开源蜂鸟 **Hummingbird E203 RISC-V Core** 的 SoC 总线拓扑、存储系统架构设计方案以及全流程软硬件协同控制规范。

> **声明**：为遵守开源与保密规范，本项目**不包含任何专有 AI 加速计算引擎（PE 阵列/矩阵乘法逻辑）源码及专有总线 Verilog 源码**，仅保留完整 E203 CPU 开源核及其启动固件，并对外公开**高并发单口 SRAM 分阶段独占仲裁总线架构的设计制作说明**与集成契约。

---

## 一、 系统总体架构拓扑

在典型嵌入式边缘计算 SoC 中，CPU 需要与协处理器（加速器）以及外部测试/调试端口（Loader）高效协同。为避免增加双口 RAM（面积翻倍）或昂贵的多端口交叉开关（Crossbar），本系统设计了一套**任务级单口分阶段独占互斥仲裁总线系统（Single-Port Staged Arbiter）**。

### 1. 系统地址空间映射（Memory Map）

| 地址空间范围 | 目标从设备 / 存储体 | 容量 / 规格 | 访问特性与功能说明 |
|:---|:---|:---|:---|
| `0x8000_0000 ~ 0x8000_FFFF` | **CPU ITCM** | 64 KiB | 指令紧耦合存储器，存放启动代码与核心指令 |
| `0x9000_0000 ~ 0x9000_FFFF` | **CPU DTCM** | 64 KiB | 数据紧耦合存储器，存放全局变量、栈及状态邮箱 |
| `0xA000_0000 ~ 0xA007_FFFF` | **片上共享 SRAM** | 512 KiB (64×8KB) | 统一大容量共享内存，存放任务输入、参数与计算结果 |
| `0x1100_0000 ~ 0x1100_0FFF` | **SysPer MMIO 控制块** | 4 KiB | 系统外设控制寄存器，支持握手、中断与分阶段控制 |

### 2. 共享 SRAM 物理组织拓扑

* **总容量**：512 KiB（`0x80000` 字节）。
* **宏单元组合**：采用 64 颗 8 KiB 单口静态存储器（`RAMSP4096X16`，4096 深 × 16 位宽）。
* **物理分块结构**：
  * **Top Banks**：`addr[18]` 将 512 KiB 拆分为 Bank0 与 Bank1（各 256 KiB）。
  * **Word 寻址**：`addr[17:2]` 产生 64K 个 32-bit 字地址。
  * **Sub-banks 组织**：`word_addr[15:12]` 选择 16 个子 Bank，`word_addr[11:0]` 选择各宏的 4096 行。
  * 每组采用两颗 16 位宏拼成标准的 32 位数据位宽，支持按字节掩码写（WEN/WEM）。

---

## 二、 总线制作与核心仲裁机制设计说明

### 1. 架构接入点与握手协议
* **接入位置**：置于 E203 原生 1-to-8 总线矩阵的 `sysmem_icb` 下游。
* **协议标准**：采用标准 ICB（Internal Chip Bus）总线协议，包含：
  * **命令通道**：`cmd_valid`、`cmd_ready`、`cmd_read`、`cmd_addr`、`cmd_wdata`、`cmd_wmask`。
  * **响应通道**：`rsp_valid`、`rsp_ready`、`rsp_err`、`rsp_rdata`。
  * 只有在 `valid` 与 `ready` 均为高电平时，事务正式生效并推进。

### 2. 为什么需要任务级独占仲裁？
在低功耗 SoC 设计中，如果 CPU、DMA 与外部 Loader 频繁竞争同一组单口 SRAM，会带来极大的总线仲裁开销、复杂的死锁判定以及频繁的管线打断气泡。
**制作方案**：将总线访问划分为清晰的“阶段（Epoch）”，每个阶段由唯一的 Master 独占整个 512 KiB SRAM 的单一访问物理端口。

### 3. 三方主设备与优先级定义
1. **Master 0 (CPU)**：E203 处理器（执行裸机任务控制与状态机推进）；
2. **Master 1 (Accelerator DMA)**：专用加速器数据搬运引擎；
3. **Master 2 (Testbench Loader)**：外部测试平台 / 主机适配装载通道。

---

## 三、 四阶段单口互斥协议与排空机制（Drain Controller）

为确保总线在切换 Master 时绝无数据错乱与在途（In-flight）交易丢失，仲裁器必须实现**零残留响应排空（Drain）机制**。

### 1. 核心工作流程状态机

```
+---------------+     MMIO LOAD_REQ       +---------------+
|               | ----------------------> |               |
|  阶段 1: CPU  |                         | 阶段 2: Loader|
|  (配置与触发) | <---------------------- | (输入数据装载)|
+---------------+     LOAD_DONE Token     +---------------+
        |
        | MMIO AI_START
        v
+---------------+     STATUS.DONE=1       +---------------+
| 阶段 3: DMA   | ----------------------> |  阶段 4: Host |
| (计算与写回)  |                         |  (验证结果读出)|
+---------------+                         +---------------+
```

1. **Stage 1 (任务启动)**：
   * 系统复位后，默认由 CPU 独占总线。
   * CPU 写入 MMIO `REG_LOAD_REQ (0x40)`，仲裁器收到请求后：
     * 立即切断 CPU 新发送的 `cmd_valid`（返回反压）；
     * 等待所有已接收的 CPU 在途命令对应的响应全部返回（In-flight 计数器清零，排空完成）；
     * 将 SRAM 单口总线授权给 Loader。
2. **Stage 2 (参数装载)**：
   * Loader 经专用装载路径向 512 KiB SRAM 写入计算参数与激活数据；
   * CPU 在此期间若尝试访问共享 SRAM，其总线 `ready` 保持拉低进入反压等待（CPU 亦可执行 WFI 休眠降低功耗）；
   * Loader 完成全部数据写入后，向共享邮箱 `0xA007_E000` 写入 `LOAD_DONE` Token 并释放授权；
   * 排空 Loader 最后一笔写响应后，CPU 重新接管总线。
3. **Stage 3 (加速器运算)**：
   * CPU 读到 `LOAD_DONE` Token，写入 MMIO `REG_START (0x04)` 启动加速器；
   * SRAM 单口总线全权交由加速器 DMA 独占，高效批量读取数据并将结果写回 SRAM；
   * 加速器最后一笔数据写回并确认响应排空后，MMIO 控制块产生中断（或置位 `STATUS_DONE`）。
4. **Stage 4 (结果读出与确认)**：
   * CPU 写入 MMIO `REG_READOUT_REQ (0x48)`，切换授权允许外部 Host / 测试平台读出输出数据进行对比校验；
   * Host 读取完毕后写入 `READOUT_DONE` Token；
   * CPU 最终写入 `REG_CONFIRM (0x50)`，完成整套任务闭环。

### 2. 响应路由与 Tag FIFO 机制
由于 ICB 读操作存在固定周期的存储器延迟（RAM Read Latency），仲裁器内部维护一个深度为 8 的 **Tag FIFO**：
* 每次命令握手成功（`cmd_valid && cmd_ready`）时，将当前授权主设备 ID 压入 Tag FIFO；
* 每一个读响应从 SRAM 返回（`rsp_valid`）时，弹出队首 ID，将响应严格定向路由给对应 Master；
* 即使在切换临界拍，也绝不会将上一主设备的响应误派送给下一主设备。

---

## 四、 裸机控制固件设计说明

在 `firmware/` 目录下提供了完整的 RISC-V 裸机驱动代码：
* `start.s`：自包含汇编启动文件，初始化 `sp` 栈指针，关闭机器模式全局中断后跳转至 `main`。
* `link.ld`：链接脚本，精确映射 ITCM（`0x8000_0000`, 64KB）与 DTCM（`0x9000_0000`, 64KB）。
* `main.c`：标准 C 语言分阶段总线驱动，严格遵循无轮询死等待机制，支持 WFI 休眠与机器模式外部中断（MEI）。

---

## 五、 目录结构说明

```
.
├── README.md               # 本设计与制作说明书
├── firmware                # E203 裸机驱动与固件
│   ├── link.ld             # TCM 链接脚本
│   ├── main.c              # 分阶段总线状态机驱动源码
│   ├── params.h            # 地址与寄存器宏定义
│   └── start.s             # 汇编启动文件
└── e203_hbirdv2_full       # 开源蜂鸟 E203 RISC-V 处理器完整 RTL 代码库
    └── rtl
        └── e203
            ├── core        # E203 核心执行单元、IFU、EXU、LSU
            ├── fab         # ICB 1-to-8 与 1-to-16 总线互连 Fabric
            └── general     # 通用基础单元库
```

---

## 六、 开源许可与引用

* 蜂鸟 Hummingbird E203 Core 遵循 Apache License 2.0 开源协议。
* 本设计规范适用于全国大学生集成电路创新创业大赛及嵌入式芯片设计赛道，欢迎学术交流与参考。

---

## 七、 服务器端修改适配与综合/签核说明（新发布）

针对服务器真实 SMIC 40nm 工艺与 EDA 环境（DC、Formality、PrimeTime、PrimeTime PX），本项目在  目录下提供了完整的真实修改代码、硬件适配原理与签核伪代码说明：
* **真实代码**：
  * [](./e203_server_adaptation/sirv_mrom_top.server_fixed.v)：修复官方 BootROM 同步读与 ICB 零延迟假定冲突的 1-cycle 真实握手适配代码；
  * [](./e203_server_adaptation/sirv_mrom_top.official_orig.v)：官方未修复版本（供对比 diff）；
  * [](./e203_server_adaptation/apply_rom_fix.py)：自动打补丁脚本。
* **伪代码与详细说明**（详见 [e203_server_adaptation/README.md](./e203_server_adaptation/README.md)）：
  * **Design Compiler 综合流程伪代码与参数设置**（150MHz 门控时钟插入、SVF 记录）；
  * **Formality (FM / LEC) 形式验证逻辑等价性证明伪代码**；
  * **PrimeTime STA 全工艺角静态时序签核伪代码**（Hold 0 违例收敛方案）；
  * **PrimeTime PX 真实门级仿真活动性功耗分析伪代码**（全 SoC 9.65 mW 签核）。
