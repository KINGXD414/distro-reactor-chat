#ifndef PUBLIC_H
#define PUBLIC_H

#include <string>
#include <chrono>
#include <random>

enum EnMsgType
{
    LOGIN_MSG = 1,
    LOGIN_MSG_ACK,
    LOGINOUT_MSG,
    REG_MSG,
    REG_MSG_ACK,
    ONE_CHAT_MSG,
    ADD_FRIEND_MSG,
    ADD_FRIEND_MSG_ACK,
    CREATE_GROUP_MSG,
    ADD_GROUP_MSG,
    ADD_GROUP_MSG_ACK,
    GROUP_CHAT_MSG,
    CREATE_GROUP_MSG_ACK,

    // ====== Store-then-Deliver: 客户端消息确认 ======
    ONE_CHAT_MSG_ACK   = 20,  // 一对一聊天确认
    GROUP_CHAT_MSG_ACK = 21   // 群聊消息确认
};

// 生成全局唯一的消息ID
// 格式: sender_timestampNs_random4
inline std::string generateMsgUuid(int senderId)
{
    auto now = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    // 低4位随机数防同一纳秒内冲突
    static thread_local std::mt19937_64 rng(std::random_device{}());
    int r = rng() % 10000;
    return std::to_string(senderId) + "_" + std::to_string(ns) + "_"
           + std::to_string(r);
}

#endif
