# 星闪 SLE 等时传输控制器

本工程实现 SLE 单链路周期等时单播和非链接态同步广播。C11 代码负责控制信令、业务数据和射频调度，SystemVerilog 参考模块根据 C 控制器输出的描述符启动收发窗口并上报完成中断。

- [SLE_CIS](SLE_CIS/) 实现成员 A 的链接态同步等时单播，包含 G/T 建链、参数更新、断开、分片重组、反向反馈和公共等时适配层。
- [SLE_BIS](SLE_BIS/) 实现成员 B 的非链接态同步广播，包含广播发现、同步、A6 收发、跳频以及两个模块共用的射频调度器。

## 运行测试

在仓库根目录使用 PowerShell 运行。前两条命令执行 C 回归，后两条命令使用 Vivado XSim 执行 RTL 自检并在各自的 `xsim/results/` 生成日志和波形。

```powershell
.\SLE_CIS\run_tests.ps1 -Compiler gcc
.\SLE_BIS\run_tests.ps1 -Compiler gcc
.\SLE_CIS\xsim\run_xsim.ps1
.\SLE_BIS\xsim\run_xsim.ps1
```

XSim 脚本默认使用开发机的 Vivado 2020.2 安装路径。其他安装位置可通过 `-VivadoBin` 指定，C 编译器可通过 `-Compiler` 指定。Linux 或 WSL 的 GCC 命令见两个模块的 README。

两套 C 回归的通过标志均为 `ALL TESTS PASSED`。RTL 回归的通过标志分别为 `CIS_XSIM_PASS phases=18 completions=12` 和 `BIS_XSIM_PASS starts=6 completions=8`。当前验证覆盖软件双端及事件窗口 RTL，真实 PHY、板级 DMA 与中断、设备间空口互通仍待上板验证。模块 README 提及的协议原文、Word 开发文档和历史波形保存在本地工程中。
