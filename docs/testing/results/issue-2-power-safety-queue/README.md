# Issue #2：满队列下的电源安全命令

修复提交：`c9f03ae2b2bab6d786409c834a7e3f1b3e6397c5`，V1.0.8。基线为
`c02bf611cbe88dc3b2fc50f1925ab7abdd8d23f4`。本项仅处理 #2 的队列接收、取消与有界性，
不把 #11 的主窗口最终掉电告警或 #23 的实体设备验收一并算作已解决。

## 修改与边界

- 关闭状态单独检查；请求验证、requestId 冲突和重复处理发生在队列修改之前。
- 普通请求仍仅在当前待执行队列少于128条时接收；正常容量溢出仍拒绝。
- OFF、stop、disconnect 不受普通容量检查提前拒绝，先取消待执行的 ON/任务/序列/计划。
- 安全请求按语义范围合并：7个非空 OFF 通道集合、stop、disconnect，共9种。
  通道数组的顺序不影响合并，不取不同 OFF 通道集合的并集。
- 同范围旧请求被替代时记为 canceled，不记为 completed；同 requestId 重放不重新排队。
  有界动作历史仍可使较早的 canceled 记录过期，不能承诺永久可查。
- 即使128条积压均为不能按既有策略取消的普通请求，也有额外安全容量；最坏待执行队列137条。
  不为预留容量丢弃无关普通操作。
- 接收返回 queued；实际执行、硬件读回、失败与unknown处理保留。不能中断已经开始的设备I/O，
  不改变GUI勾选通道、任务拥有通道或网络权限。

GUI电源按钮、任务停止、MainWindow全部断开及网络power.output仍进入同一个PowerService::Submit。
网络通道继续来自GUI选定集合，输入来源为network；本次没有开放新网络操作。

## 本地实际运行

在Linux用g++ C++17编译原样提取的生产Submit、Require与ValidateChannels，
仅用不消费队列的受控调度替代Windows设备工作线程。两份源程序和摘要均保留，未修改断言使旧版通过。

| 对象 | 结果 |
|---|---|
| 基线 | 32条受控检查中15条失败，复现容量拒绝及其后果 |
| 修复 | 32条受控检查全部通过 |
| 128条ON积压 | 本地OFF、网络OFF、stop、disconnect各自可接收，旧ON全部取消 |
| 请求边界 | 非法通道、冲突requestId不改变队列；满队列重复请求不重新执行；普通超限拒绝 |
| 128条非取消积压 | 100轮×9种安全请求共900次接收，队列始终不超过137，动作历史不超过256 |
| 合并与关闭 | 顺逆序通道集合一致；保留的旧OFF记录为canceled；无关普通请求保留；shutdown仍拒绝新请求 |

复跑保存的证据程序（输出到临时目录，不需要改生产源码）：

```sh
g++ -std=c++17 -O2 -pthread -Ithird_party/json \
  docs/testing/results/issue-2-power-safety-queue/after-probe.cpp -o /tmp/serialctl-power-admission
/tmp/serialctl-power-admission
```

同样编译before-probe.cpp应返回失败，不能把其非零退出码报为环境故障。
证据程序是固定提交的提取副本；后续回归以现行生产代码和原生PowerServiceTest为准。

## Windows原生回归

PowerServiceTest新增6个场景：4种满ON队列安全调用、非取消积压与合并、OFF读回失败。
它使用生产服务、真实工作线程和已有simulation后端；测试专用暂停接缝以条件变量确认消费线程已暂停，
不靠固定sleep猜测饱和时机。该接缝仅由SERIALCTL_POWER_TESTING启用，生产GUI目标不含它。

恢复消费后检查：安全请求真正执行；CH1按勾选/拥有范围关闭，旧CH2 ON不执行，未选CH3保持；
断连及注入OFF读回失败时输出保持unknown，失败不被写成已确认关闭；暂停状态的Shutdown仍可退出。

执行入口：scripts/build.ps1、scripts/test.ps1、scripts/check.ps1。
[首次Windows流水线](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39525281696)失败。
公开网页仅显示构建/测试步骤exit 1，当前会话无法下载原始Windows日志。

在Linux受控诊断模型中，保留生产队列、工作线程、执行、simulation及原测试逻辑，
仅替代Win32时间/编码、未使用的硬件后端和文件日志。模型复现了新增读回断言失败：
累计读取次数注入未命中单通道OFF读回，实际收到confirmed/completed。
测试随后改为仅在测试目标启用的精确CHAN:OUTP?超时注入，原失败/unknown要求保留；
改后整组PowerServiceTest在该模型中返回0。该结果不冒充Windows原生或真实设备通过。
生产Submit与c9f03ae保持逐字一致。Windows原生复跑 [39526391816](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39526391816)
已于2026-10-21成功完成，对应源码/测试提交60616269ad4aa978f6ef5a8af13eaa2ac222964c。
标准流程强制BUILD_TESTING=ON，当前17项CTest包含上述新增场景，并执行PE兼容与发布包重开检查、
应用启动冒烟、更新异常场景及Python回归。此处依据匹配提交的工作流最终成功状态及其强制步骤；
原始Windows日志和当前EXE摘要尚未下载，不虚构逐项运行耗时或原始日志内容。
main已快进到同一6061626；[主分支流水线39527088549](https://github.com/john2000smith7-del/SerialCtl/actions/runs/39527088549)
也已成功完成，2026-10-21 01:31 UTC复核。修复前后受控复现、原生测试新增断言和匹配提交的两次成功流程
共同支持关闭#2的软件队列缺陷；原始Windows日志/EXE摘要获取限制仍保留，不将下述现场项目算作已验收。

真实Windows 7、IT6332A USB/RS232、物理拔线、真实GUI满载点击和真实WebSocket故障场景：**未运行**。
CI受控后端与Linux提取程序不能替代它们。兼容平台、/MT、PE6.01、YY-Thunks、PuTTY和NI-VISA输入未更改。
