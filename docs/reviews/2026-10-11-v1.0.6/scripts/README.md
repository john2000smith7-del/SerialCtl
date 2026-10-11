# 审核补充复现脚本

这些脚本保留原始内容，依赖审核目录旁的 `source/` 固定源码；不修改生产实现。不要直接在仓库文档目录生成测试二进制或覆盖已归档日志。先复制本审核目录到独立临时目录，再获取被审SHA：

```bash
cp -a docs/reviews/2026-10-11-v1.0.6 /tmp/serialctl-review-repro
cd /tmp/serialctl-review-repro
git clone --no-checkout https://github.com/john2000smith7-del/SerialCtl.git source
git -C source checkout --detach 5f12678e4da327f146edb5ce12eb3b3ea9c8afd0
python3 scripts/security_source_harness.py
python3 scripts/verify-wire-boundary.py
bash scripts/run-architecture-model.sh
python3 scripts/desktop_layout_model.py
g++ -std=c++17 -O2 -Isource/src/terminal evidence/product/paste-tail-probe.cpp -o /tmp/serialctl-paste-probe
/tmp/serialctl-paste-probe
python3 scripts/run-portable-tests.py
python3 source/tests/integration/test_serialctl_client.py
```

环境要求Linux、Python3、g++支持C++17；生产Windows程序仍须按仓库PowerShell入口在Windows构建。`security_source_harness.py`原样提取Submit/Events函数，替代不消费的power worker和内存历史；`verify-wire-boundary.py`预装帧头代替真实socket；双终端脚本运行原生产TerminalModel；paste探针替代发送与GUI。它们不验证Win32窗口、真实CMD执行、串口/电源硬件或Win7。

`security_handshake_probe.py --help`给出候选握手复验参数；本轮未运行该联网探针，只应指向有权限的测试服务。完整GUI操作仍使用被审SHA的 `source/scripts/smoke-ui.ps1`，现场和高DPI检查按 [人工清单](../reports/09-manual-acceptance.md)。
