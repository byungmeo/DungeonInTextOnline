#include <chrono>
#include <hiredis/hiredis.h>
#include <iostream>
#include <random>
#include "rapidjson/document.h"
#include <mutex>
#include <string>
#include <queue>
#include <thread>

#include <WinSock2.h>
#include <WS2tcpip.h>

// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")

using namespace rapidjson;
using namespace std;

static unsigned short SERVER_PORT = 27015;

SOCKET sock;

string userName;

queue<string> msgQueue;
mutex msgQueueMutex;
condition_variable msgQueueFilledCv;

queue<string> commandQueue;
mutex commandQueueMutex;
condition_variable commandQueueFilledCv;

// 시드값을 얻기 위한 random_device 생성.
std::random_device rd;
// random_device 를 통해 난수 생성 엔진을 초기화 한다.
std::mt19937 gen(rd());
// 0 부터 99 까지 균등하게 나타나는 난수열을 생성하기 위해 균등 분포 정의.
std::uniform_int_distribution<int> dis(0, 99);

bool sendMessage(string message) {
    int r;
    string data = message;
    int dataLen = data.length() + 1; // 문자열의 끝을 의미하는 NULL 문자 포함

    // 길이를 먼저 보낸다.
    // binary 로 4bytes 를 길이로 encoding 한다.
    // 이 때 network byte order 로 변환하기 위해서 htonl 을 호출해야된다.
    int dataLenNetByteOrder = htonl(dataLen);
    int offset = 0;
    while (offset < 4) {
        r = send(sock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "failed to send length: " << WSAGetLastError() << std::endl;
            return false;
        }
        offset += r;
    }
    std::cout << "Sent length info: " << dataLen << std::endl;

    // send 로 명령어를 보낸다.
    offset = 0;
    while (offset < dataLen) {
        r = send(sock, data.c_str() + offset, dataLen - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return false;
        }
        std::cout << "Sent " << r << " bytes" << std::endl;
        offset += r;
    }
}

void messageThreadProc() {
    std::cout << "Message thread is starting." << std::endl;
    while (true) {
        // lock_guard 혹은 unique_lock 의 경우 scope 단위로 lock 범위가 지정되므로,
        // 아래처럼 새로 scope 을 열고 lock 을 잡는 것이 좋다.
        string msg;
        {
            unique_lock<mutex> ul(msgQueueMutex);

            // job queue 에 이벤트가 발생할 때까지 condition variable 을 잡을 것이다.
            while (msgQueue.empty()) {
                msgQueueFilledCv.wait(ul);
            }

            // while loop 을 나왔다는 것은 job queue 에 작업이 있다는 것이다.
            // queue 의 front 를 기억하고 front 를 pop 해서 큐에서 뺀다.
            msg = msgQueue.front();
            msgQueue.pop();

        }

        std::cout << "msgThread : " << msg << std::endl;

        // TODO: JSON으로 된 메시지를 뜯어서 출력
    }

   std::cout << "Message thread is quitting." << std::endl;
}

void socketThreadProc() {
    std::cout << "Socket thread is starting." << std::endl;

    int r;

    while (true) {
        // JSON으로 변환된 명령어를 입력받음
        string command;
        bool hasCommand = false;
        {
            unique_lock<mutex> ul(commandQueueMutex);
            // job queue 에 이벤트가 발생할 때까지 condition variable 을 잡을 것이다.
            if (!commandQueue.empty()) {
                // while loop 을 나왔다는 것은 job queue 에 작업이 있다는 것이다.
                // queue 의 front 를 기억하고 front 를 pop 해서 큐에서 뺀다.
                command = commandQueue.front();
                commandQueue.pop();
                hasCommand = true;
            }
        }

        // 입력이 있으면 서버에 전송한다.
        if (hasCommand) {
            if (sendMessage(command) == false) {
                return;
            }
        }

        fd_set readSet;
        SOCKET maxSock = -1;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        maxSock = max(maxSock, sock);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100;
        r = select(maxSock + 1, &readSet, NULL, NULL, &timeout);

        if (r == SOCKET_ERROR) {
            std::cerr << "select failed: " << WSAGetLastError() << std::endl;
            break;
        } else if (r == 0) continue;

        char buf[1000];
        if (FD_ISSET(sock, &readSet)) {
            bool lenCompleted = false;
            int packetLen = 0;
            int offset = 0;
            while (!lenCompleted) {
                // 길이 정보를 받기 위해서 4바이트를 읽는다.
                // network byte order 로 전성되기 때문에 ntohl() 을 호출한다.
                r = recv(sock, (char*)&(packetLen)+offset, 4 - offset, 0);
                if (r == SOCKET_ERROR) {
                    std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
                    return;
                } else if (r == 0) {
                    // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
                    // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
                    std::cerr << "Socket closed: " << sock << std::endl;
                    return;
                }
                offset += r;

                // 완성 못했다면 다음번에 계속 시도할 것이다.
                if (offset < 4) {
                    continue;
                }

                // network byte order 로 전송했었다.
                // 따라서 이를 host byte order 로 변경한다.
                int dataLen = ntohl(packetLen);
                std::cout << "Received length info: " << dataLen << std::endl;
                packetLen = dataLen;

                // 우리는 Client class 안에 packet 길이를 최대 64KB 로 제한하고 있다.
                // 혹시 우리가 받을 데이터가 이것보다 큰지 확인한다.
                // 만일 크다면 처리 불가능이므로 오류로 처리한다.
                if (packetLen > sizeof(buf)) {
                    std::cerr << "Too big data: " << packetLen << std::endl;
                    return;
                }

                // 이제 packetLen 을 완성했다고 기록하고 offset 을 초기화해준다.
                lenCompleted = true;
                offset = 0;
            }

            r = recv(sock, buf, packetLen, 0);
            string msg = buf;
            {
                lock_guard<mutex> lg(msgQueueMutex);

                bool wasEmpty = msgQueue.empty();
                msgQueue.push(msg);

                // 그리고 worker thread 를 깨워준다.
                // 무조건 condition variable 을 notify 해도 되는데,
                // 해당 condition variable 은 queue 에 뭔가가 들어가서 더 이상 빈 큐가 아닐 때 쓰이므로
                // 여기서는 무의미하게 CV 를 notify하지 않도록 큐의 길이가 0에서 1이 되는 순간 notify 를 하도록 하자.
                if (wasEmpty) {
                    msgQueueFilledCv.notify_one();
                }

                // lock_guard 는 scope 이 벗어날 때 풀릴 것이다.
            }

        }
    }
    std::cout << "Socket thread is quitting." << std::endl;
}

string loginToJson() {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"login\", \"userName\": \"%s\"}", userName.c_str());
    return jsonData;
}

string moveToJson(int x, int y) {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"move\", \"userName\": \"%s\", \"x\": %d, \"y\": %d}", userName.c_str(), x, y);
    return jsonData;
}

string chatToJson(string dest, string msg) {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"chat\", \"userName\": \"%s\", \"dest\": \"%s\", \"msg\": \"%s\"}", userName.c_str(), dest.c_str(), msg.c_str());
    return jsonData;
}

string otherToJson(string command) {
    char jsonData[UCHAR_MAX];
    sprintf_s(jsonData, sizeof(jsonData), "{\"command\": \"%s\", \"userName\": \"%s\"}", command.c_str(), userName.c_str());
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
                std::cerr << "잘못된 좌표 입력" << std::endl;
                cin.clear(); // 에러 비트 초기화
                cin.ignore(UCHAR_MAX, '\n'); // 버퍼를 비운다
                continue; // 처음부터 다시 입력
            }
            cin >> y;
            if (cin.fail() || abs(y) > 3) {
                std::cerr << "잘못된 좌표 입력" << std::endl;
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
        } else if (input.compare("attack") == 0 || input.compare("monsters") == 0 || input.compare("users") == 0) {
            jsonData = otherToJson(input);
            break;
        } else if (input.compare("bot") == 0) {
            return "bot";
        } else if (input.compare("exit") == 0) {
            return "exit";
        } else {
            std::cerr << "잘못된 명령어" << std::endl;
            cin.ignore(UCHAR_MAX, '\n'); //버퍼를 비운다
            continue;
        }
    }

    return jsonData;
}

void pushCommandToQueue(string command) {
    {
        lock_guard<mutex> lg(commandQueueMutex);

        bool wasEmpty = commandQueue.empty();
        commandQueue.push(command);

        // 그리고 worker thread 를 깨워준다.
        // 무조건 condition variable 을 notify 해도 되는데,
        // 해당 condition variable 은 queue 에 뭔가가 들어가서 더 이상 빈 큐가 아닐 때 쓰이므로
        // 여기서는 무의미하게 CV 를 notify하지 않도록 큐의 길이가 0에서 1이 되는 순간 notify 를 하도록 하자.
        if (wasEmpty) {
            commandQueueFilledCv.notify_one();
        }

        // lock_guard 는 scope 이 벗어날 때 풀릴 것이다.
    }

    return;
}

string randomCommandJson() {
    string jsonData;
    int random = dis(gen) % 2;

    // attack, move만 우선  수행
    if (random == 0) {
        jsonData = otherToJson("attack");
    } else {
        int x = dis(gen) % 7 - 3; // -3 ~ 3
        int y = dis(gen) % 7 - 3;
        jsonData = moveToJson(x, y);
    }

    return jsonData;
}

int main() {
    std::cout << "Client" << std::endl;

    std::cout << "닉네임 : ";
    std::cin >> userName;

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
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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

    thread socketThread(socketThreadProc); // 소켓 작업을 처리하는 Thread
    thread msgThreaed(messageThreadProc); // 서버로부터 받은 메시지를 처리하는 Thread

    // 닉네임을 서버에 전송
    pushCommandToQueue(loginToJson());

    while (true) {
        string command;
        command = inputCommandJson();
        if (command.compare("exit") == 0) {
            std::cout << "Client를 종료 합니다." << std::endl;
            break;
        } else if (command.compare("bot") == 0) {
            while (true) {
                command = randomCommandJson();
                pushCommandToQueue(command);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            pushCommandToQueue(command);
        }
    }

    // join
    socketThread.join();
    msgThreaed.join();

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