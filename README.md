# 集群聊天服务器

## 项目介绍

基于 C++ Muduo 网络库实现的**高并发、分布式、可集群部署**的聊天服务器，支持用户注册登录、一对一聊天、好友管理、群组管理、离线消息、跨服务器消息转发等完整功能。

### 架构概览

```
                         +-----------------------+
                         |    Nginx :8000         |
                         |  TCP Stream Proxy      |
                         |  least_conn + 健康检查   |
                         +----+------+------+-----+
                              |      |      |
                +-------------+------+------+-------------+
                |             |      |      |             |
                v             v      v      v             v
         +------------+ +------------+ +------------+
         |ChatServer  | |ChatServer  | |ChatServer  |   <-- Muduo Reactor
         | :6000      | | :6001      | | :6002      |       对等集群
         +------+-----+ +------+-----+ +------+-----+
                |              |              |
                +--------------+--------------+
                               |
                +--------------+--------------+
                |                             |
                v                             v
         +--------------+          +------------------+
         |    MySQL     |          |     Redis        |
         |  持久化+消息  |          | pub/sub 跨节点通信 |
         |  兜底可靠性   |          | + Cache-Aside 缓存 |
         +--------------+          +------------------+
```

### 消息可靠性: Store-then-Deliver

每条聊天消息采用 **先尝试落库、再推送** 策略，客户端 ACK 后删除离线记录；数据库连接或写入失败需要通过运行日志和监控发现：

```
Sender 发消息
    |
    v
1. 生成 msg_uuid -> 存入 offlineMsg 表 (落库兜底)
    |
    v
2. 尝试实时投递 (本机 TcpConnection 或 Redis pub/sub 跨节点)
    |
    v
3. 客户端收到 -> 回 ACK (含 msg_uuid)
    |
    v
4. 服务端收到 ACK -> 删除 offlineMsg 对应记录
```

- **正常路径**: 消息实时送达 -> ACK -> 清理 -> 无残留
- **异常路径**: 服务器宕机/网络断开 -> 消息留在 DB -> 用户下次登录批量拉取
- **去重**: 登录拉取后全量删除; ACK 已清理的消息不会重复出现

## 技术栈

* C++14
* Muduo Reactor 高并发网络库
* MySQL 8.0 数据持久化 + 连接池
* Redis 发布订阅（跨服务器通信）+ Cache-Aside 缓存
* Nginx TCP Stream 四层负载均衡
* nlohmann json 序列化
* CMake 构建
* 单例模式、线程安全、异常处理

## 功能列表

* [x] 用户注册
* [x] 用户登录/退出
* [x] 一对一聊天
* [x] 好友添加/查询
* [x] 创建群组/加入群组
* [x] 群组聊天
* [x] 离线消息存储与推送
* [x] 多服务器集群部署
* [x] 跨服务器消息转发（Redis pub/sub）
* [x] 服务器异常下线自动处理
* [x] Nginx TCP Stream 四层负载均衡 + 健康检查
* [x] Store-then-Deliver 消息流程（落库、推送、客户端 ACK 删除）
* [x] 客户端 ACK 确认机制

## 目录结构

```
.
├── nginx.conf                 # Nginx TCP Stream 负载均衡配置
├── CMakeLists.txt
├── autobuild.sh
├── run_all_tests.sh           # Apache JMeter 官方压测入口
├── soak_test.sh               # Apache JMeter 浸泡入口
├── cluster.sh                 # 集群管理（启动/停止/扩缩容）
├── bin/                       # 编译产物
│   ├── ChatClient
│   ├── ChatServer
├── include/
│   ├── public.hpp              # 消息类型枚举 + msg_uuid 生成
│   ├── protocol.hpp            # 长度前缀帧协议
│   └── server/
│       ├── chatserver.hpp
│       ├── chatservice.hpp
│       ├── db/
│       │   ├── db.h
│       │   └── connectionpool.hpp
│       ├── model/
│       │   ├── user.hpp
│       │   ├── usermodel.hpp
│       │   ├── friendmodel.hpp
│       │   ├── group.hpp
│       │   ├── groupuser.hpp
│       │   ├── groupmodel.hpp
│       │   └── offlinemessagemodel.hpp
│       ├── redis/
│       │   └── redis.hpp
│       └── threadpool/
│           ├── FixedThreadPool.hpp
│           └── syncQueue_2.hpp
├── src/
│   ├── client/
│   │   └── main.cpp           # 客户端（含ACK逻辑）
│   └── server/
│       ├── main.cpp            # 服务端入口
│       ├── chatserver.cpp      # Muduo TCP Server
│       ├── chatservice.cpp     # 业务逻辑核心（Store-then-Deliver）
│       ├── db/
│       │   └── db.cpp
│       ├── model/
│       │   ├── usermodel.cpp
│       │   ├── friendmodel.cpp
│       │   ├── groupmodel.cpp
│       │   └── offlinemessagemodel.cpp
│       └── redis/
│           └── redis.cpp
├── example/                   # 服务端/客户端示例入口
└── thirdparty/
    └── json.hpp               # nlohmann/json (header-only)
```

性能数据统一使用 `perf/jmeter/` 的 Apache JMeter 计划，长连接使用 `perf/tsung/` 的 Tsung 脚本。

## 编译运行

完整优化过程、压测命令和结果见 [docs/optimization-and-benchmark.md](docs/optimization-and-benchmark.md)。

### 0. 环境准备

```bash
# 安装 MySQL 8.0
sudo apt install mysql-server
sudo mysql_secure_installation

# 安装 Redis
sudo apt install redis-server

# 安装 Nginx（需支持 stream 模块，1.9.0+）
sudo apt install nginx

# 安装编译依赖
sudo apt install g++ cmake make libmuduo-dev libmysqlclient-dev libhiredis-dev
```

### 1. 初始化数据库

```bash
# 登录 MySQL
sudo mysql -u root -p

# 创建数据库和表
CREATE DATABASE chat DEFAULT CHARACTER SET utf8mb4;

CREATE TABLE user (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(50) UNIQUE NOT NULL,
    pwd VARCHAR(50) NOT NULL,
    state INT DEFAULT 0
);

CREATE TABLE friend (
    userid INT,
    friendid INT,
    PRIMARY KEY (userid, friendid)
);

CREATE TABLE `group` (
    id INT PRIMARY KEY AUTO_INCREMENT,
    groupname VARCHAR(50) NOT NULL,
    groupdesc VARCHAR(200) DEFAULT ''
);

CREATE TABLE groupuser (
    id INT PRIMARY KEY AUTO_INCREMENT,
    groupid INT,
    userid INT,
    grouprole VARCHAR(20) DEFAULT 'member'
);

CREATE TABLE offlinemessage (
    userid INT,
    message VARCHAR(2000)
);

EXIT;
```

> **清空测试数据**（数据乱了想重来）：
> ```bash
> mysql -u root -p123456 chat -e "TRUNCATE offlinemessage; TRUNCATE groupuser; TRUNCATE friend; TRUNCATE \`group\`; TRUNCATE user;"
> ```

### 2. 确保 MySQL 和 Redis 在运行

```bash
sudo systemctl start mysql
sudo systemctl start redis-server

# 验证
sudo systemctl status mysql
redis-cli ping   # 应返回 PONG
```

### 3. 编译

```bash
chmod +x autobuild.sh
./autobuild.sh
```

编译产物在 `bin/` 目录下：`ChatServer`/`ChatClient`。性能压测使用 `perf/jmeter/chat_login.jmx` 中的 Apache JMeter TCP Sampler。

### 数据库连接配置

服务端不再从源码读取数据库密码。启动前设置已有数据库的连接参数：

```bash
export CHAT_DB_HOST=127.0.0.1
export CHAT_DB_PORT=3306
export CHAT_DB_USER=root
export CHAT_DB_PASSWORD='你的MySQL密码'
export CHAT_DB_NAME=chat
export CHAT_REDIS_HOST=127.0.0.1
export CHAT_REDIS_PORT=6379
```

### 业务线程池模式

默认使用有界固定线程池：

```bash
export CHAT_THREADPOOL_MODE=fixed
```

也可以在启动时选择从外部 `threadpool` 项目适配的缓存线程池：

```bash
export CHAT_THREADPOOL_MODE=cached
./bin/ChatServer 127.0.0.1 6000
```

缓存线程池会在所有核心线程忙碌且队列有积压时扩容到上限，空闲超时的非核心线程会退出。队列满时会等待空闲位，任务不会回退到 Muduo IO 线程执行。线程池只在进程启动时选择，不能在运行中热切换，否则会破坏登录、退出和聊天消息的顺序；需要切换模式时先停止服务再重新启动。

`cluster.sh` 会复用这些环境变量重置在线状态；未设置 `CHAT_DB_PASSWORD` 时不会执行无密码数据库操作。

### 数据库索引迁移

好友、群成员和离线消息查询依赖以下索引。首次部署或已有数据库升级时执行：

```bash
mysql -u "$CHAT_DB_USER" -p "$CHAT_DB_NAME" \
  < sql/002_performance_indexes.sql
```

迁移脚本会增加 `friend`、`groupuser` 和 `offlinemessage` 的查询索引。重复执行前先检查索引是否已经存在，避免 MySQL 报重复索引错误。

### 连接池告警

当数据库连接池没有空闲连接时，服务端会记录：

```text
MySQL connection pool exhausted; falling back to a temporary connection
```

压测时应同时观察该告警。如果频繁出现，说明业务线程数超过数据库连接池承载能力，应调整 `ChatServer` 的数据库连接池大小或降低并发，而不能只看登录接口的平均延迟。

业务线程池队列达到 80% 时也会记录告警：

```text
business thread pool queue is above 80%
```

该告警表示请求已经开始排队，后续应同时记录队列长度、数据库连接池可用数和接口 P95/P99。

### 4. 启动服务端

**方式 A：单实例（开发调试最常用）**

```bash
cd bin
./ChatServer 127.0.0.1 6000

# 无线程池模式（仅用于已有实现对比）
./ChatServer 127.0.0.1 6000 --no-pool
```

**方式 B：集群管理脚本（推荐）**

```bash
chmod +x cluster.sh
./cluster.sh start          # 启动 3 节点（6000/6001/6002）+ 自动更新 Nginx
./cluster.sh start 5        # 启动 5 节点
./cluster.sh start 3 7000   # 3 节点，从 7000 端口开始
./cluster.sh status         # 查看集群状态
./cluster.sh add 6003       # 热添加节点
./cluster.sh del 6001       # 热移除节点
./cluster.sh stop           # 停止全部
```

**方式 C：手动启动多实例（不用脚本）**

```bash
./bin/ChatServer 127.0.0.1 6000 &
./bin/ChatServer 127.0.0.1 6001 &
./bin/ChatServer 127.0.0.1 6002 &
```

### 5. 启动 Nginx 负载均衡（集群部署时需要）

```bash
# 停掉旧 Nginx（如果有）
sudo nginx -s quit 2>/dev/null

# 使用项目配置启动
sudo nginx -c $(pwd)/nginx.conf

# 重载配置（修改 nginx.conf 后）
sudo nginx -s reload

# 验证监听
sudo ss -tlnp | grep 8000
```

> 如果报 `unknown directive "stream"`：确认 `/etc/nginx/modules-enabled/` 下有 `mod-stream.conf`。没有则执行 `sudo apt install libnginx-mod-stream`。

### 6. 客户端连接

```bash
# 直连单实例（开发调试）
./bin/ChatClient 127.0.0.1 6000

# 通过 Nginx 负载均衡（集群部署，推荐）
./bin/ChatClient 127.0.0.1 8000
```

### 7. 压测

```bash
# 先启动服务端
./bin/ChatServer 127.0.0.1 6000 &

# 官方压测工具
./run_all_tests.sh 50 1 6000
# 浸泡测试
./soak_test.sh 300 50
# 详细说明：perf/jmeter/README.md
```

## 核心设计决策

| 层面 | 决策 | 原因 |
|------|------|------|
| 协议 | 长度前缀帧 (4字节大端头+载荷) | O(1) 粘包处理，替代分隔符方案 |
| 并发 | IO线程 + 业务线程池分离 | 业务逻辑在线程池执行，队列满时在提交边界背压，避免静默丢任务 |
| 缓存 | Redis Cache-Aside | user/friends/groups 三类热点数据缓存 |
| 集群 | Redis pub/sub 对等节点 | 跨服务器消息实时路由 |
| 可靠性 | Store-then-Deliver | 先落 offlineMsg 再推送，DB 兜底 |
| 负载均衡 | Nginx TCP Stream | 单入口 + least_conn + 故障转移 |

## 数据库

```sql
-- 创建数据库
CREATE DATABASE chat DEFAULT CHARACTER SET utf8mb4;

-- 用户表
CREATE TABLE user (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(50) UNIQUE NOT NULL,
    pwd VARCHAR(50) NOT NULL,
    state INT DEFAULT 0  -- 0=离线, 1=在线
);

-- 好友表 (双向复合主键)
CREATE TABLE friend (
    userid INT,
    friendid INT,
    PRIMARY KEY (userid, friendid)
);

-- 群组表
CREATE TABLE `group` (
    id INT PRIMARY KEY AUTO_INCREMENT,
    groupname VARCHAR(50) NOT NULL,
    groupdesc VARCHAR(200) DEFAULT ''
);

-- 群组成员表
CREATE TABLE groupuser (
    id INT PRIMARY KEY AUTO_INCREMENT,
    groupid INT,
    userid INT,
    grouprole VARCHAR(20) DEFAULT 'member'
);

-- 离线消息表 (Store-then-Deliver 兜底存储)
CREATE TABLE offlinemessage (
    userid INT,
    message VARCHAR(2000)  -- 含 msg_uuid 的完整 JSON
);
```
