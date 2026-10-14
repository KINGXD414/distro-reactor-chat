#include "db.h"
#include "connectionpool.hpp"
#include <muduo/base/Logging.h>
#include <vector>
#include <cstdlib>
using namespace std;

static string envOrDefault(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

MySQL::MySQL()
    : _conn(nullptr), _fromPool(false)
{
}

MySQL::~MySQL()
{
    if (_conn != nullptr)
    {
        if (_fromPool)
        {
            ConnectionPool::instance()->release(_conn);
        }
        else
        {
            mysql_close(_conn);
        }
        _conn = nullptr;
    }
}

bool MySQL::connect()
{
    // 优先从连接池获取（不阻塞等待，避免 IO 线程饥饿）
    _conn = ConnectionPool::instance()->acquire(0);
    if (_conn != nullptr)
    {
        _fromPool = true;
        return true;
    }

    // Fallback: 直接创建新连接
    LOG_WARN << "MySQL connection pool exhausted; falling back to a temporary connection, available="
             << ConnectionPool::instance()->available();
    _conn = mysql_init(nullptr);
    const string server = envOrDefault("CHAT_DB_HOST", "127.0.0.1");
    const string user = envOrDefault("CHAT_DB_USER", "root");
    const string password = envOrDefault("CHAT_DB_PASSWORD", "");
    const string dbname = envOrDefault("CHAT_DB_NAME", "chat");
    const int port = atoi(envOrDefault("CHAT_DB_PORT", "3306").c_str());
    MYSQL *p = mysql_real_connect(_conn, server.c_str(), user.c_str(),
                                  password.c_str(), dbname.c_str(), port, nullptr, 0);
    if (p != nullptr)
    {
        mysql_query(_conn, "set names utf8mb4");
        _fromPool = false;
    }
    else
    {
        LOG_ERROR << "connect mysql fail!";
        mysql_close(_conn);
        _conn = nullptr;
        return false;
    }
    return true;
}

bool MySQL::update(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_ERROR << __FILE__ << ":" << __LINE__ << ":" << sql << " 更新失败!";
        return false;
    }
    return true;
}

MYSQL_RES *MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_ERROR << __FILE__ << ":" << __LINE__ << ":" << sql << " 查询失败!";
        return nullptr;
    }
    // 改用 mysql_store_result：一次性将结果集从服务端全部取到客户端内存
    // 避免 mysql_use_result 因未取完行导致的 "Commands out of sync" 错误
    return mysql_store_result(_conn);
}

MYSQL *MySQL::getConnection()
{
    return _conn;
}

string MySQL::escape(const string &str)
{
    if (_conn == nullptr) return str;
    vector<char> buf(str.size() * 2 + 1);
    mysql_real_escape_string(_conn, buf.data(), str.c_str(), str.size());
    return string(buf.data());
}
