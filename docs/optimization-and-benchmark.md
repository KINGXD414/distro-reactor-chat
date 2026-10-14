# MuduoChatServer 优化与性能测试记录

## 1. 范围

本次只完善已有功能、可靠性、并发执行和压测方法，没有新增业务模块。已有功能包括登录、注册、注销、好友、一对一聊天、群组、群聊、离线消息、Redis 发布订阅和 MySQL 持久化。

## 2. 已完成的优化

### 2.1 业务可靠性

- 补齐创建群组成功 ACK，客户端可以得到 `groupid`。
- 聊天消息采用 Store-then-Deliver：先保存离线消息，再推送在线用户。
- 增加客户端 ACK，确认后删除离线消息。
- 完善登录、注销和异常断开时的连接映射清理。
- 增加连接到用户的反向索引，不信任客户端提交的用户 ID。
- 保持登录、注销和聊天消息在同一业务处理路径中执行，避免乱序。

### 2.2 MySQL、Redis 和配置

- 数据库和 Redis 地址、端口、用户、密码改为环境变量。
- 未设置 `CHAT_DB_PASSWORD` 时拒绝空密码启动。
- Redis 连接失败时输出具体地址和错误原因，并释放失败上下文。
- 修复数据库连接池可用连接数读取的数据竞争。
- 连接池耗尽时记录告警，并保留临时连接降级路径。
- 新增 `sql/002_performance_indexes.sql`，为 `friend`、`groupuser` 和 `offlinemessage` 增加查询索引。

### 2.3 线程池

新增统一接口：

```text
include/server/threadpool/TaskExecutor.hpp
```

固定线程池和缓存线程池均实现该接口。

固定线程池：有界队列、固定工作线程、队列满时在提交边界背压，不在 Muduo IO 线程执行数据库任务。

缓存线程池：

```text
include/server/threadpool/CachedThreadPool.hpp
```

- 核心线程全部忙碌且有积压时扩容到上限。
- 非核心线程空闲超时退出。
- 队列满时等待空闲位置，不丢任务、不回退到 IO 线程。
- 停止时等待任务完成并 `join`，没有复制外部实现中的 `detach` 风险。

启动时选择模式：

```bash
export CHAT_THREADPOOL_MODE=fixed
export CHAT_THREADPOOL_MODE=cached
```

线程池不能运行中热切换，切换模式必须重启服务，以保证任务生命周期和同一用户消息顺序。

## 3. 压测工具

不使用自研 benchmark 作为性能结论依据。

- Apache JMeter：`perf/jmeter/chat_login.jmx`、`perf/jmeter/chat_business.jmx`，测试登录、登录+创建群组、平均值、P50、P95、P99、最大值和错误率。
- Tsung：`perf/tsung/run_tsung.sh`，测试 TCP 长连接建立、保持、释放和文件描述符回收。

JMeter 使用官方 TCP Sampler，4 字节长度前缀对应服务端协议。

## 4. 测试条件

测试在 WSL2 Ubuntu 22.04 本地环境进行，服务端、MySQL、Redis 和压测工具在同一台机器上。结果用于功能和版本对比，不能直接等同于生产容量。

```bash
export CHAT_DB_HOST=127.0.0.1
export CHAT_DB_PORT=3306
export CHAT_DB_USER=root
export CHAT_DB_PASSWORD='你的MySQL密码'
export CHAT_DB_NAME=chat
export CHAT_REDIS_HOST=127.0.0.1
export CHAT_REDIS_PORT=6379
ulimit -n 65535
```

JMeter 测试账号为 `3131–3230`，密码为 `123456`。每轮测试前执行：

```bash
mysql -u root -p chat -e \
"UPDATE user SET state=0 WHERE id BETWEEN 3131 AND 3230;"
for id in $(seq 3131 3230); do redis-cli DEL user:$id >/dev/null; done
```

## 5. JMeter 结果

### 5.1 登录

单用户登录多次运行耗时约 23–42 ms，响应码 200，错误率 0%。

10 用户登录的一次结果：平均 30.40 ms，P50 26 ms，P90 46.90 ms，P95 47 ms，P99 47 ms，错误率 0%。

### 5.2 50 用户业务（100 请求）

| 模式 | 平均 | P50 | P95 | P99 | 最大值 | 错误率 |
|---|---:|---:|---:|---:|---:|---:|
| 固定线程池 | 38.76 ms | 29 ms | 66 ms | 70 ms | 70 ms | 0% |
| 缓存线程池 | 37.12 ms | 29 ms | 62 ms | 73 ms | 74 ms | 0% |

### 5.3 100 用户业务（200 请求）

| 模式 | 平均 | P50 | P95 | P99 | 最大值 | 错误率 |
|---|---:|---:|---:|---:|---:|---:|
| 固定线程池 | 39.36 ms | 31 ms | 67 ms | 73 ms | 155 ms | 0% |
| 缓存线程池 | 38.11 ms | 35 ms | 64 ms | 72 ms | 110 ms | 0% |

### 5.4 100 用户突发业务（ramp_time=1，200 请求）

第一次运行：固定线程池平均 273.60 ms、P95 880 ms、P99 1205 ms、最大 1221 ms；缓存线程池平均 195.77 ms、P95 645 ms、P99 684 ms、最大 698 ms；两者错误率均为 0%。

第二次运行：固定线程池平均 258.71 ms、P95 1220 ms、P99 1329 ms、最大 1330 ms；缓存线程池平均 250.46 ms、P95 782 ms、P99 962 ms、最大 1209 ms；两者错误率均为 0%。

结论：缓存线程池主要改善突发流量下的排队和尾延迟，吞吐量提升不稳定；平滑流量下两种模式接近。

### 5.5 1000 长连接 + 100 用户业务

缓存线程池混合测试的一次结果：

| 接口 | 请求数 | 平均 | P95 | P99 | 最大值 | 错误率 |
|---|---:|---:|---:|---:|---:|---:|
| Login | 100 | 212.73 ms | 288 ms | 292 ms | 293 ms | 0% |
| Create group | 100 | 285.87 ms | 842 ms | 873 ms | 873 ms | 0% |

另一次 10 秒平滑启动的混合验证中，创建群组平均 58.82 ms、P95 67 ms、P99 83 ms、最大 108 ms、错误率 0%。这说明突发速率和运行状态会显著影响尾延迟。

## 6. Tsung 结果

配置：1000 个连接，20/s 到达，保持 300 秒。

固定线程池和缓存线程池均完成：建立 1000、保持约 300 秒、释放 1000、连接错误 0、未确认请求 0、最终活动连接 0。`ss` 峰值约 2001 是客户端和服务端 socket 加监听 socket 的总数，不等于 2001 个业务客户端。

该测试只验证 TCP 连接保持，不包含登录和聊天业务。

## 7. 数据库观察

`SHOW ENGINE INNODB STATUS` 观察到 `0 queries inside InnoDB`、`0 queries in queue`、无明显锁等待、Buffer pool 命中率 1000/1000、无 pending reads/writes。服务端也未出现线程池队列 80% 或 MySQL 连接池耗尽告警。因此当前规模下的突发尾延迟不能简单归因于 MySQL 锁等待或连接池容量不足。

## 8. 最终结论

1. 已有聊天功能、群组 ACK 和离线消息流程已验证。
2. 固定线程池适合作为稳定基线；缓存线程池适合突发业务。
3. 缓存线程池的主要收益是降低 P95/P99 和最大延迟，而不是稳定提升吞吐。
4. 线程池模式必须启动时选择，不能运行中热切换。
5. WSL2 本地测试验证了 1000 长连接保持 300 秒并全部释放，但不能据此宣称生产环境支持固定数量的长连接。
6. 所有性能结论必须注明并发数、ramp time、保持时间、账号数量、数据库/Redis 位置、FD 限制和工具版本。

## 9. 复测命令

```bash
cd /mnt/d/C++perject/MuduoChatServer
export CHAT_DB_PASSWORD='你的MySQL密码'
./autobuild.sh
```

```bash
export CHAT_THREADPOOL_MODE=fixed   # 或 cached
./bin/ChatServer 127.0.0.1 6000
```

```bash
ulimit -n 65535
./perf/tsung/run_tsung.sh 127.0.0.1 6000 1000 20 300
```

```bash
$HOME/apache-jmeter-5.6.3/bin/jmeter -n \
  -t perf/jmeter/chat_business.jmx \
  -Jserver=127.0.0.1 -Jport=6000 \
  -Jthreads=100 -Jramp_time=1 \
  -Jbase_user_id=3131 -Jpassword=123456 \
  -Jtcp.binarylength.prefix.length=4 \
  -l results/business-100user.jtl \
  -e -o results/business-100user-report
```
