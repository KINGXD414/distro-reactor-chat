# 官方压测工具配置包

此目录包含 Apache JMeter 测试计划和 Tsung TCP 长连接测试模板。测试计划调用的是现成工具；Groovy 仅在 JMeter 内构造项目协议报文，不承担压测调度或统计工作。

## 工具与覆盖范围

- `chat_login.jmx`：登录接口并发、成功率、吞吐量和响应延迟。
- `chat_business.jmx`：先登录，再调用项目已有的创建群组请求；JMeter 分别记录 Login 和 Create group 样本。
- `tcp_connections.xml.in` + `run_tsung.sh`：不发送应用报文的 TCP 连接建立与保持测试，观察连接数、接入速率、FD、CPU、内存和稳定性。

JMeter TCP Sampler 官方支持 `LengthPrefixedBinaryTCPClientImpl`，用 `-Jtcp.binarylength.prefix.length=4` 匹配项目的四字节长度前缀。JMeter 测试计划要求登录用户已经存在，线程数不能超过可用的连续测试账号数。创建群组会实际写入数据库，每线程创建一个群，测试前后应清理测试群数据。注册计划会实际创建用户，使用独一无二的 `run_id`。

当前添加好友、加入群组、单聊、群聊请求没有向发送端返回业务完成 ACK；因此 JMeter 的 TCP 请求-响应耗时不能代表这些操作的业务完成延迟。不要把发送时间写成端到端消息延迟。若要测消息端到端延迟，需要有接收端/消息 ID 关联统计，或先完善项目已有 ACK 流程再压测。

## WSL2 安装

```bash
sudo apt update
sudo apt install -y jmeter tsung gettext-base erlang-nox
```

如果发行版仓库没有 `jmeter`，按 Apache JMeter 官方发行包安装并确保 Java 运行环境可用。JMeter 官方建议通过非 GUI 模式运行负载测试。

## JMeter 登录

```bash
cd /mnt/d/C++perject/MuduoChatServer
mkdir -p results
jmeter -n -t perf/jmeter/chat_login.jmx \
  -Jserver=127.0.0.1 -Jport=6000 \
  -Jthreads=10 -Jloops=1 -Jramp_time=5 \
  -Jbase_user_id=1001 -Jpassword=123456 \
  -Jtcp.binarylength.prefix.length=4 \
  -l results/login.jtl -e -o results/login-report
```

线程 `N` 登录账号 `base_user_id + N`。先准备连续账号；不要让同一账号在已有会话期间再次登录。

## JMeter 登录及创建群组

```bash
jmeter -n -t perf/jmeter/chat_business.jmx \
  -Jserver=127.0.0.1 -Jport=6000 \
  -Jthreads=10 -Jramp_time=5 \
  -Jbase_user_id=1001 -Jpassword=123456 \
  -Jtcp.binarylength.prefix.length=4 \
  -l results/business.jtl -e -o results/business-report
```

每个线程先登录唯一账号，再创建一个唯一命名的群。单次只运行一次；不要循环创建。此计划会改写群组表。

## Tsung TCP 长连接保持

启动被测 ChatServer 后，在另一 WSL 终端运行：

```bash
cd /mnt/d/C++perject/MuduoChatServer
chmod +x perf/tsung/run_tsung.sh
ulimit -n 65535
./perf/tsung/run_tsung.sh 127.0.0.1 6000 1000 20 300
```

参数依次是目标 IP、端口、总连接数、连接到达速率（每秒）、连接保持秒数。上述例子逐步建立 1000 条连接，目标约 20 条/秒，每条保持 300 秒。Tsung raw session 在 TCP 建连后保持连接，不发送长度前缀或业务 JSON；它测的是 TCP accept/长连接资源，不是登录/聊天业务性能。Tsung 会在 `perf/tsung/results/` 保存配置与报告数据。

扩大连接数之前，分别设置服务端和压测端的 FD 限制，并确认压测端没有先耗尽资源：

```bash
ulimit -n 65535
cat /proc/$(pgrep -n ChatServer)/limits | grep -i 'open files'
ls /proc/$(pgrep -n ChatServer)/fd | wc -l
ss -s
pidstat -p "$(pgrep -n ChatServer)" 1
```

`ulimit` 只影响从当前 shell 启动的进程；ChatServer 必须在提高限制之后启动。单机 WSL 同时运行服务端和 Tsung 时，压测端会争用 CPU、内存和文件描述符，结果适合功能和趋势验证，不等同于独立压测机/生产 Linux 的容量结论。Tsung 文档也要求按并发连接提高压测进程的 FD 限制。

## 查看结果

- JMeter：打开对应的 `*-report/index.html`，检查样本成功率、吞吐及 P50/P90/P95/P99。
- Tsung：运行结束后使用生成的 Tsung 日志目录与 HTML 报告，重点看 `connected`、连接失败、连接/断开速率及服务端 FD/CPU/内存。
- 每次记录 commit、目标端口（直连或 Nginx 8000）、线程/连接数、到达速率、保持时长、WSL 资源和数据库/Redis 状态。登录、建群、长连接三种结果分开报告。
