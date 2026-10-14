-- Existing-schema performance indexes for login and message/group queries.
-- Run once against the chat database after checking that the index names do not exist.
USE chat;

ALTER TABLE friend
    ADD INDEX idx_friend_userid (userid),
    ADD INDEX idx_friend_friendid (friendid);

ALTER TABLE groupuser
    ADD INDEX idx_groupuser_user_group (userid, groupid),
    ADD INDEX idx_groupuser_group_user (groupid, userid);

ALTER TABLE offlinemessage
    ADD INDEX idx_offlinemessage_userid (userid);
