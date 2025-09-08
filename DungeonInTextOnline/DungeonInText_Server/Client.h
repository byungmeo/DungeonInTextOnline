#pragma once
#include "ServerCore.h"
#include <atomic>
#include <hiredis/hiredis.h>
#include <memory>
#include <mutex>
#include <WinSock2.h>

using namespace std;

class Player;

static const int USER_EXPIRE_TIME = 300; // 유저 정보는 5분 뒤에 만료

class Client {
public:
    SOCKET sock;  // 이 클라이언트의 active socket
    mutex socketMutex;
    redisContext* c;

    shared_ptr<Player> playerInfo;
    mutex playerInfoMutex;

    atomic<bool> doingRecv;

    bool lenCompleted;
    int packetLen;
    char packet[BUFFER_SIZE];
    int offset;

    bool isREST; // REST API를 통해 접속한 클라이언트인지 여부를 저장

    Client(SOCKET sock, redisContext* context);

    ~Client();
};