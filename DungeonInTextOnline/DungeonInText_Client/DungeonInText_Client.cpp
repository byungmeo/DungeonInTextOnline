#include <chrono>
#include <hiredis/hiredis.h>
#include <iostream>
#include "rapidjson/document.h"
#include <string>
#include <thread>

#include <WinSock2.h>
#include <WS2tcpip.h>

// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")

using namespace rapidjson;
using namespace std;

static unsigned short SERVER_PORT = 27015;

int main() {
    cout << "Client" << endl;

    int r = 0;

    // Winsock 을 초기화한다.
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        std::cerr << "WSAStartup failed with error " << r << std::endl;
        return 1;
    }

    struct sockaddr_in serverAddr;

    // TCP socket 을 만든다.
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // TCP 는 연결 기반이다. 서버 주소를 정하고 connect() 로 연결한다.
    // connect 후에는 별도로 서버 주소를 기재하지 않고 send/recv 를 한다.
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    r = connect(sock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        std::cerr << "connect failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // TODO: ID 입력 및 전송 구현

    // 1초 간격으로 계속 패킷을 보내본다.
    while (true) {
        // 명령어 입력
        string command;
        getline(cin, command);

        // TODO: 명령어 문자열을 JSON으로 변환
        
        int dataLen = command.length() + (size_t)1; // 문자열의 끝을 의미하는 NULL 문자 포함

        // 길이를 먼저 보낸다.
        // binary 로 4bytes 를 길이로 encoding 한다.
        // 이 때 network byte order 로 변환하기 위해서 htonl 을 호출해야된다.
        int dataLenNetByteOrder = htonl(dataLen);
        int offset = 0;
        while (offset < 4) {
            r = send(sock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
            if (r == SOCKET_ERROR) {
                std::cerr << "failed to send length: " << WSAGetLastError() << std::endl;
                return 1;
            }
            offset += r;
        }
        std::cout << "Sent length info: " << dataLen << std::endl;

        // send 로 명령어를 보낸다.
        offset = 0;
        while (offset < dataLen) {
            r = send(sock, command.c_str() + offset, dataLen - offset, 0);
            if (r == SOCKET_ERROR) {
                std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
                return 1;
            }
            std::cout << "Sent " << r << " bytes" << std::endl;
            offset += r;
        }
    }

    // Socket 을 닫는다.
    r = closesocket(sock);
    if (r == SOCKET_ERROR) {
        std::cerr << "closesocket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();
    return 0;
}