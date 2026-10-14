# Apache JMeter 压测

项目使用 Apache JMeter 的 TCP Sampler，不再把仓库内自研 benchmark 作为性能结论依据。

## 前置条件

- 先启动现有 `ChatServer`，确认监听地址和端口。
- 测试账号必须提前在项目已有的用户表中创建，密码通过 `-Jpassword` 传入。
- 默认每个 JMeter 线程只登录一次（`loops=1`）。线程 `N` 使用账号 `base_user_id + N`；如果提高循环次数，必须预先准备足够的不同账号，否则服务端的重复登录保护会把后续样本判为失败。

## 运行

```bash
jmeter -n -t perf/jmeter/chat_login.jmx \
  -Jthreads=50 -Jloops=1 -Jport=6000 \
  -Jbase_user_id=1000 -Jpassword=change-me \
  -Jtcp.binarylength.prefix.length=4 \
  -l results/login.jtl -e -o results/login-report
```

`LengthPrefixedBinaryTCPClientImpl` 是 JMeter 官方 TCP 实现，`tcp.binarylength.prefix.length=4` 对应服务端的 4 字节长度前缀。JMeter 负责连接复用、并发调度、超时、成功率、吞吐和百分位延迟统计；计划中的 Groovy 代码只负责把 JSON 转成协议需要的十六进制载荷并校验响应。

仓库根目录的 `run_all_tests.sh` 和 `soak_test.sh` 已封装上述参数。长连接、登录延迟和聊天消息吞吐要分开报告，报告同时注明 Git commit、并发数、是否经过 Nginx、缓存冷/热状态以及数据库/Redis 部署位置。采集时记录 `ulimit -n`、`/proc/<pid>/limits`、`fs.file-max`、`ss -s`、CPU、RSS、FD 和 MySQL/Redis 状态。

这份计划只对已有登录接口做请求级压测，不能直接推出“单机承载多少长连接”或集群吞吐结论；这类结论必须单独记录连接保持时长、内核/进程 FD 参数和压测机资源。
