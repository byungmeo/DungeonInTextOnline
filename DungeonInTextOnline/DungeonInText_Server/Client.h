#pragma once
#include "ServerCore.h"
#include <atomic>
#include <hiredis/hiredis.h>
#include <memory>
#include <mutex>
#include <WinSock2.h>

using namespace std;

class Player;

static const int USER_EXPIRE_TIME = 300; // ���� ������ 5�� �ڿ� ����

class Client {
public:
    SOCKET sock;  // �� Ŭ���̾�Ʈ�� active socket
    mutex socketMutex;
    redisContext* c;

    shared_ptr<Player> playerInfo;
    mutex playerInfoMutex;

    atomic<bool> doingRecv;

    bool lenCompleted;
    int packetLen;
    char packet[BUFFER_SIZE];
    int offset;

    bool isREST; // REST API�� ���� ������ Ŭ���̾�Ʈ���� ���θ� ����

    Client(SOCKET sock, redisContext* context);

    ~Client();
};