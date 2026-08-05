# AFSIM 与 ns-3 通信桥接

本项目把 AFSIM 2.9cn 的推演实体和通信设备接入 ns-3，使网络状态能够实际影响 AFSIM 消息传递，并向前端或文件输出时延、丢包率、吞吐量、流量流向和链路状态。

当前实现采用“开局发送一次完整状态，后续只发送变化量”的方式。AFSIM 仿真线程只采集状态和应用已经返回的效果，TCP 通信与 ns-3 计算在工作线程和独立服务中执行，避免网络计算阻塞推演。

## 已实现范围

- AFSIM 向 ns-3 提供实体编号、名称、位置、速度、航向、生存状态和通信设备状态。
- 按 AFSIM 的显式通信链路或同一通信网络建立节点关系，并读取真实设备速率、介质时延和消息收发结果。
- 将 AFSIM 实际传输的消息转换为持续业务流，不再使用固定合成流代替真实通信业务。
- ns-3 计算每条流的时延、丢包率、吞吐量、流量方向和链路状态。
- 链路通断由节点生存状态和有效拓扑路径判定，单个采样包丢失只计入丢包率，不会被误判成断链。
- AFSIM 每次投递消息时读取最新 ns-3 结果：链路不通时不发送，丢包命中时不投递，成功消息按 ns-3 时延延后到达。
- ns-3 时延按总投递时延处理，AFSIM 核心按该值校准消息投递时间，并在原介质发送时间更长时同步校准发送完成时间，避免重复累计原介质时延。
- 通信影响只作用于消息，不直接关闭雷达或武器；雷达探测结果、指挥命令和武器协同信息会因消息未到达或延迟到达而改变推演结果。
- 单条效果执行异常会被单独记录，同批后续效果继续执行。
- 指标可通过 HTTP 接口读取，也可导出为 CSV 文件。
- TCP 断线自动重连；积压超过上限时自动改发完整状态，保证重新同步。

## 目录结构

- `src/afsim_ns3_bridge/`：状态管理、ns-3 调用、指标存储、TCP 与 HTTP 服务。
- `src/ns3_runner/`：ns-3.47 网络计算程序。
- `src/windows_adapter/`：可由 Visual Studio 2017 编译的异步 C++ 通信适配层。
- `src/afsim_extension/`：AFSIM 2.9cn 插件，负责读取真实推演实体并在消息投递路径应用通信效果。
- `scripts/`：本地构建、启动和测试脚本。
- `tests/`：协议、增量、重连、合并、指标和 C++ 适配层测试。

## 运行环境

- Windows：AFSIM 2.9cn、Visual Studio 2017、x64、v141 工具集。
- WSL：ns-3.47、Python 3 服务和自动化测试。
- 默认 TCP 端口为 `18080`，默认指标 HTTP 端口为 `18081`。

AFSIM 商业程序和源码不属于本仓库，构建插件时必须使用具有合法授权且版本匹配的本机安装。

## WSL 测试

将 ns-3.47 放在项目内的 `.deps/ns-3.47` 后执行：

```bash
USER=afsimns3 ./scripts/run_tests.sh
```

该命令依次构建 ns-3 计算程序、编译并测试 C++ 适配层，再运行 Python 自动化测试。

## WSL 启动网络服务

```bash
./scripts/build_ns3_runner.sh
./scripts/run_service.sh \
  --host 0.0.0.0 \
  --tcp-port 18080 \
  --http-port 18081 \
  --csv output/metrics/metrics.csv
```

前端可读取以下接口：

- `GET /health`：服务状态。
- `GET /metrics/latest`：最近一次网络指标和效果决策。
- `GET /metrics/export.csv`：全部网络指标 CSV。

## Windows 构建 AFSIM 插件

在 Windows 命令提示符中执行：

```bat
cd /d E:\afsimns3
src\afsim_extension\build_vs2017.bat ^
  E:\afsim_fenbushi\src ^
  E:\afsim_fenbushi\src\build ^
  E:\afsimns3\output\afsim-extension-build
```

脚本固定使用 Visual Studio 2017 x64 和 `RelWithDebInfo`，避免将调试运行库与 AFSIM 现有库混用。构建成功后，将插件复制到 AFSIM 的插件目录：

```bat
copy /Y ^
  E:\afsimns3\output\afsim-extension-build\RelWithDebInfo\wsf_afsim_ns3.dll ^
  E:\afsim_fenbushi\src\build\RelWithDebInfo\mission_plugins\
```

启动 `mission.exe` 前可设置下列环境变量：

| 变量 | 默认值 | 作用 |
|---|---:|---|
| `AFSIM_NS3_HOST` | `127.0.0.1` | ns-3 桥接服务地址 |
| `AFSIM_NS3_PORT` | `18080` | ns-3 桥接服务 TCP 端口 |
| `AFSIM_NS3_INTERVAL_SECONDS` | `1.0` | AFSIM 状态采样间隔，单位为仿真秒 |
| `AFSIM_NS3_DATA_RATE_BPS` | `1000000` | AFSIM 尚未提供速率时采用的回退值 |
| `AFSIM_NS3_DELAY_MS` | `1.0` | 显式设置时作为 ns-3 链路时延；未设置时读取 AFSIM 介质时延 |
| `AFSIM_NS3_LOSS_RATE` | `0.0` | 显式设置时作为 ns-3 链路丢包率；未设置时读取 AFSIM 收发结果 |
| `AFSIM_NS3_LOG` | 当前目录日志文件 | 插件运行日志路径 |
| `AFSIM_NS3_TRACE_TICKS` | `0` | 设为 `1` 时记录每个采样周期 |

## 当前验证结果

- Windows：Visual Studio 2017 原生适配层编译成功，CTest `1/1` 通过。
- WSL：C++ 测试 `1/1`、Python 自动化测试 `10/10` 通过，ns-3.47 计算程序可构建运行。
- AFSIM 2.9cn：插件被 `mission.exe` 成功加载，真实场景完成首次全量和后续增量同步。
- 真实配置与业务流：AFSIM 设备速率读取为 `100000000 bps`，实际消息生成的业务流进入 ns-3 计算。
- 功能效果：消息级通断、丢包和时延由最新 ns-3 结果逐条判定，不再通过关闭雷达或武器实现。
- 指标输出：时延、丢包率、吞吐量、流量方向、链路状态及 CSV 导出均已验证。
- 异常隔离：注入单条雷达效果异常后，错误被记录，后续武器效果仍继续应用。
- 非阻塞更新：快速增量提交约 `11.9 ms`，合并为一次 ns-3 计算。
