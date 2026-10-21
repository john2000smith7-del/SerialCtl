# Issue #4：完整响应长度边界

基线main `a0cbf46`，V1.0.8；其网络代码与`2fa1b16`相同。本项不提高128 KiB协议上限。

MakeApiResponse统一生成并按完整UTF-8序列化结果检查response，包含requestId、转义和外层字段；
正常、subscribe提前返回和无效对象响应使用同一构造函数。缓存保存实际发送的有界响应。
events超限返回HISTORY_TOO_LARGE，可改用subscribe；其他过大结果/错误详情返回RESPONSE_TOO_LARGE。
不截断JSON或历史、不关闭健康连接、不扩大操作权限，不把无法返回大结果解释成设备操作已经撤销。
Events原有内部result大于上限时的快速拒绝仍保留；完整外层检查补齐原漏洞。

## 已运行的本地证据

- before-probe.cpp原样提取基线生产EventsLocked/Events，受控注入合法Base64历史，再按基线response结构封装。
  实测内部131069字节未报错，完整报文131309字节，大于131072。它返回0表示成功复现缺陷。
- 当前生产ApiResponse.h与ApiResponseTest.cpp在Linux g++ C++17编译运行通过：
  131071/131072字节保留完整结果，131073返回小型错误；requestId为1/128字节及128个需转义字符，
  元信息包含中文；普通HISTORY_GAP、非history结果超限和超大错误详情也核对。
- 移除完整长度判断的受控变异构建返回1，保留removed-boundary-result.log，证明断言能捕获回归。
- wire-probe.cpp实际调用生产响应函数，verify-wire.py将其输出喂给生产Python Wire.receive。
  9份合法结果/错误和紧随其后的第二份响应均可解析；未修复的131073声明被拒绝。
  这是预装完整帧模型，不是握手、套接字或Windows端到端运行。wire-result.json保留实际长度。

复跑：

```sh
g++ -std=c++17 -O2 -Isrc/network -Ithird_party/json tests/unit/ApiResponseTest.cpp -o /tmp/api-response
/tmp/api-response
g++ -std=c++17 -O2 -Isrc/network -Ithird_party/json docs/testing/results/issue-4-complete-response-limit/wire-probe.cpp -o /tmp/wire-response
/tmp/wire-response > docs/testing/results/issue-4-complete-response-limit/wire-responses.jsonl
python3 docs/testing/results/issue-4-complete-response-limit/verify-wire.py
```

wire-responses.jsonl为临时大报文，不需提交。可把整个证据目录复制到临时目录，再从仓库根目录运行脚本。

## 原生服务器与Python客户端的新增回归

GatewayHistoryBoundary通过生产Publish生成6份正常事件，每份原始输出不超过16384字节；
以最后一份的合法Base64长度及0–3字节source填充形成精确完整报文，不直接写私有history容器。
原生ApiServerTest和Python test_gateway_client各覆盖1/128字节requestId×limit−1/limit/limit+1：

1. 近上限完整结果或HISTORY_TOO_LARGE业务错误正确，重复请求回放相同有界响应。
2. 同一连接继续resources查询、subscribe，6份原始事件按序完整回放，并继续接收实时输出。
3. 原生还检查冲突requestId拒绝；已有未知请求、伪造资源、0–255及多客户端回归保留。
4. 测试fixture的history-boundary/clear history命令仅存在于测试程序stdin，不进入生产协议。

Windows：**待运行**。标准入口scripts/build.ps1、scripts/test.ps1、scripts/check.ps1，当前配置19项CTest。
运行后附匹配提交的结果；当前会话原始Actions日志与EXE摘要下载仍受api.github.com策略限制。
真实Win7、现场客户端/物理串口与长时测试：**未运行**。兼容目标、/MT、PE6.01、YY-Thunks、
固定PuTTY、许可证和NI-VISA输入未更改。
