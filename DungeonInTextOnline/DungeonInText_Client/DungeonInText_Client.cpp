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

string moveToJson(int x, int y) {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"move\", \"userName\": \"unknown\", \"x\": %d, \"y\": %d}", x, y);
    return jsonData;
}

string chatToJson(string dest, string msg) {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"chat\", \"userName\": \"unknown\", \"dest\": \"%s\", \"msg\": \"%s\"}", dest.c_str(), msg.c_str());
    return jsonData;
}

string otherToJson(string command) {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"%s\", \"userName\": \"unknown\"}", command.c_str());
    return jsonData;
}

string inputCommandJson() {
    string input, jsonData;
    while (true) {
        cin >> input;
        if (input.compare("move") == 0) {
            int x, y;
            cin >> x;
            if (cin.fail() || abs(x) > 3) {
                cerr << "잘못된 좌표 입력" << endl;
                cin.clear(); // 에러 비트 초기화
                cin.ignore(UCHAR_MAX, '\n'); // 버퍼를 비운다
                continue; // 처음부터 다시 입력
            }
            cin >> y;
            if (cin.fail() || abs(y) > 3) {
                cerr << "잘못된 좌표 입력" << endl;
                cin.clear(); //에러 비트 초기화
                cin.ignore(UCHAR_MAX, '\n'); //버퍼를 비운다
                continue; // 처음부터 다시 입력
            }
            jsonData = moveToJson(x, y);
            break;
        } else if (input.compare("chat") == 0) {
            string dest, msg;
            cin >> dest;
            getline(cin, msg);
            jsonData = chatToJson(dest, msg.c_str() + 1);
            break;
        } else if (input.compare("attack") == 0 || input.compare("bot") == 0 || input.compare("monsters") == 0 || input.compare("users") == 0) {
            jsonData = otherToJson(input);
            break;
        } else if (input.compare("exit") == 0) {
            return "exit";
        } else {
            cerr << "잘못된 명령어" << endl;
            cin.ignore(UCHAR_MAX, '\n'); //버퍼를 비운다
            continue;
        }
    }

    return jsonData;
}

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
    /*
    */

    while (true) {
        // JSON으로 변환된 명령어를 입력받음
        string command;
        command = inputCommandJson();
        if (command.compare("exit") == 0) {
            cout << "Client를 종료 합니다." << endl;
            break;
        }
        
        // TODO: 서버로부터 로그 수신 구현

        int dataLen = command.length() + 1; // 문자열의 끝을 의미하는 NULL 문자 포함

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

        char buf[1000];
        r = recv(sock, buf, dataLen, 0);
        std::cout << "데이터 되돌려받음 : " << buf << std::endl;
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