#include "chatserver.hpp"
#include "chatservice.hpp"
#include "protocol.hpp"
#include "json.hpp"
#include <functional>
#include <string>
#include "muduo/base/Logging.h"
using namespace std;
using namespace placeholders;
using json = nlohmann::json;

ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg)
    : _server(loop, listenAddr, nameArg), _loop(loop)
{
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, _1, _2, _3));
    _server.setThreadNum(4);
}

void ChatServer::start()
{
    _server.start();
}

void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    if (!conn->connected())
    {
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

void ChatServer::onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
{
    // 长度前缀帧协议: 4字节长度(大端) + 消息体
    // 时间复杂度 O(1)，无需扫描缓冲区找分隔符
    while (buf->readableBytes() >= 4)
    {
        // 读取4字节长度头（网络字节序 → 主机字节序）
        uint32_t len = buf->peekInt32();

        // 长度合法性检查：拒绝超大或损坏的报文
        if (len > kMaxMessageSize)
        {
            LOG_ERROR << "Message too large: " << len << " bytes, closing connection";
            conn->shutdown();
            return;
        }

        // 检查完整消息是否已到达
        if (buf->readableBytes() < 4 + len)
            break;  // 数据不完整，等待更多数据到来

        // 跳过长度头，取出消息体
        buf->retrieve(4);
        std::string msg(buf->peek(), len);
        buf->retrieve(len);

        processMessage(conn, msg, time);
    }
}

void ChatServer::processMessage(const TcpConnectionPtr &conn, const string &msg, Timestamp time)
{
    try
    {
        json js = json::parse(msg);
        if (!js.contains("msgid"))
        {
            LOG_ERROR << "JSON missing msgid";
            return;
        }
        int msgid = js["msgid"].get<int>();
        auto handler = ChatService::instance()->getHandler(msgid);
        if (handler)
        {
            // 深拷贝，确保handler拿到的json不会被外部修改
            string msg_data = js.dump();
            json safe_js = json::parse(msg_data);
            handler(conn, safe_js, time);
        }
        else
        {
            LOG_ERROR << "No handler for msgid: " << msgid;
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "Parse error: " << e.what() << ", raw: [" << msg << "]";
    }
}
