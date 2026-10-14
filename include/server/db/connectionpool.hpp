#ifndef CONNECTION_POOL_HPP
#define CONNECTION_POOL_HPP

#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>
#include <iostream>

using namespace std;

class ConnectionPool {
public:
    static ConnectionPool *instance() {
        static ConnectionPool pool;
        return &pool;
    }

    // 从池中获取一个连接
    // maxWaitMs: 最大等待毫秒数，0 表示不等待立即返回
    MYSQL *acquire(int maxWaitMs = 3000) {
        unique_lock<mutex> lock(mtx_);
        if (pool_.empty()) {
            if (maxWaitMs == 0) return nullptr;
            cv_.wait_for(lock, chrono::milliseconds(maxWaitMs),
                         [this]() { return !pool_.empty(); });
        }
        if (pool_.empty()) return nullptr;
        MYSQL *conn = pool_.front();
        pool_.pop();
        return conn;
    }

    // 归还连接到池中
    void release(MYSQL *conn) {
        if (conn == nullptr) return;
        lock_guard<mutex> lock(mtx_);
        pool_.push(conn);
        cv_.notify_one();
    }

    // 初始化连接池
    void init(const string &host, const string &user, const string &passwd,
              const string &db, int port = 3306, int poolSize = 8) {
        lock_guard<mutex> lock(mtx_);
        for (int i = 0; i < poolSize; i++) {
            MYSQL *conn = mysql_init(nullptr);
            if (conn == nullptr) {
                cerr << "ConnectionPool: mysql_init failed" << endl;
                continue;
            }
            // MYSQL_OPT_RECONNECT deprecated in MySQL 8.0, auto-reconnect is default
            mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");

            MYSQL *p = mysql_real_connect(conn, host.c_str(), user.c_str(),
                                          passwd.c_str(), db.c_str(),
                                          port, nullptr, 0);
            if (p != nullptr) {
                pool_.push(p);
            } else {
                cerr << "ConnectionPool: connect failed: " << mysql_error(conn) << endl;
                mysql_close(conn);
            }
        }
        cout << "ConnectionPool initialized: " << pool_.size()
             << " connections ready" << endl;
    }

    size_t available() const {
        lock_guard<mutex> lock(mtx_);
        return pool_.size();
    }

private:
    ConnectionPool() = default;
    ~ConnectionPool() {
        lock_guard<mutex> lock(mtx_);
        while (!pool_.empty()) {
            MYSQL *conn = pool_.front();
            pool_.pop();
            mysql_close(conn);
        }
    }
    ConnectionPool(const ConnectionPool &) = delete;
    ConnectionPool &operator=(const ConnectionPool &) = delete;

    queue<MYSQL *> pool_;
    mutable mutex mtx_;
    condition_variable cv_;
};

#endif // CONNECTION_POOL_HPP
