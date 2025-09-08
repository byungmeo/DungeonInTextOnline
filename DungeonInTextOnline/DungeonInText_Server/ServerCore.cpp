#include "Client.h"
#include "ServerCore.h"
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <WinSock2.h>

static unsigned short SERVER_PORT = 27015;

SOCKET CreatePassiveSocket() {
    // TCP socket 을 만든다.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // socket 을 특정 주소, 포트에 바인딩 한다.
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int r = bind(passiveSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        std::cerr << "bind failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // TCP 는 연결을 받는 passive socket 과 실제 통신을 할 수 있는 active socket 으로 구분된다.
    // passive socket 은 socket() 뒤에 listen() 을 호출함으로써 만들어진다.
    // active socket 은 passive socket 을 이용해 accept() 를 호출함으로써 만들어진다.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        std::cerr << "listen faijled with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    return passiveSock;
}

bool SendMsg(shared_ptr<Client> client, string message)
{
    // 만약 메시지를 보내려는 대상이 REST Client인 경우 보내지 않습니다.
    if (client->isREST) return true;

    SOCKET activeSock = client->sock;
    int r;

    string data = message;
    int dataLen = data.length() + 1; // 문자열의 끝을 의미하는 NULL 문자 포함

    // 길이를 먼저 보낸다.
    // binary 로 4bytes 를 길이로 encoding 한다.
    // 이 때 network byte order 로 변환하기 위해서 htonl 을 호출해야된다.
    int dataLenNetByteOrder = htonl(dataLen);
    int offset = 0;
    while (offset < 4) {
        {
            unique_lock<mutex> ul(client->socketMutex);
            r = send(activeSock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
        }
        if (r == SOCKET_ERROR) {
            std::cerr << "failed to send length: " << WSAGetLastError() << std::endl;
            return false;
        }
        offset += r;
    }
    //std::cout << "Sent length info: " << dataLen << std::endl;

    // send 로 명령어를 보낸다.
    offset = 0;
    while (offset < dataLen) {
        {
            unique_lock<mutex> ul(client->socketMutex);
            r = send(activeSock, data.c_str() + offset, dataLen - offset, 0);
        }
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return false;
        }
        //std::cout << "Sent " << r << " bytes" << std::endl;
        offset += r;
    }

    return true;
}

bool RecvLength(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;
    int r;

    // 이전에 어디까지 작업했는지에 따라 다르게 처리한다.
    // 이전에 packetLen 을 완성하지 못했다. 그걸 완성하게 한다.
    if (client->lenCompleted == false) {
        // 길이 정보를 받기 위해서 4바이트를 읽는다.
        // network byte order 로 전성되기 때문에 ntohl() 을 호출한다.
        {
            unique_lock<mutex> ul(client->socketMutex);
            r = recv(activeSock, (char*)&(client->packetLen) + client->offset, 4 - client->offset, 0);
        }
        if (r == SOCKET_ERROR) {
            std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
            return false;
        }
        else if (r == 0) {
            // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
            // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
            std::cerr << "Socket closed: " << activeSock << std::endl;
            return false;
        }
        client->offset += r;

        // 완성 못했다면 다음번에 계속 시도할 것이다.
        if (client->offset < 4) {
            return true;
        }

        // network byte order 로 전송했었다.
        // 따라서 이를 host byte order 로 변경한다.
        int dataLen = ntohl(client->packetLen);
        //std::cout << "[" << activeSock << "] Received length info: " << dataLen << std::endl;
        client->packetLen = dataLen;

        // 우리는 Client class 안에 packet 길이를 최대 64KB 로 제한하고 있다.
        // 혹시 우리가 받을 데이터가 이것보다 큰지 확인한다.
        // 만일 크다면 처리 불가능이므로 오류로 처리한다.
        if (client->packetLen > sizeof(client->packet)) {
            std::cerr << "[" << activeSock << "] Too big data: " << client->packetLen << std::endl;
            return false;
        }

        // 이제 packetLen 을 완성했다고 기록하고 offset 을 초기화해준다.
        client->lenCompleted = true;
        client->offset = 0;
    }

    return true;
}

bool RecvPacket(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;
    int r;

    {
        unique_lock<mutex> ul(client->socketMutex);
        r = recv(activeSock, client->packet + client->offset, client->packetLen - client->offset, 0);
    }
    if (r == SOCKET_ERROR) {
        std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
        return false;
    }
    else if (r == 0) {
        // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
        // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
        return false;
    }
    client->offset += r;

    // 완성한 경우와 partial recv 인 경우를 구분해서 로그를 찍는다.
    if (client->offset == client->packetLen) {
        //std::cout << "[" << activeSock << "] Received " << client->packetLen << " bytes" << std::endl;

        // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
        client->lenCompleted = false;
        client->offset = 0;
        client->packetLen = 0;
    }
    else {
        //std::cout << "[" << activeSock << "] Partial recv " << r << "bytes. " << client->offset << "/" << client->packetLen << std::endl;
    }

    return true;
}