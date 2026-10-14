#include "json.hpp"
#include "protocol.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <functional>
using namespace std;
using json = nlohmann::json;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>
#include <atomic>

#include "group.hpp"
#include "user.hpp"
#include "public.hpp"

// 记录当前系统登录的用户信息
User g_currentUser;
// 记录当前登录用户的好友列表信息
vector<User> g_currentUserFriendList;
// 记录当前登录用户的群组列表信息
vector<Group> g_currentUserGroupList;

// 控制主菜单页面程序是否运行
bool isMainMenuRunning = false;

// 用于读写线程之间的通信（同步登录/注册结果）
sem_t rwsem;
// 记录登录状态
atomic_bool g_isLoginSuccess{false};

// 接收线程函数
void readTaskHandler(int clientfd);
// 获取系统时间（聊天信息需要添加时间信息）
string getCurrentTime();
// 主聊天页面程序
void mainMenu(int);
// 显示当前登录成功用户的基本信息
void showCurrentUserData();

// 聊天客户端主逻辑：
// main线程用作发送线程，子线程用作接收线程
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << endl;
        exit(-1);
    }

    // 解析通过命令行参数传递的ip和port
    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    // 创建client端的socket
    int clientfd = socket(AF_INET, SOCK_STREAM, 0);
    if (-1 == clientfd)
    {
        cerr << "socket create error" << endl;
        exit(-1);
    }

    // 填写client需要连接的server信息 ip+port
    sockaddr_in server;
    memset(&server, 0, sizeof(sockaddr_in));

    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = inet_addr(ip);

    // client和server进行连接
    if (-1 == connect(clientfd, (sockaddr *)&server, sizeof(sockaddr_in)))
    {
        cerr << "connect server error" << endl;
        close(clientfd);
        exit(-1);
    }

    // 初始化读写线程通信用的信号量
    sem_init(&rwsem, 0, 0);

    // 连接服务器成功，启动接收子线程
    std::thread readTask(readTaskHandler, clientfd);
    readTask.detach();

    // main线程用于接收用户输入，负责发送数据
    for (;;)
    {
        // 显示首页面菜单 登录、注册、退出
        cout << "========================" << endl;
        cout << "1. login" << endl;
        cout << "2. register" << endl;
        cout << "3. quit" << endl;
        cout << "========================" << endl;
        cout << "choice:";

        int choice = 0;
        // ===================== 修复死循环核心逻辑 =====================
        // 读取输入失败（非数字）时，清空错误状态并丢弃缓冲区脏数据
        if (!(cin >> choice))
        {
            cin.clear();          // 清除cin错误状态
            cin.ignore(10000, '\n'); // 忽略直到换行的所有字符
            cerr << "invalid input!" << endl;
            continue;
        }
        // 读取成功后，丢弃多余字符（避免残留换行影响后续getline）
        cin.ignore(10000, '\n');
        // ============================================================

        switch (choice)
        {
        case 1: // login业务
        {
            int id = 0;
            char pwd[50] = {0};
            cout << "userid:";
            
            // ===================== 修复登录输入校验 =====================
            // 校验userid是否为有效数字，失败则直接返回菜单
            if (!(cin >> id))
            {
                cin.clear();
                cin.ignore(10000, '\n');
                cerr << "userid must be a number!" << endl;
                continue;
            }
            cin.ignore(10000, '\n'); // 清理残留换行
            // ============================================================

            cout << "userpassword:";
            cin.getline(pwd, 50);

            // 组装登录JSON数据包
            json js;
            js["msgid"] = LOGIN_MSG;
            js["id"] = id;
            js["password"] = pwd;
            string request = js.dump();

            g_isLoginSuccess = false;

            int len = sendWithLength(clientfd, request);
            if (len == -1)
            {
                cerr << "send login msg error:" << request << endl;
            }

            // 等待子线程处理完登录响应并通知
            sem_wait(&rwsem);

            // 登录成功则进入聊天主菜单
            if (g_isLoginSuccess)
            {
                isMainMenuRunning = true;
                mainMenu(clientfd);
            }
            // 登录失败后，缓冲区已清空，下一次菜单正常显示
        }
        break;

        case 2: // register业务
        {
            char name[50] = {0};
            char pwd[50] = {0};
            cout << "username:";
            cin.getline(name, 50);
            cout << "userpassword:";
            cin.getline(pwd, 50);

            // 组装注册JSON数据包
            json js;
            js["msgid"] = REG_MSG;
            js["name"] = name;
            js["password"] = pwd;
            string request = js.dump();

            int len = sendWithLength(clientfd, request);
            if (len == -1)
            {
                cerr << "send reg msg error:" << request << endl;
            }

            // 等待子线程处理完注册响应通知
            sem_wait(&rwsem);
        }
        break;

        case 3: // quit业务：关闭socket并退出程序
            close(clientfd);
            sem_destroy(&rwsem);
            exit(0);

        default:
            cerr << "invalid input!" << endl;
            break;
        }
    }

    return 0;
}

// 处理注册的响应逻辑
void doRegResponse(json &responsejs)
{
    if (0 != responsejs["errno"].get<int>()) // 注册失败
    {
        cerr << "name is already exist, register error!" << endl;
    }
    else // 注册成功
    {
        cout << "name register success, userid is " << responsejs["id"]
             << ", do not forget it!" << endl;
    }
}

// 处理登录的响应逻辑
void doLoginResponse(int clientfd, json &responsejs)
{
    if (0 != responsejs["errno"].get<int>()) // 登录失败
    {
        cerr << responsejs["errmsg"] << endl;
        g_isLoginSuccess = false;
    }
    else // 登录成功
    {
        // 记录当前用户的id和name
        g_currentUser.setId(responsejs["id"].get<int>());
        g_currentUser.setName(responsejs["name"]);

        // 记录当前用户的好友列表信息
        if (responsejs.contains("friends"))
        {
            g_currentUserFriendList.clear();

            vector<string> vec = responsejs["friends"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                User user;
                user.setId(js["id"].get<int>());
                user.setName(js["name"]);
                user.setState(js["state"]);
                g_currentUserFriendList.push_back(user);
            }
        }

        // 记录当前用户的群组列表信息
        if (responsejs.contains("groups"))
        {
            g_currentUserGroupList.clear();

            vector<string> vec1 = responsejs["groups"];
            for (string &groupstr : vec1)
            {
                json grpjs = json::parse(groupstr);
                Group group;
                group.setId(grpjs["id"].get<int>());
                group.setName(grpjs["groupname"]);
                group.setDesc(grpjs["groupdesc"]);

                vector<string> vec2 = grpjs["users"];
                for (string &userstr : vec2)
                {
                    GroupUser user;
                    json js = json::parse(userstr);
                    user.setId(js["id"].get<int>());
                    user.setName(js["name"]);
                    user.setState(js["state"]);
                    user.setRole(js["role"]);
                    group.getUsers().push_back(user);
                }

                g_currentUserGroupList.push_back(group);
            }
        }

        // 显示登录用户的基本信息
        showCurrentUserData();

        // 显示当前用户的离线消息 个人聊天或群组消息
        if (responsejs.contains("offlinemsg"))
        {
            vector<string> vec = responsejs["offlinemsg"];
            for (string &str : vec)
            {
                json js = json::parse(str);
                if (ONE_CHAT_MSG == js["msgid"].get<int>())
                {
                    cout << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
                         << " said: " << js["msg"].get<string>() << endl;
                }
                else
                {
                    cout << "群消息[" << js["groupid"] << "]:" << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
                         << " said: " << js["msg"].get<string>() << endl;
                }

                // 离线消息只有在客户端成功收到并处理后才 ACK，
                // 服务端据此删除数据库记录。
                if (js.contains("msg_uuid"))
                {
                    json ack;
                    ack["msgid"] = (js["msgid"].get<int>() == ONE_CHAT_MSG)
                                       ? ONE_CHAT_MSG_ACK : GROUP_CHAT_MSG_ACK;
                    ack["id"] = g_currentUser.getId();
                    ack["msg_uuid"] = js["msg_uuid"].get<string>();
                    sendWithLength(clientfd, ack.dump());
                }
            }
        }

        g_isLoginSuccess = true;
    }
}

// 子线程 - 专门处理接收服务器消息（长度前缀帧协议）
void readTaskHandler(int clientfd)
{
    for (;;)
    {
        // 按长度前缀帧协议接收: 4字节头 + 消息体，自动处理粘包/半包
        string msg;
        if (!recvWithLength(clientfd, msg))
        {
            close(clientfd);
            exit(-1);
        }

        // 反序列化JSON
        json js = json::parse(msg);
        int msgtype = js["msgid"].get<int>();

        // 一对一聊天消息
        if (ONE_CHAT_MSG == msgtype)
        {
            cout << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
                 << " said: " << js["msg"].get<string>() << endl;

            // ====== Store-then-Deliver: 发送 ACK 确认 ======
            if (js.contains("msg_uuid"))
            {
                json ack;
                ack["msgid"] = ONE_CHAT_MSG_ACK;
                ack["id"] = g_currentUser.getId();
                ack["msg_uuid"] = js["msg_uuid"].get<string>();
                sendWithLength(clientfd, ack.dump());
            }
            continue;
        }

        // 群聊消息
        if (GROUP_CHAT_MSG == msgtype)
        {
            cout << "群消息[" << js["groupid"] << "]:" << js["time"].get<string>() << " [" << js["id"] << "]" << js["name"].get<string>()
                 << " said: " << js["msg"].get<string>() << endl;

            // ====== Store-then-Deliver: 发送 ACK 确认 ======
            if (js.contains("msg_uuid"))
            {
                json ack;
                ack["msgid"] = GROUP_CHAT_MSG_ACK;
                ack["id"] = g_currentUser.getId();
                ack["msg_uuid"] = js["msg_uuid"].get<string>();
                sendWithLength(clientfd, ack.dump());
            }
            continue;
        }

        // 登录响应
        if (LOGIN_MSG_ACK == msgtype)
        {
            doLoginResponse(clientfd, js);
            sem_post(&rwsem); // 通知主线程登录结果处理完成
            continue;
        }

        // 注册响应
        if (REG_MSG_ACK == msgtype)
        {
            doRegResponse(js);
            sem_post(&rwsem); // 通知主线程注册结果处理完成
            continue;
        }

        // 创建群组响应。服务端已经完成创建并返回 ACK，客户端需要把结果展示给用户。
        if (CREATE_GROUP_MSG_ACK == msgtype)
        {
            if (js.value("errno", 1) == 0)
            {
                cout << "create group success, groupid: "
                     << js.value("groupid", 0) << endl;
            }
            else
            {
                cout << "create group failed: "
                     << js.value("errmsg", "unknown error") << endl;
            }
            continue;
        }
    }
}

// 显示当前登录成功用户的基本信息、好友、群组
void showCurrentUserData()
{
    cout << "======================login user======================" << endl;
    cout << "current login user => id:" << g_currentUser.getId() << " name:" << g_currentUser.getName() << endl;
    cout << "----------------------friend list---------------------" << endl;
    if (!g_currentUserFriendList.empty())
    {
        for (User &user : g_currentUserFriendList)
        {
            cout << user.getId() << " " << user.getName() << " " << user.getState() << endl;
        }
    }
    cout << "----------------------group list----------------------" << endl;
    if (!g_currentUserGroupList.empty())
    {
        for (Group &group : g_currentUserGroupList)
        {
            cout << group.getId() << " " << group.getName() << " " << group.getDesc() << endl;
            for (GroupUser &user : group.getUsers())
            {
                cout << user.getId() << " " << user.getName() << " " << user.getState()
                     << " " << user.getRole() << endl;
            }
        }
    }
    cout << "======================================================" << endl;
}

// 命令处理函数声明
void help(int fd = 0, string str = "");
void chat(int, string);
void addfriend(int, string);
void creategroup(int, string);
void addgroup(int, string);
void groupchat(int, string);
void loginout(int, string);

// 系统支持的客户端命令列表
unordered_map<string, string> commandMap = {
    {"help", "显示所有支持的命令，格式help"},
    {"chat", "一对一聊天，格式chat:friendid:message"},
    {"addfriend", "添加好友，格式addfriend:friendid"},
    {"creategroup", "创建群组，格式creategroup:groupname:groupdesc"},
    {"addgroup", "加入群组，格式addgroup:groupid"},
    {"groupchat", "群聊，格式groupchat:groupid:message"},
    {"loginout", "注销，格式loginout"}};

// 注册系统支持的客户端命令处理函数
unordered_map<string, function<void(int, string)>> commandHandlerMap = {
    {"help", help},
    {"chat", chat},
    {"addfriend", addfriend},
    {"creategroup", creategroup},
    {"addgroup", addgroup},
    {"groupchat", groupchat},
    {"loginout", loginout}};

// 主聊天页面程序
void mainMenu(int clientfd)
{
    help();

    char buffer[1024] = {0};
    while (isMainMenuRunning)
    {
        cin.getline(buffer, 1024);
        string commandbuf(buffer);
        string command;

        int idx = commandbuf.find(":");
        if (-1 == idx)
        {
            command = commandbuf;
        }
        else
        {
            command = commandbuf.substr(0, idx);
        }

        auto it = commandHandlerMap.find(command);
        if (it == commandHandlerMap.end())
        {
            cerr << "invalid input command!" << endl;
            continue;
        }

        // 调用对应命令的处理回调
        it->second(clientfd, commandbuf.substr(idx + 1));
    }
}

// help命令：显示所有支持的命令
void help(int, string)
{
    cout << "show command list >>> " << endl;
    for (auto &p : commandMap)
    {
        cout << p.first << " : " << p.second << endl;
    }
    cout << endl;
}

// addfriend命令：添加好友
void addfriend(int clientfd, string str)
{
    int friendid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_FRIEND_MSG;
    js["id"] = g_currentUser.getId();
    js["friendid"] = friendid;
    string buffer = js.dump();

    int len = sendWithLength(clientfd, buffer);
    if (-1 == len)
    {
        cerr << "send addfriend msg error -> " << buffer << endl;
    }
}

// chat命令：一对一聊天
void chat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx)
    {
        cerr << "chat command invalid!" << endl;
        return;
    }

    int friendid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1);

    json js;
    js["msgid"] = ONE_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["toid"] = friendid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    string buffer = js.dump();

    int len = sendWithLength(clientfd, buffer);
    if (-1 == len)
    {
        cerr << "send chat msg error -> " << buffer << endl;
    }
}

// creategroup命令：创建群组
void creategroup(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx)
    {
        cerr << "creategroup command invalid!" << endl;
        return;
    }

    string groupname = str.substr(0, idx);
    string groupdesc = str.substr(idx + 1);

    json js;
    js["msgid"] = CREATE_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupname"] = groupname;
    js["groupdesc"] = groupdesc;
    string buffer = js.dump();

    int len = sendWithLength(clientfd, buffer);
    if (-1 == len)
    {
        cerr << "send creategroup msg error -> " << buffer << endl;
    }
}

// addgroup命令：加入群组
void addgroup(int clientfd, string str)
{
    int groupid = atoi(str.c_str());
    json js;
    js["msgid"] = ADD_GROUP_MSG;
    js["id"] = g_currentUser.getId();
    js["groupid"] = groupid;
    string buffer = js.dump();

    int len = sendWithLength(clientfd, buffer);
    if (-1 == len)
    {
        cerr << "send addgroup msg error -> " << buffer << endl;
    }
}

// groupchat命令：群聊
void groupchat(int clientfd, string str)
{
    int idx = str.find(":");
    if (-1 == idx)
    {
        cerr << "groupchat command invalid!" << endl;
        return;
    }

    int groupid = atoi(str.substr(0, idx).c_str());
    string message = str.substr(idx + 1);

    json js;
    js["msgid"] = GROUP_CHAT_MSG;
    js["id"] = g_currentUser.getId();
    js["name"] = g_currentUser.getName();
    js["groupid"] = groupid;
    js["msg"] = message;
    js["time"] = getCurrentTime();
    string buffer = js.dump();

    int len = sendWithLength(clientfd, buffer);
    if (-1 == len)
    {
        cerr << "send groupchat msg error -> " << buffer << endl;
    }
}

// loginout命令：注销退出
void loginout(int clientfd, string)
{
    json js;
    js["msgid"] = LOGINOUT_MSG;
    js["id"] = g_currentUser.getId();
    string buffer = js.dump();

    int len = sendWithLength(clientfd, buffer);
    if (-1 == len)
    {
        cerr << "send loginout msg error -> " << buffer << endl;
    }
    else
    {
        isMainMenuRunning = false;
    }
}

// 获取系统当前时间
string getCurrentTime()
{
    auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm *ptm = localtime(&tt);
    char date[60] = {0};
    sprintf(date, "%d-%02d-%02d %02d:%02d:%02d",
            (int)ptm->tm_year + 1900, (int)ptm->tm_mon + 1, (int)ptm->tm_mday,
            (int)ptm->tm_hour, (int)ptm->tm_min, (int)ptm->tm_sec);
    return std::string(date);
}
