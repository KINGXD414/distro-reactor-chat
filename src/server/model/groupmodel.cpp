#include "groupmodel.hpp"
#include "db.h"
#include <muduo/base/Logging.h>
#include <unordered_map>

// group表: id(int,PRI,auto_increment) groupname(varchar(50)) groupdesc(varchar(200))
// groupuser表: id(int,PRI,auto_increment) groupid(int) userid(int) grouprole(varchar(20),default "member")

bool GroupModel::creatGroup(Group &group)
{
    MySQL mysql;
    if (!mysql.connect()) return false;

    char sql[1024] = {0};
    sprintf(sql, "insert into `group`(groupname, groupdesc) values('%s', '%s')",
            mysql.escape(group.getName()).c_str(),
            mysql.escape(group.getDesc()).c_str());

    if (mysql.update(sql))
    {
        // 获取自增的群ID
        group.setId(mysql_insert_id(mysql.getConnection()));
        return true;
    }
    return false;
}

void GroupModel::addGroup(int userid, int groupid, string role)
{
    MySQL mysql;
    if (!mysql.connect()) return;

    char sql[1024] = {0};
    sprintf(sql, "insert into groupuser(groupid, userid, grouprole) values(%d, %d, '%s')",
            groupid, userid, mysql.escape(role).c_str());

    mysql.update(sql);
}

// ★ 查询用户所在群组，必须包含群组成员列表（含role）
// 客户端doLoginResponse期望每个group有 "users" 字段，每个user有 id/name/state/role
vector<Group> GroupModel::queryGroups(int userid)
{
    vector<Group> groupVec;
    MySQL mysql;
    if (!mysql.connect()) return groupVec;

    // 一次查询拿到群组和成员，避免登录时每个群组再发一条 SQL（N+1）。
    // mine 连接保证结果只包含当前用户所在的群组，gu/u 为成员明细。
    char sql[2048] = {0};
    sprintf(sql,
        "select g.id, g.groupname, g.groupdesc, u.id, u.name, u.state, gu.grouprole "
        "from `group` g "
        "inner join groupuser mine on mine.groupid = g.id and mine.userid = %d "
        "left join groupuser gu on gu.groupid = g.id "
        "left join user u on u.id = gu.userid "
        "order by g.id, u.id",
        userid);

    MYSQL_RES *res = mysql.query(sql);
    if (res != nullptr)
    {
        unordered_map<int, size_t> groupIndex;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr)
        {
            int groupid = atoi(row[0]);
            auto it = groupIndex.find(groupid);
            if (it == groupIndex.end())
            {
                Group group;
                group.setId(groupid);
                group.setName(row[1] == nullptr ? "" : row[1]);
                group.setDesc(row[2] == nullptr ? "" : row[2]);
                groupVec.push_back(group);
                groupIndex[groupid] = groupVec.size() - 1;
                it = groupIndex.find(groupid);
            }

            // LEFT JOIN 下无成员时后三列为空，不能把空行转成用户 0。
            if (row[3] != nullptr)
            {
                GroupUser user;
                user.setId(atoi(row[3]));
                user.setName(row[4] == nullptr ? "" : row[4]);
                user.setState(row[5] != nullptr && atoi(row[5]) == 1 ? "online" : "offline");
                user.setRole(row[6] == nullptr ? "member" : row[6]);
                groupVec[it->second].getUsers().push_back(user);
            }
        }
        mysql_free_result(res);
    }

    return groupVec;
}

vector<int> GroupModel::queryGroupUsers(int userid, int groupid)
{
    char sql[1024] = {0};
    // 只有群成员才能向群内广播；此前仅排除发送者，未校验其群成员身份。
    sprintf(sql,
            "select userid from groupuser where groupid = %d and userid != %d "
            "and exists (select 1 from groupuser where groupid = %d and userid = %d)",
            groupid, userid, groupid, userid);

    vector<int> vec;
    MySQL mysql;
    if (mysql.connect())
    {
        MYSQL_RES *res = mysql.query(sql);
        if (res != nullptr)
        {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                vec.push_back(atoi(row[0]));
            }
            mysql_free_result(res);
        }
    }
    return vec;
}

