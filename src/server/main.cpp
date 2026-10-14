#include "chatserver.hpp"
#include "chatservice.hpp"
#include "FixedThreadPool.hpp"
#include "CachedThreadPool.hpp"
#include "connectionpool.hpp"
#include <iostream>
#include <signal.h>
#include <string.h>
#include <cstdlib>
#include <memory>
#include <algorithm>
using namespace std;

pool::TaskExecutor *g_pool = nullptr;

static string envOrDefault(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

void resetHandler(int)
{
    ChatService::instance()->reset();
    if (g_pool) g_pool->Stop();
    exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatServer 127.0.0.1 6000 [--no-pool]" << endl;
        exit(-1);
    }

    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    // 解析可选参数：--no-pool 禁用业务线程池，业务直接在 IO 线程执行
    bool usePool = true;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--no-pool") == 0) {
            usePool = false;
        }
    }
    const string poolMode = envOrDefault("CHAT_THREADPOOL_MODE", "fixed");

    // 初始化 MySQL 连接池（根据是否用线程池调整大小）
    int poolSize = usePool ? 32 : 4;
    const string dbHost = envOrDefault("CHAT_DB_HOST", "127.0.0.1");
    const string dbUser = envOrDefault("CHAT_DB_USER", "root");
    const string dbPassword = envOrDefault("CHAT_DB_PASSWORD", "");
    const string dbName = envOrDefault("CHAT_DB_NAME", "chat");
    const int dbPort = atoi(envOrDefault("CHAT_DB_PORT", "3306").c_str());
    if (dbPassword.empty())
    {
        cerr << "CHAT_DB_PASSWORD is not set; refusing to start with an empty database password" << endl;
        return 2;
    }
    ConnectionPool::instance()->init(dbHost, dbUser, dbPassword, dbName, dbPort, poolSize);

    if (usePool)
    {
        const size_t hardwareThreads = std::max(1u, thread::hardware_concurrency());
        std::unique_ptr<pool::TaskExecutor> businessPool;
        if (poolMode == "cached")
        {
            // Do not create more DB-bound workers than the configured DB pool
            // can normally serve; the cached pool is for bursts, not unlimited
            // database concurrency.
            const size_t coreThreads = std::max<size_t>(1, std::min(hardwareThreads, static_cast<size_t>(poolSize)));
            const size_t maxThreads = std::max(coreThreads, std::min(hardwareThreads * 2, static_cast<size_t>(poolSize * 2)));
            businessPool.reset(new pool::CachedThreadPool(5000, coreThreads, maxThreads));
            cout << ">>> Mode: CACHED ThreadPool (core=" << coreThreads
                 << ", max=" << maxThreads << "), DB pool=" << poolSize << endl;
        }
        else
        {
            businessPool.reset(new pool::FixedThreadPool(5000, hardwareThreads));
            cout << ">>> Mode: FIXED ThreadPool (size=" << hardwareThreads << "), DB pool=" << poolSize << endl;
        }
        g_pool = businessPool.get();
        ChatService::instance()->setThreadPool(g_pool);

        signal(SIGINT, resetHandler);

        EventLoop loop;
        InetAddress addr(ip, port);
        ChatServer server(&loop, addr, "ChatServer");
        server.start();
        loop.loop();
    }
    else
    {
        cout << ">>> Mode: NO ThreadPool (IO threads only), DB pool=" << poolSize << endl;

        signal(SIGINT, resetHandler);

        EventLoop loop;
        InetAddress addr(ip, port);
        ChatServer server(&loop, addr, "ChatServer");
        server.start();
        loop.loop();
    }

    return 0;
}
