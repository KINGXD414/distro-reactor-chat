#include "offlinemessagemodel.hpp"
#include "db.h"
#include <string>

// offlinemessage表: userid(int) message(varchar(2000))
// Store-then-Deliver: 每条消息携带 msg_uuid，支持精确删除

bool OfflineMsgModel::insert(int userid, const string &msg)
{
    MySQL mysql;
    if (!mysql.connect()) return false;

    // 使用 mysql_real_escape_string 做专业 SQL 转义
    // message 字段扩至 2000 字符以适应带 msg_uuid 的 JSON
    char sql[2048] = {0};
    sprintf(sql, "insert into offlinemessage(userid, message) values(%d, '%s')",
            userid, mysql.escape(msg).c_str());

    return mysql.update(sql);
}

void OfflineMsgModel::remove(int userid)
{
    char sql[1024] = {0};
    sprintf(sql, "delete from offlinemessage where userid = %d", userid);

    MySQL mysql;
    if (mysql.connect())
    {
        mysql.update(sql);
    }
}

// Store-then-Deliver: 客户端 ACK 后精确删除单条消息
void OfflineMsgModel::removeByMsgUuid(int userid, const string &msg_uuid)
{
    MySQL mysql;
    if (!mysql.connect()) return;

    // LIKE 匹配 JSON 中的 "msg_uuid":"xxx" 字段
    // 安全转义 msg_uuid 防止 SQL 注入
    char sql[2048] = {0};
    string pattern = "%\"msg_uuid\":\"" + msg_uuid + "\"%";
    sprintf(sql, "delete from offlinemessage where userid = %d and message like '%s' limit 1",
            userid, mysql.escape(pattern).c_str());

    mysql.update(sql);
}

vector<string> OfflineMsgModel::query(int userid)
{
    char sql[1024] = {0};
    sprintf(sql, "select message from offlinemessage where userid = %d", userid);

    vector<string> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                vec.push_back(row[0]);
            }
            mysql_free_result(res);
        }
    }
    return vec;
}
