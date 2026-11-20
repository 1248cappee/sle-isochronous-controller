# 成员 A · SLE 链接态同步等时单播

交付对象是绑定到一条已建立的单播异步链路的等时子链路。完整双端用例运行帧类型 1、2.4 GHz、1 MHz、周期适配、G 先发业务和 T 后发反馈。控制面通过父链路有序传送。父链路接入、真实 RF、DMA、加密和板级寄存器由平台提供。工程用 CIS 表示成员 A 的工作范围，空口字节格式采用 T/XS 10002—2025。

## 复跑

在本目录使用 PowerShell 执行以下命令。默认使用本机 Vivado 2020.2 和 Vitis 随附 GCC，也可通过脚本参数 `-Compiler` 和 `-VivadoBin` 指定安装位置。

```powershell
.\run_tests.ps1
.\xsim\run_xsim.ps1
```

Linux / WSL 从本目录运行带地址和未定义行为检查的回归：

```sh
gcc -std=c11 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-sanitize-recover=all *.c ../SLE_BIS/sle_*.c -o /tmp/sle_cis_test
/tmp/sle_cis_test
```

PC 成功标志为 `ALL TESTS PASSED`。XSim 重新运行 PC 回归，由 `CIS_EVENT` 和 `CIS_LINK_EVENT` 生成两组描述符；成功标志为 `CIS_XSIM_PASS phases=18 completions=12`。Vivado 打开 `xsim/results/cis_paired_window.wdb`，加载同目录 WCFG。观察 `sampled_us`、`start_phase`、`phase_second`、`tx_direction`、`active_channel` 和 `irq_valid`。复位后 sampled_us 与 GUI 的绝对仿真时间相差 2.5 µs；核对 C 日志时使用 sampled_us。

## 控制与数据入口

1. 为每个 G/T 实例长期分配一个 `cis_node`，用 `cis_node_init` 绑定父链路时基、公共 `sle_sch`、物理信道地图和板级端口。父链路传入的时间戳必须对应已知的父事件组起点，单位为单调 µs。各端分别持有自己的射频调度表；同一个射频上的 A 和 B 共享同一张表。
2. 主机在 G 的 `node.control` 上调用 `cis_create`。父链路将 `send_control` 中的完整信令按序可靠送至对端 `cis_control_rx`。每次输入一个完整的类型/长度/值元素。控制任务串行执行，回调先复制消息再返回。接收通知先处理到期定时器，禁止重入。父链路故障用 `cis_parent_down` 撤销已绑定资源。
3. 控制任务以 `cis_node_tick` 推进时间。激活时触发 `CIS_LINK_UP`。用 `cis_node_queue_at` 按业务时间戳映射到组和 SDU 序号，或用 `cis_node_queue` 传入已知的原始组与序号。数据在接受时复制，单个实例支持一个待发 SDU，接收端可同时缓存四个 SDU。主机按序提交，遇到回压稍后重试。
4. 在预定窗口前调用 `cis_node_schedule`。BSP 提供校准后的先发及后发空口时长。先发、收发转换和后发组成一次不可拆的射频预约。publish 必须在返回前复制描述符及 TX 数据、完成 cache 维护并转交硬件所有权。抢占失败或已经错过的窗口按返回码处理，下一窗口仍按原锚点计算。
5. ISR 将带 token 的包、反馈、结束快照交给控制任务；使用 `cis_node_service` 排队读取并确认。接收端须在后发窗口前把反馈提交给基带。peek 后至 ack 前缓冲区保持有效，超时检查之前先排空已到达的 IRQ。上层按截止时间调用 `cis_node_deliver`，保留完整 SDU、长度及交付时间。
6. 用 `cis_propose` 交互参数后，G 直接调用 `cis_update` 或 T 调用 `cis_request_update`。在最后一个旧组与新配置生效点之间禁止新增旧组。新时间表从生效锚点开始编号，先前已重组完整的 SDU 保留原交付时刻；未完成的旧流数据向主机报告丢弃。任一方可调用 `cis_disconnect`，父链路确认该次断开后回调 `cis_disconnect_ack`。先停止并排空 DMA，再释放窗口和数据缓冲区。

## 对照代码和证据

- `cis_wire.c` 和 `cis_control.c` 实现 §7.2.17、§7.2.19、§7.3.2 的建链、协商、更新、拒绝、断开。0x0022 的载荷为 56 字节；0x0023/24 各 52 字节。同一逻辑值在不同消息里的偏置按各条款分别转换。
- `sle_isoal.c`、`cis_data.c` 实现周期分段、组内顺序、A3 信息位、反馈与跨组有效载荷基准。配置复用时写入 0xFFFE，小端字节为 FE FF；未配置时省略类型索引。A3 的 28 信息位存放在 4 字节容器的低位，12 位控制 CRC 由 PHY 处理。
- `cis_node.c` 联接主机、控制面、数据面和板级事件。示例见 `test_end_to_end.c`。协议 SDU 分段额度按 §6.5.3.2 计算；同步数据头及可选类型索引另计入 A3 长度。目标平台应核对 Max_PDU 口径、描述符容量和空口向量。
- `test_data.c` 对 B 使用 §7.3.3.4 格式，并通过真实 BIS 创建、发现、调度和收包入口验收公共 ISOAL。既有 B 接口按完整线上 PDU 检查最大长度，本次用例选取可容纳额外帧头的配置。接入 B 现有最大长度边界时，需先统一容量和开销定义。
- `test_radio.c` 中真实 CIS/BIS 各自取得同一调度器的授权，另一活动检测到冲突；窗口释放后可以继续调度。板级事件含两相的时间、方向、逻辑链路 ID、CRC 种子和 token。旧完成通知与新活动隔离。
- `results/` 保存 A 的 Windows/WSL 日志及 B 原始 PC/XSim 回归日志；`xsim/results/` 包含 A 的输入、编译、展开、运行日志和可打开的波形。本地没有目标板的 EM、IRQ/MMIO 定义与端到端对端录包，M4 板端、M5 双人签署及完整 §11.1.8.2 测试矩阵仍待实测。

## 本轮实现范围

数据端点启用单条未加密链路和 G→T 业务；反向支持 TB 反馈。控制字段与射频时基支持五种调度时隙，低层定时测试另覆盖 T 先发，主机端点对当前未启用的业务方向和非周期配置返回失败。链接态为确保 A3 的 5 位序号在恢复窗口内唯一，要求 每组新包数 × 丢弃周期 ≤ 32，每组链接态新包数为 1–15。缓存数和一期资源限制由接入校验或回压返回。音频压缩、非周期业务、多流、两向业务载荷和真实 PHY 路径属于后续集成范围。
