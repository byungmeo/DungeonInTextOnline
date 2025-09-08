#include "WebCore.h"
#include <cstring>
#include <inaddr.h>
#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>

// REST API TCP ���� ����
static const char* REST_SERVER_ADDRESS = "127.0.0.1";
static const unsigned short REST_SERVER_PORT = 27016;

SOCKET CreatePassiveSocketREST()
{
    // REST API ��ſ� TCP socket �� �����.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // socket �� Ư�� �ּ�, ��Ʈ�� ���ε� �Ѵ�.
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(REST_SERVER_PORT);
    inet_pton(AF_INET, REST_SERVER_ADDRESS, &serverAddr.sin_addr.s_addr);

    int r = ::bind(passiveSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        std::cerr << "bind failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // passive socket�� �����ϰ� ��ȯ�Ѵ�.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        std::cerr << "listen faijled with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    return passiveSock;
}