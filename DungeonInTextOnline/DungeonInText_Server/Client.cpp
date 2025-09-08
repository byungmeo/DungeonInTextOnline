#include "Client.h"
#include "Player.h"
#include <iostream>
#include <mutex>
#include <WinSock2.h>

Client::Client(SOCKET sock, redisContext* context) 
	: sock(sock), c(context), playerInfo(new Player()), doingRecv(false), lenCompleted(false), packetLen(0), offset(0), isREST(false)
{
}

Client::~Client()
{
    // ���� ���� ���� ���� ����
    {
        unique_lock<mutex> ul(playerInfoMutex);

        redisReply* reply;
        reply = (redisReply*)redisCommand(c, "EXPIRE USER:%s:socket %d", playerInfo->name.c_str(), USER_EXPIRE_TIME);

        // ���� ���� �� ���� ������ �����س����ϴ�.
        reply = (redisReply*)redisCommand(c, "SET USER:%s:hp %d", playerInfo->name.c_str(), playerInfo->hp);
        reply = (redisReply*)redisCommand(c, "SET USER:%s:x %d", playerInfo->name.c_str(), playerInfo->x);
        reply = (redisReply*)redisCommand(c, "SET USER:%s:y %d", playerInfo->name.c_str(), playerInfo->y);
        reply = (redisReply*)redisCommand(c, "SET USER:%s:str %d", playerInfo->name.c_str(), playerInfo->str);
        reply = (redisReply*)redisCommand(c, "HSET USER:%s:inventory hp %d", playerInfo->name.c_str(), playerInfo->inventory["hp"]);
        reply = (redisReply*)redisCommand(c, "HSET USER:%s:inventory str %d", playerInfo->name.c_str(), playerInfo->inventory["str"]);
        freeReplyObject(reply);
    }

    std::cout << "Client destroyed. Socket: " << sock << std::endl;
}
