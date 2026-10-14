#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <vector>
using namespace std;
using namespace muduo;
using namespace muduo::net;

ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

ChatService::ChatService()
{
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::login, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::loginout, this, _1, _2, _3)});
    _msgHandlerMap.insert({REG_MSG, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriend, this, _1, _2, _3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});

    // ====== Store-then-Deliver: ACK 消息处理 ======
    _msgHandlerMap.insert({ONE_CHAT_MSG_ACK, std::bind(&ChatService::oneChatAck, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG_ACK, std::bind(&ChatService::groupChatAck, this, _1, _2, _3)});

    if (_redis.connect())
    {
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
    }

    m_threadPool = nullptr;
}

MsgHandler ChatService::getHandler(int msgid)
{
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end())
    {
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp) {
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
        };
    }
    return _msgHandlerMap[msgid];
}

// ======================== 登录 (Redis缓存优化版) ========================
void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id = js["id"].get<int>();
    string pwd = js["password"].get<string>();

    dispatch([this, conn, id, pwd]() {
        // 请求可能已经在业务队列中等待，而客户端先断开；不要为已关闭连接建立在线会话。
        if (!conn->connected()) return;

        // ====== 1. 查用户（缓存旁路） ======
        User user;
        string userKey = "user:" + to_string(id);
        string cachedUser = _redis.cacheGet(userKey);
        if (!cachedUser.empty())
        {
            json uj = json::parse(cachedUser);
            user.setId(uj["id"].get<int>());
            user.setName(uj["name"].get<string>());
            user.setPwd(uj["pwd"].get<string>());
            user.setState(uj["state"].get<string>());
        }
        else
        {
            user = _userModel.query(id);
        }

        json res;
        res["msgid"] = LOGIN_MSG_ACK;

        if (user.getId() != -1 && user.getPwd() == pwd)
        {
            {
                lock_guard<mutex> lock(_connMutex);
                if (!conn->connected()) return;
                if (_userConnMap.find(id) != _userConnMap.end())
                {
                    res["errno"] = 2;
                    res["errmsg"] = "该账号已经登录，不允许重复登录";
                    conn->getLoop()->runInLoop([conn, resp = res.dump()]() {
                        if (conn->connected()) sendWithLength(conn, resp);
                    });
                    return;
                }
                _userConnMap[id] = conn;
                _connUserMap[conn.get()] = id;
            }

            _redis.subscribe(id);

            User u;
            u.setId(id);
            u.setState("online");
            _userModel.updateState(u);

            // 更新用户缓存（状态→online）
            json cacheUser;
            cacheUser["id"] = user.getId();
            cacheUser["name"] = user.getName();
            cacheUser["pwd"] = user.getPwd();
            cacheUser["state"] = "online";
            _redis.cacheSet(userKey, cacheUser.dump());

            res["errno"] = 0;
            res["id"] = user.getId();
            res["name"] = user.getName();

            vector<string> offlineMsgs = _offlineMsgModel.query(id);
            if (!offlineMsgs.empty())
            {
                res["offlinemsg"] = offlineMsgs;
                // 不在登录响应发送前删除。客户端逐条 ACK 后才删除，
                // 这样连接在响应发送失败时仍能在下次登录重试。
            }
            else
            {
                res["offlinemsg"] = json::array();
            }

            // ====== 2. 查好友列表（缓存旁路） ======
            vector<User> friends;
            string friendsKey = "friends:" + to_string(id);
            string cachedFriends = _redis.cacheGet(friendsKey);
            if (!cachedFriends.empty())
            {
                json fja = json::parse(cachedFriends);
                for (auto &fj : fja)
                {
                    User f;
                    f.setId(fj["id"].get<int>());
                    f.setName(fj["name"].get<string>());
                    f.setState(fj["state"].get<string>());
                    friends.push_back(f);
                }
            }
            else
            {
                friends = _friendModel.query(id);
                // 写入缓存
                json fja = json::array();
                for (User &f : friends)
                {
                    json fj;
                    fj["id"] = f.getId();
                    fj["name"] = f.getName();
                    fj["state"] = f.getState();
                    fja.push_back(fj);
                }
                _redis.cacheSet(friendsKey, fja.dump());
            }

            if (!friends.empty())
            {
                vector<string> vec2;
                for (User &f : friends)
                {
                    json ft;
                    ft["id"] = f.getId();
                    ft["name"] = f.getName();
                    ft["state"] = f.getState();
                    vec2.push_back(ft.dump());
                }
                res["friends"] = vec2;
            }
            else
            {
                res["friends"] = json::array();
            }

            // ====== 3. 查群组列表（缓存旁路） ======
            vector<Group> groups;
            string groupsKey = "groups:" + to_string(id);
            string cachedGroups = _redis.cacheGet(groupsKey);
            if (!cachedGroups.empty())
            {
                json gja = json::parse(cachedGroups);
                for (auto &gj : gja)
                {
                    Group g;
                    g.setId(gj["id"].get<int>());
                    g.setName(gj["groupname"].get<string>());
                    g.setDesc(gj["groupdesc"].get<string>());
                    for (auto &uj : gj["users"])
                    {
                        GroupUser gu;
                        gu.setId(uj["id"].get<int>());
                        gu.setName(uj["name"].get<string>());
                        gu.setState(uj["state"].get<string>());
                        gu.setRole(uj["role"].get<string>());
                        g.getUsers().push_back(gu);
                    }
                    groups.push_back(g);
                }
            }
            else
            {
                groups = _groupModel.queryGroups(id);
                // 写入缓存
                json gja = json::array();
                for (Group &g : groups)
                {
                    json gj;
                    gj["id"] = g.getId();
                    gj["groupname"] = g.getName();
                    gj["groupdesc"] = g.getDesc();
                    json usersArr = json::array();
                    for (GroupUser &gu : g.getUsers())
                    {
                        json uj;
                        uj["id"] = gu.getId();
                        uj["name"] = gu.getName();
                        uj["state"] = gu.getState();
                        uj["role"] = gu.getRole();
                        usersArr.push_back(uj);
                    }
                    gj["users"] = usersArr;
                    gja.push_back(gj);
                }
                _redis.cacheSet(groupsKey, gja.dump());
            }

            if (!groups.empty())
            {
                vector<string> vec2;
                for (Group &g : groups)
                {
                    json grpjs;
                    grpjs["id"] = g.getId();
                    grpjs["groupname"] = g.getName();
                    grpjs["groupdesc"] = g.getDesc();

                    vector<string> userVec;
                    for (GroupUser &gu : g.getUsers())
                    {
                        json userjs;
                        userjs["id"] = gu.getId();
                        userjs["name"] = gu.getName();
                        userjs["state"] = gu.getState();
                        userjs["role"] = gu.getRole();
                        userVec.push_back(userjs.dump());
                    }
                    grpjs["users"] = userVec;
                    vec2.push_back(grpjs.dump());
                }
                res["groups"] = vec2;
            }
            else
            {
                res["groups"] = json::array();
            }

            conn->getLoop()->runInLoop([conn, resp = res.dump()]() {
                if (conn->connected()) sendWithLength(conn, resp);
            });
        }
        else
        {
            res["errno"] = 1;
            res["errmsg"] = "用户名或者密码错误";
            conn->getLoop()->runInLoop([conn, resp = res.dump()]() {
                if (conn->connected()) sendWithLength(conn, resp);
            });
        }
    });
}

// ======================== 注册 ========================
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["name"].get<string>();
    string pwd = js["password"].get<string>();

    dispatch([this, conn, name, pwd]() {
        if (_userModel.isUserExist(name))
        {
            json response;
            response["msgid"] = REG_MSG_ACK;
            response["errno"] = 1;
            response["errmsg"] = "name is already exist, register error!";
            conn->getLoop()->runInLoop([conn, resp = response.dump()]() {
                if (conn->connected()) sendWithLength(conn, resp);
            });
            return;
        }

        User user;
        user.setName(name);
        user.setPwd(pwd);
        if (_userModel.insert(user))
        {
            json response;
            response["msgid"] = REG_MSG_ACK;
            response["errno"] = 0;
            response["id"] = user.getId();
            conn->getLoop()->runInLoop([conn, resp = response.dump()]() {
                if (conn->connected()) sendWithLength(conn, resp);
            });
        }
        else
        {
            json response;
            response["msgid"] = REG_MSG_ACK;
            response["errno"] = 1;
            conn->getLoop()->runInLoop([conn, resp = response.dump()]() {
                if (conn->connected()) sendWithLength(conn, resp);
            });
        }
    });
}

// ======================== 注销 ========================
void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    if (userid == -1 || (js.contains("id") && js["id"].get<int>() != userid))
    {
        LOG_WARN << "loginout rejected: unauthenticated or mismatched connection";
        return;
    }

    {
        lock_guard<mutex> lock(_connMutex);
        _userConnMap.erase(userid);
        _connUserMap.erase(conn.get());
    }
    // 注销请求在 IO 线程到达，状态落库和 Redis 操作放入业务线程池。
    dispatch([this, userid]() {
        lock_guard<mutex> lock(_connMutex);
        if (_userConnMap.find(userid) != _userConnMap.end()) return;
        _redis.unsubscribe(userid);
        User user(userid, "", "", "offline");
        _userModel.updateState(user);

        // 缓存失效：只更新用户状态 → offline（好友/群组不需要删，断线不会改变关系）
        string userKey = "user:" + to_string(userid);
        string cached = _redis.cacheGet(userKey);
        if (!cached.empty())
        {
            json uj = json::parse(cached);
            uj["state"] = "offline";
            _redis.cacheSet(userKey, uj.dump());
        }
    });
}

// ======================== 客户端异常退出 ========================
void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    int userid = -1;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn)
            {
                userid = it->first;
                _userConnMap.erase(it);
                _connUserMap.erase(conn.get());
                break;
            }
        }
    }
    if (userid != -1)
    {
        // 断线回调运行在 IO 线程，状态落库和 Redis 操作放入业务线程池。
        dispatch([this, userid]() {
            lock_guard<mutex> lock(_connMutex);
            if (_userConnMap.find(userid) != _userConnMap.end()) return;
            _redis.unsubscribe(userid);
            User user(userid, "", "", "offline");
            _userModel.updateState(user);

            // 缓存失效：只更新用户状态 → offline
            string userKey = "user:" + to_string(userid);
            string cached = _redis.cacheGet(userKey);
            if (!cached.empty())
            {
                json uj = json::parse(cached);
                uj["state"] = "offline";
                _redis.cacheSet(userKey, uj.dump());
            }
        });
    }
}

void ChatService::reset()
{
    _userModel.resetState();
}

// ======================== 一对一聊天 (Store-then-Deliver) ========================
// 核心: 先落库 → 再尝试实时投递。即使实时投递失败，下次登录仍能拉取。
void ChatService::oneChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int toid = js["toid"].get<int>();
    int fromid = authenticatedUserId(conn);
    if (fromid == -1 || (js.contains("id") && js["id"].get<int>() != fromid))
    {
        LOG_WARN << "oneChat rejected: unauthenticated or mismatched connection";
        return;
    }

    // 生成消息唯一ID并注入到 JSON 中
    string msg_uuid = generateMsgUuid(fromid);
    js["msg_uuid"] = msg_uuid;
    string msg_str = js.dump();

    dispatch([this, toid, msg_str]() {
        // ====== Step 1: 先落库 (Store) ======
        // 无论用户在线与否，先持久化。用户登录时批量拉取清理。
        if (!_offlineMsgModel.insert(toid, msg_str))
        {
            LOG_ERROR << "oneChat persistence failed for recipient " << toid
                      << ", message " << msg_str;
            return;
        }

        // ====== Step 2: 尝试实时投递 (Deliver) ======
        {
            lock_guard<mutex> lock(_connMutex);
            auto it = _userConnMap.find(toid);
            if (it != _userConnMap.end())
            {
                // 目标用户在本机在线 → 直接发送
                TcpConnectionPtr toConn = it->second;
                toConn->getLoop()->runInLoop([toConn, msg_str]() {
                    if (toConn->connected()) sendWithLength(toConn, msg_str);
                });
                return;
            }
        }

        // 目标不在本机 → 通过 Redis 发布到集群
        User user = _userModel.query(toid);
        if (user.getState() == "online")
        {
            _redis.publish(toid, msg_str);
        }
        // 用户离线：消息已在 DB 中，下次登录自动拉取，无需额外操作
    });
}

// ======================== 添加好友 ========================
void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    int friendid = js["friendid"].get<int>();
    if (userid == -1) return;

    dispatch([this, userid, friendid]() {
        _friendModel.insert(userid, friendid);

        // 失效双方的好友缓存，下次登录时重建
        _redis.cacheDel("friends:" + to_string(userid));
        _redis.cacheDel("friends:" + to_string(friendid));
    });
}

// ======================== 创建群组 ========================
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    string name = js["groupname"].get<string>();
    string desc = js["groupdesc"].get<string>();
    if (userid == -1) return;

    dispatch([this, conn, userid, name, desc]() {
        Group group(-1, name, desc);
        json res;
        res["msgid"] = CREATE_GROUP_MSG_ACK;

        if (_groupModel.creatGroup(group))
        {
            _groupModel.addGroup(userid, group.getId(), "creator");
            res["errno"] = 0;
            res["groupid"] = group.getId();
            res["errmsg"] = "创建群组成功";

            // 失效创建者的群组缓存
            _redis.cacheDel("groups:" + to_string(userid));
        }
        else
        {
            res["errno"] = 1;
            res["errmsg"] = "创建群组失败";
        }

        conn->getLoop()->runInLoop([conn, resp = res.dump()]() {
            if (conn->connected()) sendWithLength(conn, resp);
        });
    });
}

// ======================== 加入群组 ========================
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    int groupid = js["groupid"].get<int>();
    if (userid == -1) return;

    dispatch([this, userid, groupid]() {
        _groupModel.addGroup(userid, groupid, "member");

        // 失效该用户的群组缓存
        _redis.cacheDel("groups:" + to_string(userid));
    });
}

// ======================== 群组聊天 (Store-then-Deliver) ========================
// 核心: 对每个群成员先落库 → 再尝试实时投递
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    int groupid = js["groupid"].get<int>();
    if (userid == -1 || (js.contains("id") && js["id"].get<int>() != userid))
    {
        LOG_WARN << "groupChat rejected: unauthenticated or mismatched connection";
        return;
    }

    // 生成消息唯一ID并注入到 JSON 中
    string msg_uuid = generateMsgUuid(userid);
    js["msg_uuid"] = msg_uuid;
    string msg_str = js.dump();

    dispatch([this, userid, groupid, msg_str]() {
        vector<int> useridVec = _groupModel.queryGroupUsers(userid, groupid);

        // ====== Step 1: 对所有群成员先落库 (Store) ======
        for (int uid : useridVec)
        {
            if (!_offlineMsgModel.insert(uid, msg_str))
            {
                LOG_ERROR << "groupChat persistence failed for recipient " << uid
                          << ", group " << groupid;
                return;
            }
        }

        // ====== Step 2: 尝试实时投递 (Deliver) ======
        vector<TcpConnectionPtr> localConns;
        vector<int> notLocalIds;

        {
            lock_guard<mutex> lock(_connMutex);
            for (int uid : useridVec)
            {
                auto it = _userConnMap.find(uid);
                if (it != _userConnMap.end())
                    localConns.push_back(it->second);
                else
                    notLocalIds.push_back(uid);
            }
        }

        // 本机在线 → 直接发送
        for (auto &toConn : localConns)
        {
            toConn->getLoop()->runInLoop([toConn, msg_str]() {
                if (toConn->connected()) sendWithLength(toConn, msg_str);
            });
        }

        // 不在本机 → Redis publish 或已离线（消息已在DB中）
        for (int uid : notLocalIds)
        {
            User user = _userModel.query(uid);
            if (user.getState() == "online")
                _redis.publish(uid, msg_str);
            // 离线用户：消息已在 DB 中，无需额外操作
        }
    });
}

// ======================== Redis订阅消息回调 ========================
void ChatService::handleRedisSubscribeMessage(int userid, string msg)
{
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end())
    {
        TcpConnectionPtr conn = it->second;
        conn->getLoop()->runInLoop([conn, msg]() {
            if (conn->connected()) sendWithLength(conn, msg);
        });
        return;
    }
    if (!_offlineMsgModel.insert(userid, msg))
    {
        LOG_ERROR << "offline message persistence failed for recipient " << userid;
    }
}

// ======================== Store-then-Deliver: 一对一聊天 ACK ========================
// 客户端收到消息后回执，服务端从离线表删除该消息（避免下次登录重复推送）
void ChatService::oneChatAck(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    string msg_uuid = js["msg_uuid"].get<string>();
    if (userid == -1) return;

    dispatch([this, userid, msg_uuid]() {
        _offlineMsgModel.removeByMsgUuid(userid, msg_uuid);
    });
}

// ======================== Store-then-Deliver: 群聊消息 ACK ========================
void ChatService::groupChatAck(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = authenticatedUserId(conn);
    string msg_uuid = js["msg_uuid"].get<string>();
    if (userid == -1) return;

    dispatch([this, userid, msg_uuid]() {
        _offlineMsgModel.removeByMsgUuid(userid, msg_uuid);
    });
}

void ChatService::setThreadPool(pool::TaskExecutor *pool)
{
    m_threadPool = pool;
}

int ChatService::authenticatedUserId(const TcpConnectionPtr &conn)
{
    lock_guard<mutex> lock(_connMutex);
    auto it = _connUserMap.find(conn.get());
    return it == _connUserMap.end() ? -1 : it->second;
}
