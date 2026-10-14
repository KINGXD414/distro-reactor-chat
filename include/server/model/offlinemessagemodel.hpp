#ifndef OFFLINEMESSAGEMODEL_H
#define OFFLINEMESSAGEMODEL_H

#include <string>
#include <vector>
using namespace std;

// 提供离线消息表的操作接口方法
class OfflineMsgModel
{
public:
    // 存储用户的离线信息
    bool insert(int userid, const string &msg);

    // 删除用户全部离线消息（登录时批量拉取后调用）
    void remove(int userid);

    // 按 msg_uuid 删除单条消息（客户端 ACK 后调用）
    void removeByMsgUuid(int userid, const string &msg_uuid);

    // 查询用户的离线消息
    vector<string> query(int userid);
};

#endif
