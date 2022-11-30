#include <chrono>
#include <condition_variable>
#include <hiredis/hiredis.h>
#include <iostream>
#include <list>
#include <map>
#include "rapidjson/document.h"
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <WinSock2.h>
#include <WS2tcpip.h>

using namespace rapidjson;
using namespace std;

// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")

static unsigned short SERVER_PORT = 27015;
static const int NUM_WORKER_THREADS = 10;

class Client {
public:
    SOCKET sock;  // 이 클라이언트의 active socket

    // TODO: doingRecv -> doingJob
    atomic<bool> doingRecv;

    bool lenCompleted;
    int packetLen;
    char packet[65536];  // 최대 64KB 로 패킷 사이즈 고정
    int offset;

    Client(SOCKET sock) : sock(sock), doingRecv(false), lenCompleted(false), packetLen(0), offset(0) {
    }

    ~Client() {
        cout << "Client destroyed. Socket: " << sock << endl;
    }
};

map<SOCKET, shared_ptr<Client> > activeClients;
mutex activeClientsMutex;

// 패킷이 도착한 client 들의 큐
queue<shared_ptr<Client> > jobQueue;
mutex jobQueueMutex;
condition_variable jobQueueFilledCv;

SOCKET createPassiveSocket() {
    // TCP socket 을 만든다.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        cerr << "socket failed with error " << WSAGetLastError() << endl;
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
        cerr << "bind failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // TCP 는 연결을 받는 passive socket 과 실제 통신을 할 수 있는 active socket 으로 구분된다.
    // passive socket 은 socket() 뒤에 listen() 을 호출함으로써 만들어진다.
    // active socket 은 passive socket 을 이용해 accept() 를 호출함으로써 만들어진다.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        cerr << "listen faijled with error " << WSAGetLastError() << endl;
        return 1;
    }

    return passiveSock;
}

bool recvLength(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    // 이전에 어디까지 작업했는지에 따라 다르게 처리한다.
    // 이전에 packetLen 을 완성하지 못했다. 그걸 완성하게 한다.
    if (client->lenCompleted == false) {
        // 길이 정보를 받기 위해서 4바이트를 읽는다.
        // network byte order 로 전성되기 때문에 ntohl() 을 호출한다.
        int r = recv(activeSock, (char*)&(client->packetLen) + client->offset, 4 - client->offset, 0);
        if (r == SOCKET_ERROR) {
            cerr << "recv failed with error " << WSAGetLastError() << endl;
            return false;
        } else if (r == 0) {
            // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
            // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
            cerr << "Socket closed: " << activeSock << endl;
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
        cout << "[" << activeSock << "] Received length info: " << dataLen << endl;
        client->packetLen = dataLen;

        // 우리는 Client class 안에 packet 길이를 최대 64KB 로 제한하고 있다.
        // 혹시 우리가 받을 데이터가 이것보다 큰지 확인한다.
        // 만일 크다면 처리 불가능이므로 오류로 처리한다.
        if (client->packetLen > sizeof(client->packet)) {
            cerr << "[" << activeSock << "] Too big data: " << client->packetLen << endl;
            return false;
        }

        // 이제 packetLen 을 완성했다고 기록하고 offset 을 초기화해준다.
        client->lenCompleted = true;
        client->offset = 0;
    }
}

bool recvPacket(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    // 여기까지 도달했다는 것은 packetLen 을 완성한 경우다. (== lenCompleted 가 true)
    // packetLen 만큼 데이터를 읽으면서 완성한다.
    if (client->lenCompleted == false) {
        return true;
    }

    int r = recv(activeSock, client->packet + client->offset, client->packetLen - client->offset, 0);
    if (r == SOCKET_ERROR) {
        cerr << "recv failed with error " << WSAGetLastError() << endl;
        return false;
    } else if (r == 0) {
        // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
        // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
        return false;
    }
    client->offset += r;

    // 완성한 경우와 partial recv 인 경우를 구분해서 로그를 찍는다.
    if (client->offset == client->packetLen) {
        cout << "[" << activeSock << "] Received " << client->packetLen << " bytes" << endl;

        // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
        client->lenCompleted = false;
        client->offset = 0;
        client->packetLen = 0;
    } else {
        cout << "[" << activeSock << "] Partial recv " << r << "bytes. " << client->offset << "/" << client->packetLen << endl;
    }
}

bool processClient(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;
    int r;

    // packet을 받기 전 length를 먼저 받는다.
    if (recvLength(client) == false) {
        return false;
    }

    // packet을 받는다.
    if (recvPacket(client) == false) {
        return false;
    }

    // TODO: JSON 파싱 후 처리
    cout << "받은 원본 데이터 : " << client->packet << endl;
    Document d;
    d.Parse(client->packet);
    Value& s = d["command"];
    string command = s.GetString();
    s = d["userName"];
    string userName = s.GetString();
    cout << "명령어 : " << command << endl;
    cout << "유저명 : " << userName << endl;
    if (command.compare("move") == 0) {
        int x, y;
        s = d["x"];
        x = s.GetInt();
        s = d["y"];
        y = s.GetInt();
        cout << "좌표 : (" << x << ", " << y << ")" << endl;
    } else if (command.compare("chat") == 0) {
        string dest, msg;
        s = d["dest"];
        dest = s.GetString();
        s = d["msg"];
        msg = s.GetString();
        cout << "귓속말 상대 : " << dest << endl;
        cout << "귓속막 내용 : " << msg << endl;
    } else if (command.compare("attack") == 0) {
        // 우선 유저가 공격 했다는 사실을 다른 모든 유저에게 단순 공지
    } else if (command.compare("monsters") == 0) {
    } else if (command.compare("users") == 0) cout << "잘못된 명령어" << endl;
    
    // 우선 명령어를 그대로 되돌려 준다
    // r = send(activeSock, client->packet, r, 0);
    string data = client->packet;
    int dataLen = data.length() + 1; // 문자열의 끝을 의미하는 NULL 문자 포함

    // 길이를 먼저 보낸다.
    // binary 로 4bytes 를 길이로 encoding 한다.
    // 이 때 network byte order 로 변환하기 위해서 htonl 을 호출해야된다.
    int dataLenNetByteOrder = htonl(dataLen);
    int offset = 0;
    while (offset < 4) {
        r = send(activeSock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
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
        r = send(activeSock, data.c_str() + offset, dataLen - offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return false;
        }
        std::cout << "Sent " << r << " bytes" << std::endl;
        offset += r;
    }

    return true;
}

void workerThreadProc(int workerId) {
    cout << "Worker thread is starting. WorkerId: " << workerId << endl;

    while (true) {
        // lock_guard 혹은 unique_lock 의 경우 scope 단위로 lock 범위가 지정되므로,
        // 아래처럼 새로 scope 을 열고 lock 을 잡는 것이 좋다.
        shared_ptr<Client> client;
        {
            unique_lock<mutex> ul(jobQueueMutex);

            // job queue 에 이벤트가 발생할 때까지 condition variable 을 잡을 것이다.
            while (jobQueue.empty()) {
                jobQueueFilledCv.wait(ul);
            }

            // while loop 을 나왔다는 것은 job queue 에 작업이 있다는 것이다.
            // queue 의 front 를 기억하고 front 를 pop 해서 큐에서 뺀다.
            client = jobQueue.front();
            jobQueue.pop();

        }

        // 위의 block 을 나왔으면 client 는 존재할 것이다.
        // 그러나 혹시 나중에 코드가 변경될 수도 있고 그러니 client 가 null 이 아닌지를 확인 후 처리하도록 하자.
        // shared_ptr 은 boolean 이 필요한 곳에 쓰일 때면 null 인지 여부를 확인해준다.
        if (client) {
            SOCKET activeSock = client->sock;
            bool successful = processClient(client);
            if (successful == false) {
                closesocket(activeSock);

                // 전체 동접 클라이언트 목록인 activeClients 에서 삭제한다.
                // activeClients 는 메인 쓰레드에서도 접근한다. 따라서 mutex 으로 보호해야될 대상이다.
                // lock_guard 가 scope 단위로 동작하므로 lock 잡히는 영역을 최소화하기 위해서 새로 scope 을 연다.
                {
                    lock_guard<mutex> lg(activeClientsMutex);

                    // activeClients 는 key 가 SOCKET 타입이고, value 가 shared_ptr<Client> 이므로 socket 으로 지운다.
                    activeClients.erase(activeSock);
                }
            } else {
                // 다시 select 대상이 될 수 있도록 플래그를 꺼준다.
                // 참고로 오직 성공한 경우만 이 flag 를 다루고 있다.
                // 그 이유는 오류가 발생한 경우는 어차피 동접 리스트에서 빼버릴 것이고 select 를 할 일이 없기 때문이다.
                client->doingRecv.store(false);
            }
        }
    }

    cout << "Worker thread is quitting. Worker id: " << workerId << endl;
}

int main() {
    cout << "Server" << endl;

    int r = 0;

    // Initial Winsock
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        cerr << "WSAStartup failed with error " << r << endl;
        return 1;
    }

    // Create passive socket
    SOCKET passiveSock = createPassiveSocket();

    list<shared_ptr<thread> > threads;
    for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
        shared_ptr<thread> workerThread(new thread(workerThreadProc, i));
        threads.push_back(workerThread);
    }

    while (true) {
        // TODO: writeSet 추가

        fd_set readSet, exceptionSet;

        // Initial Set
        FD_ZERO(&readSet);
        FD_ZERO(&exceptionSet);

        // select 의 첫번째 인자는 max socket 번호에 1을 더한 값이다.
        // 따라서 max socket 번호를 계산한다.
        SOCKET maxSock = -1;

        // passive socket 은 기본으로 각 socket set 에 포함되어야 한다.
        FD_SET(passiveSock, &readSet);
        FD_SET(passiveSock, &exceptionSet);
        maxSock = max(maxSock, passiveSock);

        // 현재 남아있는 active socket 들에 대해서도 모두 set 에 넣어준다.
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;

            // 아직 job queue 안에 안들어간 클라이언트만 select 확인 대상으로 한다.
            if (client->doingRecv.load() == false) {
                FD_SET(activeSock, &readSet);
                FD_SET(activeSock, &exceptionSet);
                maxSock = max(maxSock, activeSock);
            }
        }

        // select 를 해준다. 동접이 있더라도 doingRecv 가 켜진 것들은 포함하지 않았었다.
        // 이런 것들은 worker thread 가 처리 후 doingRecv 를 끄면 다시 select 대상이 되어야 하는데,
        // 아래는 timeout 없이 한정 없이 select 를 기다리므로 doingRecv 변경으로 다시 select 되어야 하는 것들이
        // 굉장히 오래 걸릴 수 있다. 그런 문제를 해결하기 위해서 select 의 timeout 을 100 msec 정도로 제한한다.
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100;
        r = select(maxSock + 1, &readSet, NULL, &exceptionSet, &timeout);

        // select 의 반환값이 오류일 때 SOCKET_ERROR, 그 외의 경우 이벤트가 발생한 소켓 갯수이다.
        // 따라서 반환값 r 이 0인 경우는 아래를 스킵하게 한다.
        if (r == SOCKET_ERROR) {
            cerr << "select failed: " << WSAGetLastError() << endl;
            break;
        } else if (r == 0)  continue;

        // passive socket 이 readable 하다면 이는 새 연결이 들어왔다는 것이다.
        if (FD_ISSET(passiveSock, &readSet)) {
            // passive socket 을 이용해 accept() 를 한다.
            // accept() 는 blocking 이지만 이미 select() 를 통해 새 연결이 있음을 알고 accept() 를 호출한다.
            // 따라서 여기서는 blocking 되지 않는다.
            // 연결이 완료되고 만들어지는 소켓은 active socket 이다.
            cout << "Waiting for a connection" << endl;
            struct sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET activeSock = accept(passiveSock, (sockaddr*)&clientAddr, &clientAddrSize);

            // accpet() 가 실패하면 해당 연결은 이루어지지 않았음을 의미한다.
            // 그 연결이 잘못된다고 하더라도 다른 연결들을 처리해야되므로 에러가 발생했다고 하더라도 계속 진행한다.
            if (activeSock == INVALID_SOCKET) {
                cerr << "accept failed with error " << WSAGetLastError() << endl;
                return 1;
            } else {
                // 새로 client 객체를 만든다.
                shared_ptr<Client> newClient(new Client(activeSock));

                // socket 을 key 로 하고 해당 객체 포인터를 value 로 하는 map 에 집어 넣는다.
                activeClients.insert(make_pair(activeSock, newClient));

                // 로그를 찍는다.
                char strBuf[1024];
                inet_ntop(AF_INET, &(clientAddr.sin_addr), strBuf, sizeof(strBuf));
                cout << "New client from " << strBuf << ":" << ntohs(clientAddr.sin_port) << ". "
                    << "Socket: " << activeSock << endl;
            }
        }

        // 오류 이벤트가 발생하는 소켓의 클라이언트는 제거한다.
        // activeClients 를 순회하는 동안 그 내용을 변경하면 안되니 지우는 경우를 위해 별도로 list 를 쓴다.
        list<SOCKET> toDelete;
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;

            if (FD_ISSET(activeSock, &exceptionSet)) {
                cerr << "Exception on socket " << activeSock << endl;

                // 소켓을 닫는다.
                closesocket(activeSock);

                // 지울 대상에 포함시킨다.
                // 여기서 activeClients 에서 바로 지우지 않는 이유는 현재 activeClients 를 순회중이기 때문이다.
                toDelete.push_back(activeSock);

                // 소켓을 닫은 경우 더 이상 처리할 필요가 없으니 아래 read 작업은 하지 않는다.
                continue;
            }

            // 읽기 이벤트가 발생하는 소켓의 경우 recv() 를 처리한다.
            // 주의: 아래는 여전히 recv() 에 의해 blocking 이 발생할 수 있다.
            //       우리는 이를 producer-consumer 형태로 바꿀 것이다.
            if (FD_ISSET(activeSock, &readSet)) {
                // 이제 다시 select 대상이 되지 않도록 client 의 flag 를 켜준다.
                client->doingRecv.store(true);

                // 해당 client 를 job queue 에 넣자. lock_guard 를 써도 되고 unique_lock 을 써도 된다.
                // lock 걸리는 범위를 명시적으로 제어하기 위해서 새로 scope 을 열어준다.
                {
                    lock_guard<mutex> lg(jobQueueMutex);

                    bool wasEmpty = jobQueue.empty();
                    jobQueue.push(client);

                    // 그리고 worker thread 를 깨워준다.
                    // 무조건 condition variable 을 notify 해도 되는데,
                    // 해당 condition variable 은 queue 에 뭔가가 들어가서 더 이상 빈 큐가 아닐 때 쓰이므로
                    // 여기서는 무의미하게 CV 를 notify하지 않도록 큐의 길이가 0에서 1이 되는 순간 notify 를 하도록 하자.
                    if (wasEmpty) {
                        jobQueueFilledCv.notify_one();
                    }

                    // lock_guard 는 scope 이 벗어날 때 풀릴 것이다.
                }
            }
        }

        // 이제 지울 것이 있었다면 지운다.
        for (auto& closedSock : toDelete) {

            // 맵에서 지우고 객체도 지워준다.
            // shared_ptr 을 썼기 때문에 맵에서 지워서 더 이상 사용하는 곳이 없어지면 객체도 지워진다.
            activeClients.erase(closedSock);
        }
    }

    // 이제 threads 들을 join 한다.
    for (shared_ptr<thread>& workerThread : threads) {
        workerThread->join();
    }

    // 연결을 기다리는 passive socket 을 닫는다.
    r = closesocket(passiveSock);
    if (r == SOCKET_ERROR) {
        cerr << "closesocket(passive) failed with error " << WSAGetLastError() << endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();
    return 0;
}