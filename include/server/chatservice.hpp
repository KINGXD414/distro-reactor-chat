#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <arpa/inet.h>
#include "json.hpp"
#include "usermodel.hpp"
#include "offlinemessagemodel.hpp"
#include "friendmodel.hpp"
#include "groupmodel.hpp"
#include "redis.hpp"
#include "TaskExecutor.hpp"
#include <muduo/base/Logging.h>

using json = nlohmann::json;
using namespace muduo;
using namespace muduo::net;
using namespace std;

using MsgHandler = std::function<void(const TcpConnectionPtr &conn, json &js, Timestamp)>;

// ============================================================
// 长度前缀帧发送（Muduo TcpConnection 版本）
// 帧格式: 4字节长度(大端) + payload
// ============================================================
inline void sendWithLength(const TcpConnectionPtr& conn, const std::string& payload)
{
    uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
    std::string framed;
    framed.reserve(4 + payload.size());
    framed.append(reinterpret_cast<const char*>(&netLen), 4);
    framed.append(payload);
    conn->send(framed.data(), framed.size());
}

class ChatService
{
public:
    static ChatService *instance();
    void setThreadPool(pool::TaskExecutor *pool);
    void login(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void reg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void loginout(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void clientCloseException(const TcpConnectionPtr &conn);
    void reset();
    MsgHandler getHandler(int msgid);
    void handleRedisSubscribeMessage(int userid, string msg);

    // ====== Store-then-Deliver: 客户端消息确认 ======
    void oneChatAck(const TcpConnectionPtr &conn, json &js, Timestamp time);
    void groupChatAck(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 返回连接完成登录后绑定的用户 ID；未认证连接返回 -1。
    int authenticatedUserId(const TcpConnectionPtr &conn);

private:
    ChatService();
    // 调度：有线程池则投递，否则在当前线程直接执行
    template <typename Func>
    void dispatch(Func &&func) {
        if (m_threadPool) {
            const size_t queued = m_threadPool->queueSize();
            const size_t capacity = m_threadPool->queueCapacity();
            if (capacity > 0 && queued * 100 >= capacity * 80) {
                LOG_WARN << "business thread pool queue is above 80%: "
                         << queued << "/" << capacity;
            }
            if (!m_threadPool->submitTask(std::forward<Func>(func)))
                LOG_ERROR << "business task rejected because executor is stopping";
        } else {
            func();
        }
    }
    unordered_map<int, MsgHandler> _msgHandlerMap;
    unordered_map<int, TcpConnectionPtr> _userConnMap;
    // 连接到用户的反向索引，业务消息不得信任客户端提交的 id。
    unordered_map<const TcpConnection *, int> _connUserMap;
    mutex _connMutex;
    UserModel _userModel;
    OfflineMsgModel _offlineMsgModel;
    FriendModel _friendModel;
    GroupModel _groupModel;
    Redis _redis;
    pool::TaskExecutor *m_threadPool;
};

#endif
