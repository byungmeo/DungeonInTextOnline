#include "mylib.h"
#include "rapidjson/document.h"

#include <hiredis/hiredis.h>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include "JsonParser.h"
#include "Player.h"
#include "Rng.h"
#include "ServerCore.h"
#include "Slime.h"
#include <WinSock2.h>
#include <WS2tcpip.h>

using namespace rapidjson;
using namespace std;

// ws2_32.lib 를 링크한다.
#pragma comment(lib, "Ws2_32.lib")
static const int NUM_WORKER_THREADS = 10;
static const int NUM_MAX_SLIMES = 10;


// REST API TCP 소켓 정보
static const char* REST_SERVER_ADDRESS = "127.0.0.1";
static const unsigned short REST_SERVER_PORT = 27016;

// REST API 응답 테스트를 위한 고정된 response 패킷 (Content-Length도 고정)
static const string response_packet = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nContent-Type: text/plain\r\n\r\nResponse";

redisContext* c;

map<SOCKET, shared_ptr<Client>> activeClients;
mutex activeClientsMutex;

list<shared_ptr<Slime>> slimeList;
mutex slimeListMutex;

queue<shared_ptr<Slime>> genSlimeQueue;
mutex genSlimeQueueMutex;
condition_variable genSlimeQueueFilledCv;

// 패킷이 도착한 client 들의 큐
queue<shared_ptr<Client> > jobQueue;
mutex jobQueueMutex;
condition_variable jobQueueFilledCv;

SOCKET createPassiveSocketREST() {
    // REST API 통신용 TCP socket 을 만든다.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // socket 을 특정 주소, 포트에 바인딩 한다.
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

    // passive socket을 생성하고 반환한다.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        std::cerr << "listen faijled with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    return passiveSock;
}

void initialUser(string userName, shared_ptr<Client> client) {
    shared_ptr<Player> playerInfo;
    {
        unique_lock<mutex> ul(client->playerInfoMutex);
        playerInfo = client->playerInfo;
        playerInfo->name = userName;
        playerInfo->hp = 30; // 기본값 30
        playerInfo->x = Rng::dis(Rng::gen) % 31; // 0 ~ 30
        playerInfo->y = Rng::dis(Rng::gen) % 31; // 0 ~ 30
        playerInfo->str = 3; // 기본값 3
        playerInfo->inventory["hp"] = 1; // 기본값 1개
        playerInfo->inventory["str"] = 1; // 기본값 1개
    }
}

void loadUser(string userName, shared_ptr<Client> client) {
    shared_ptr<Player> playerInfo;
    redisReply* reply;
    {
        unique_lock<mutex> ul(client->playerInfoMutex);
        playerInfo = client->playerInfo;
        playerInfo->name = userName;
        reply = (redisReply*)redisCommand(c, "GET USER:%s:hp", userName.c_str());
        playerInfo->hp = atoi(reply->str);
        reply = (redisReply*)redisCommand(c, "GET USER:%s:x", userName.c_str());
        playerInfo->x = atoi(reply->str);
        reply = (redisReply*)redisCommand(c, "GET USER:%s:y", userName.c_str());
        playerInfo->y = atoi(reply->str);
        reply = (redisReply*)redisCommand(c, "GET USER:%s:str", userName.c_str());
        playerInfo->str = atoi(reply->str);
        reply = (redisReply*)redisCommand(c, "HGET USER:%s:inventory hp", userName.c_str());
        playerInfo->inventory["hp"] = atoi(reply->str);
        reply = (redisReply*)redisCommand(c, "HGET USER:%s:inventory str", userName.c_str());
        playerInfo->inventory["str"] = atoi(reply->str);
        // inventory
    }
}

// GET 요청을 처리하고 Response를 생성합니다.
string createGetResponse(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    vector<string> params = split(split(client->packet, '?')[1], '&');
    vector<string> p1 = split(params[0], '=');
    vector<string> p2 = split(params[1], '=');
    
    string command = p1[1];
    string userName = p2[1];
    string json = "";

    if (command.compare("users") == 0) {
        json = JsonParser::userListToJson(activeClientsMutex, activeClients);
    } else if (command.compare("monsters") == 0) {
        json = JsonParser::monsterListToJson(client, slimeListMutex, slimeList);
    } else {
        json = JsonParser::failCommandToJson("Unknown Command");
    }

    char buffer[BUFFER_SIZE];
    sprintf_s(buffer, sizeof(buffer), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s", json.size(), json.c_str());
    string request = buffer;
    return request;
}

// POST 요청을 처리하고 Response를 생성합니다.
string createPostResponse(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    char* body = client->packet;

    // JSON을 파싱해서 태그별로 처리한다.
    Document d;
    d.Parse(body);
    Value& s = d["command"];
    string command = s.GetString();
    s = d["userName"];
    string userName = s.GetString();

    bool needLogin = false;
    {
        unique_lock<mutex> ul(client->playerInfoMutex);
        needLogin = client->playerInfo->name.empty();
    }

    if (needLogin) {
        redisReply* reply;

        // 기존 접속자의 소켓 정보가 존재하는지 확인
        reply = (redisReply*)redisCommand(c, "EXISTS USER:%s:socket", userName.c_str());
        if (reply->integer) {
            // 접속 흔적이 있다 (동접인지 아닌지는 아직 모른다)
            reply = (redisReply*)redisCommand(c, "GET USER:%s:socket", userName.c_str());
            SOCKET anotherSock = atoi(reply->str);
            if (activeSock != anotherSock) {
                // 동접인지 아닌지 확인해야 한다.
                {
                    unique_lock<mutex> ul(activeClientsMutex);
                    if (activeClients.count(anotherSock)) {
                        // 확실하게 동접인 상황
                        {
                            unique_lock<mutex> ul(client->playerInfoMutex);
                            // 정보를 그대로 이관한다. (유저 정보 객체의 소멸자가 실행되지 않을 것이다)
                            client->playerInfo = activeClients[anotherSock]->playerInfo;
                        }
                        // 동접 소켓은 닫는다. (알아서 activeClients에서 지워질 것이다)
                        closesocket(anotherSock);
                    } else {
                        // 동접이 아닌 상황이다. 마지막 종료 전 정보를 불러온다.
                        loadUser(userName, client);
                    }
                }
            } else {
                // 동접이 아니다. 우연히 이전 접속 소켓과 번호가 같은 경우라 만료 기한 설정만 취소한다.
                freeReplyObject(reply);
                reply = (redisReply*)redisCommand(c, "PERSIST USER:%s:socket", userName.c_str());
            }

        } else {
            // 유저 정보를 확실하게 초기화
            initialUser(userName, client);
        }
        freeReplyObject(reply);

        // 유저의 새 소켓 번호는 바로 저장
        reply = (redisReply*)redisCommand(c, "SET USER:%s:socket %d", userName.c_str(), (int)activeSock);
        freeReplyObject(reply);

        reply = (redisReply*)redisCommand(c, "GET USER:%s:socket", userName.c_str());
        // 유저가 로그인 하였다고 모든 유저에게 공지
        {
            unique_lock<mutex> ul(activeClientsMutex);
            for (auto& pair : activeClients) {
                // 디버깅을 위해 소켓 번호도 함께 공지
                SendMsg(pair.second, JsonParser::welcomeToJson(userName + ":" + reply->str));
            }
        }
        freeReplyObject(reply);
    }

    string json;

    if (command.compare("move") == 0) {
        int x, y;
        s = d["x"];
        x = s.GetInt();
        s = d["y"];
        y = s.GetInt();

        int current_x, current_y;
        {
            unique_lock<mutex> ul(client->playerInfoMutex);
            client->playerInfo->move(x, y);
            current_x = client->playerInfo->x;
            current_y = client->playerInfo->y;
        }
        json = JsonParser::positionToJson(current_x, current_y);
    } else if (command.compare("attack") == 0) {
        int x, y, str;
        bool isPlayerDied;
        {
            unique_lock<mutex> ul(client->playerInfoMutex);
            isPlayerDied = client->playerInfo->isDie();
            x = client->playerInfo->x;
            y = client->playerInfo->y;
            str = client->playerInfo->getStr();
        }

        if (!isPlayerDied) {
            list<shared_ptr<Slime>> toDie;
            {
                unique_lock<mutex> ul(slimeListMutex);
                for (shared_ptr<Slime> slime : slimeList) {
                    if (!slime->isDie() && slime->isRange(x, y)) {
                        int damage = slime->hitBy(str);
                        if (slime->isDie()) toDie.push_back(slime);
                        {
                            if (slime->haveHpPotion) {
                                client->playerInfo->getItem("hp");
                                //sendMessage(client, getItemToJson("hp"));
                            }
                            if (slime->haveStrPotion) {
                                client->playerInfo->getItem("str");
                                //sendMessage(client, getItemToJson("str"));
                            }

                            unique_lock<mutex> ul(activeClientsMutex);
                            for (auto& pair : activeClients) {
                                SendMsg(pair.second, JsonParser::attackToJson(userName, slime->slimeId, damage));
                            }
                        }
                    }
                }

                for (shared_ptr<Slime> slime : toDie) {
                    slimeList.remove(slime);
                    {
                        unique_lock<mutex> ul(activeClientsMutex);
                        for (auto& pair : activeClients) {
                            SendMsg(pair.second, JsonParser::killLogToJson(userName, slime->slimeId));
                        }
                    }
                    cout << "Slime(" << slime->slimeId << ") 이 죽었습니다." << endl;
                }
            }

            json = JsonParser::completeAttackToJson();
        } else {
            json = JsonParser::failCommandToJson("Your Died. Can't Attack");
        }
    } else if (command.compare("use") == 0) {
        string item;
        item = (s = d["item"]).GetString();
        int effect = 0;
        {
            unique_lock<mutex> ul(client->playerInfoMutex);
            effect = client->playerInfo->useItem(item);
        }
        json = JsonParser::itemEffectToJson(item, effect);
    } else {
        json = JsonParser::failCommandToJson("Unknown Command");
    }

    char buffer[BUFFER_SIZE];
    sprintf_s(buffer, sizeof(buffer), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s", json.size(), json.c_str());
    string request = buffer;
    return request;
}

bool processRequest(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;
    int r = 0;
    string type = "";
    string param = "";

    // Header 부분을 읽는다.
    if (client->lenCompleted == false) {
        while (!client->lenCompleted) {
            r = recv(activeSock, client->packet + client->offset, 1, 0);
            if (r == SOCKET_ERROR) {
                std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
                return false;
            } else if (r == 0) {
                // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
                // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
                return false;
            }
            client->offset++;

            if (client->offset >= 2 && client->packet[client->offset - 2] == '\r' && client->packet[client->offset - 1] == '\n') {
                if (client->offset == 2 && client->packet[0] == '\r' && client->packet[1] == '\n') {
                    // Header 부분을 모두 읽은 경우
                    client->lenCompleted = true;
                } else {
                    // Header 속성 하나를 모두 읽은 경우
                    client->packet[client->offset - 1] = '\0'; // \r과 \n를 제외하고 문자열의 끝임을 알린다.
                    string field = client->packet;
                    vector<string> result = split(field, ':');
                    if (result.size() >= 2) {
                        string key = result[0];
                        string value = "";
                        for (int i = 1; i < result.size(); ++i) {
                            value += result[i];
                        }
                        trim(value); // 양옆 공백을 제거
                        cout << "KEY : " << key << endl;
                        cout << "VAL : " << value << endl << endl;

                        if (key.compare("Content-Length") == 0) {
                            client->packetLen = atoi(value.c_str());
                        }
                    } else if (result.size() == 1) {
                        // 맨 첫줄
                        // [0]: 요청타입 / [1]: 파라메터 / [2]: 프로토콜버전
                        vector<string> result = split(field, ' ');
                        type = result[0];
                        param = result[1];
                        string protocol = result[2];
                        cout << "Request Type : " << type << endl;
                        cout << "Params : " << param << endl;
                        cout << "Protocol : " << protocol << endl;

                        // GET 요청의 경우 패킷 길이를 0으로 지정한다. (Content-Length 헤더도 없다)
                        // 더 이상 헤더 정보는 필요 없지만, 모두 다 받지 않으면
                        // REST API Client에서 다음 요청을 보내오면 이전 요청의 헤더를 받아버리기 때문에
                        // 헤더를 모두 받은 후 lenCompleted를 true로 바꿔야 한다.
                        if (type.compare("GET") == 0) {
                            client->packetLen = 0;
                        }
                    }

                    // 버퍼 초기화
                    fill(client->packet, client->packet + client->offset, 0xcccccccc);
                }
                client->offset = 0;
            }
        }
    }

    // 여기까지 도달했다는 것은 packetLen 을 완성한 경우다. (== lenCompleted 가 true)
    // packetLen 만큼 데이터를 읽으면서 완성한다.
    if (client->lenCompleted == false) {
        return true;
    }

    // 받을게 있다면 받는다.
    if (client->packetLen != 0) {
        // Body 부분을 읽는다.
        r = recv(client->sock, client->packet + client->offset, client->packetLen - client->offset, 0);
        if (r == SOCKET_ERROR) {
            std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
            return false;
        } else if (r == 0) {
            // 메뉴얼을 보면 recv() 는 소켓이 닫힌 경우 0 을 반환함을 알 수 있다.
            // 따라서 r == 0 인 경우도 loop 을 탈출하게 해야된다.
            return false;
        }
        client->offset += r;
    }

    // 완성한 경우와 partial recv 인 경우를 구분해서 로그를 찍는다.
    if (client->offset == client->packetLen) {
        cout << "[" << activeSock << "] Received " << client->packetLen << " bytes" << endl;

        client->packet[client->offset] = '\0'; // 버퍼의 뒤 쓰레기값부분은 자르도록 널 문자를 추가
        cout << client->packet << endl;

        // 다음 패킷을 위해 패킷 관련 정보를 초기화한다.
        client->lenCompleted = false;
        client->offset = 0;
        client->packetLen = 0;

        // Body 부분에 JSON 메시지를 담고 헤더의 Content-Length를 지정하여 Response 메시지를 만든다.
        string response;
        if (type.compare("GET") == 0) {
            sprintf_s(client->packet, sizeof(client->packet), param.c_str());
            response = createGetResponse(client);
        } else if (type.compare("POST") == 0) {
            response = createPostResponse(client);
        } else {
            std::cerr << "알 수 없는 HTTP 요청 타입" << std::endl;
            return 1;
        }
        

        // Response를 전송한다.
        int offset = 0;
        while (offset < response.length()) {
            r = send(client->sock, response.c_str() + offset, response.length() - offset, 0);
            if (r == SOCKET_ERROR) {
                std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
                return 1;
            }
            std::cout << "Sent " << r << " bytes" << std::endl;
            offset += r;
        }
    } else {
        cout << "[" << activeSock << "] Partial recv " << r << "bytes. " << client->offset << "/" << client->packetLen << endl;
    }

    return true;
}

bool processClient(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;

    // packet을 받기 전 length를 먼저 받는다.
    if (RecvLength(client) == false) return false;

    // 아직 length를 다 받지 못 한 경우
    if (client->lenCompleted == false) return false;

    // packet을 받는다.
    if (RecvPacket(client) == false) return false;

    // 아직 packet을 다 받지 못 한 경우
    if (client->lenCompleted == true) return false;

    // JSON을 파싱해서 태그별로 처리한다.
    std::cout << "Recieve Command : " << client->packet << std::endl;
    Document d;
    d.Parse(client->packet);
    Value& s = d["command"];
    string command = s.GetString();
    s = d["userName"];
    string userName = s.GetString();

    if (command.compare("move") == 0) {
        int x, y;
        s = d["x"];
        x = s.GetInt();
        s = d["y"];
        y = s.GetInt();

        int current_x, current_y;
        {
            unique_lock<mutex> ul(client->playerInfoMutex);
            client->playerInfo->move(x, y);
            current_x = client->playerInfo->x;
            current_y = client->playerInfo->y;
        }
        SendMsg(client, JsonParser::positionToJson(current_x, current_y));
    } else if (command.compare("chat") == 0) {
        string dest, msg;
        dest = (s = d["dest"]).GetString();
        msg = (s = d["msg"]).GetString();
        redisReply* reply;
        reply = (redisReply*)redisCommand(c, "GET USER:%s:socket", dest.c_str());
        {
            unique_lock<mutex> ul(activeClientsMutex);
            SendMsg(activeClients[atoi(reply->str)], JsonParser::whisperToJson(userName, dest, msg));
        }
    } else if (command.compare("attack") == 0) {
        int x, y, str;
        {
            unique_lock<mutex> ul(client->playerInfoMutex);
            if (client->playerInfo->isDie()) return true;
            x = client->playerInfo->x;
            y = client->playerInfo->y;
            str = client->playerInfo->getStr();
        }

        list<shared_ptr<Slime>> toDie;
        {
            unique_lock<mutex> ul(slimeListMutex);
            for (shared_ptr<Slime> slime : slimeList) {
                if (!slime->isDie() && slime->isRange(x, y)) {
                    int damage = slime->hitBy(str);
                    if (slime->isDie()) toDie.push_back(slime);
                    {
                        if (slime->haveHpPotion) {
                            client->playerInfo->getItem("hp");
                            SendMsg(client, JsonParser::getItemToJson("hp"));
                        }
                        if (slime->haveStrPotion) {
                            client->playerInfo->getItem("str");
                            SendMsg(client, JsonParser::getItemToJson("str"));
                        }

                        unique_lock<mutex> ul(activeClientsMutex);
                        for (auto& pair : activeClients) {
                            SendMsg(pair.second, JsonParser::attackToJson(userName, slime->slimeId, damage));
                        }
                    }
                }
            }

            for (shared_ptr<Slime> slime : toDie) {
                slimeList.remove(slime);
                {
                    unique_lock<mutex> ul(activeClientsMutex);
                    for (auto& pair : activeClients) {
                        SendMsg(pair.second, JsonParser::killLogToJson(userName, slime->slimeId));
                    }
                }
                cout << "Slime(" << slime->slimeId << ") 이 죽었습니다." << endl;
            }
        }
    } else if (command.compare("login") == 0) {
        // TODO: 모든 Redis 작업 예외처리 및 함수화
        redisReply* reply;

        // 기존 접속자의 소켓 정보가 존재하는지 확인
        reply = (redisReply*)redisCommand(c, "EXISTS USER:%s:socket", userName.c_str());
        if (reply->integer) {
            // 접속 흔적이 있다 (동접인지 아닌지는 아직 모른다)
            reply = (redisReply*)redisCommand(c, "GET USER:%s:socket", userName.c_str());
            SOCKET anotherSock = atoi(reply->str);
            if (activeSock != anotherSock) {
                // 동접인지 아닌지 확인해야 한다.
                {
                    unique_lock<mutex> ul(activeClientsMutex);
                    if (activeClients.count(anotherSock)) {
                        // 확실하게 동접인 상황
                        {
                            unique_lock<mutex> ul(client->playerInfoMutex);
                            // 정보를 그대로 이관한다. (유저 정보 객체의 소멸자가 실행되지 않을 것이다)
                            client->playerInfo = activeClients[anotherSock]->playerInfo;
                        }
                        // 동접 소켓은 닫는다. (알아서 activeClients에서 지워질 것이다)
                        closesocket(anotherSock);
                    } else {
                        // 동접이 아닌 상황이다. 마지막 종료 전 정보를 불러온다.
                        loadUser(userName, client);
                    }
                }
            } else {
                // 동접이 아니다. 우연히 이전 접속 소켓과 번호가 같은 경우라 만료 기한 설정만 취소한다.
                freeReplyObject(reply);
                reply = (redisReply*)redisCommand(c, "PERSIST USER:%s:socket", userName.c_str());
            }
            
        } else {
            // 유저 정보를 확실하게 초기화
            initialUser(userName, client);
        }
        freeReplyObject(reply);

        // 유저의 새 소켓 번호는 바로 저장
        reply = (redisReply*)redisCommand(c, "SET USER:%s:socket %d", userName.c_str(), (int)activeSock);
        freeReplyObject(reply);

        reply = (redisReply*)redisCommand(c, "GET USER:%s:socket", userName.c_str());
        // 유저가 로그인 하였다고 모든 유저에게 공지
        {
            unique_lock<mutex> ul(activeClientsMutex);
            for (auto& pair : activeClients) {
                // 디버깅을 위해 소켓 번호도 함께 공지
                SendMsg(pair.second, JsonParser::welcomeToJson(userName + ":" + reply->str));
            }
        }
        freeReplyObject(reply);
    } else if (command.compare("monsters") == 0) {
        SendMsg(client, JsonParser::monsterListToJson(client, slimeListMutex, slimeList));
    } else if (command.compare("users") == 0) {
        SendMsg(client, JsonParser::userListToJson(activeClientsMutex, activeClients));
    } else if (command.compare("use") == 0) {
        string item;
        item = (s = d["item"]).GetString();
        int effect = 0;
        {
            unique_lock<mutex> ul(client->playerInfoMutex);
            effect = client->playerInfo->useItem(item);
        }
        SendMsg(client, JsonParser::itemEffectToJson(item, effect));
    } else std::cout << "잘못된 명령어" << std::endl;

    return true;
}

int genSlime(int slimeId) {
    int size = 0;
    {
        unique_lock<mutex> ul(slimeListMutex);
        size = slimeList.size();
    }

    {
        unique_lock<mutex> ul(genSlimeQueueMutex);
        for (int i = size + genSlimeQueue.size(); i < 10; ++i) {
            shared_ptr<Slime> slime(new Slime(++slimeId));
            genSlimeQueue.push(slime);
        }
        genSlimeQueueFilledCv.notify_all();
    }
    return slimeId;
}

void slimeThreadProc(int threadId) {
    std::cout << "Slime thread is starting. threadId: " << threadId << std::endl;
    shared_ptr<Slime> slime = NULL;
    while (true) {
        if (slime == NULL) {
            {
                unique_lock<mutex> ul(genSlimeQueueMutex);

                // 슬라임이 생성되기 전까지 대기할 것이다.
                while (genSlimeQueue.empty()) {
                    genSlimeQueueFilledCv.wait(ul);
                }

                // 슬라임을 할당받는다.
                slime = genSlimeQueue.front();
                genSlimeQueue.pop();
                {
                    unique_lock<mutex> ul(slimeListMutex);
                    slimeList.push_back(slime);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!slime->isDie()) {
                slime->attack(activeClientsMutex, activeClients);
            } else {
                slime = NULL; // 슬라임을 놓아주고 다음 슬라임 생성을 기다린다.
            }
        }
    }
    std::cout << "Slime thread is quitting. threadId: " << threadId << std::endl;
}

void gameManagerThreadProc() {
    std::cout << "GameManager thread is starting." << std::endl;
    int slimeId = 0;
    while (true) {
        // 1분에 한번씩 슬라임이 10마리가 되도록 주기적으로 생성 된다.
        slimeId = genSlime(slimeId);
        std::cout << "슬라임 생성 완료" << std::endl;
        std::this_thread::sleep_for(std::chrono::minutes(1));
    }
    std::cout << "GameManager thread is quitting." << std::endl;
}

void workerThreadProc(int workerId) {
    std::cout << "Worker thread is starting. WorkerId: " << workerId << std::endl;

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

            // REST 요청은 다른 방법으로 처리한다.
            bool successful;
            if (client->isREST) {
                successful = processRequest(client);
            } else {
                successful = processClient(client);
            }
            
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

    std::cout << "Worker thread is quitting. Worker id: " << workerId << std::endl;
}

int main() {
    std::cout << "Server" << std::endl;

    int r = 0;

    // Initial Winsock
    WSADATA wsaData;
    r = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (r != NO_ERROR) {
        std::cerr << "WSAStartup failed with error " << r << std::endl;
        return 1;
    }

    // Redis 연결
    c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            std::cout << "Exception on Redis Connect : " << c->errstr << std::endl;
            return 1;
        } else {
            std::cout << "Can't allocate redis context" << std::endl;
        }
    }

    // Create passive socket
    SOCKET passiveSock = CreatePassiveSocket();
    // REST API Server Passive Socket
    SOCKET passiveSockREST = createPassiveSocketREST();

    list<shared_ptr<thread> > threads;
    for (int i = 0; i < NUM_WORKER_THREADS; ++i) {
        shared_ptr<thread> workerThread(new thread(workerThreadProc, i));
        threads.push_back(workerThread);
    }

    list<shared_ptr<thread>> slimeThreadList;
    for (int i = 0; i < NUM_MAX_SLIMES; ++i) {
        shared_ptr<thread> slimeThread(new thread(slimeThreadProc, i));
        slimeThreadList.push_back(slimeThread);
    }

    shared_ptr<thread> gameManagerThread(new thread(gameManagerThreadProc));

    while (true) {
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
        FD_SET(passiveSockREST, &readSet);
        FD_SET(passiveSockREST, &exceptionSet);
        maxSock = max(maxSock, passiveSock);
        maxSock = max(maxSock, passiveSockREST);

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
            std::cerr << "select failed: " << WSAGetLastError() << std::endl;
            break;
        } else if (r == 0)  continue;

        // passive socket 이 readable 하다면 이는 새 연결이 들어왔다는 것이다.
        if (FD_ISSET(passiveSock, &readSet)) {
            // passive socket 을 이용해 accept() 를 한다.
            // accept() 는 blocking 이지만 이미 select() 를 통해 새 연결이 있음을 알고 accept() 를 호출한다.
            // 따라서 여기서는 blocking 되지 않는다.
            // 연결이 완료되고 만들어지는 소켓은 active socket 이다.
            std::cout << "Waiting for a connection" << std::endl;
            struct sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET activeSock = accept(passiveSock, (sockaddr*)&clientAddr, &clientAddrSize);

            // accpet() 가 실패하면 해당 연결은 이루어지지 않았음을 의미한다.
            // 그 연결이 잘못된다고 하더라도 다른 연결들을 처리해야되므로 에러가 발생했다고 하더라도 계속 진행한다.
            if (activeSock == INVALID_SOCKET) {
                std::cerr << "accept failed with error " << WSAGetLastError() << std::endl;
                return 1;
            } else {
                // 새로 client 객체를 만든다.
                shared_ptr<Client> newClient(new Client(activeSock, c));

                // socket 을 key 로 하고 해당 객체 포인터를 value 로 하는 map 에 집어 넣는다.
                activeClients.insert(make_pair(activeSock, newClient));

                // 로그를 찍는다.
                char strBuf[1024];
                inet_ntop(AF_INET, &(clientAddr.sin_addr), strBuf, sizeof(strBuf));
                std::cout << "New client from " << strBuf << ":" << ntohs(clientAddr.sin_port) << ". "
                    << "Socket: " << activeSock << std::endl;
            }
        }

        // 새 REST 연결
        if (FD_ISSET(passiveSockREST, &readSet)) {
            std::cout << "Waiting for a REST connection" << std::endl;
            struct sockaddr_in clientAddr {};
            int clientAddrSize = sizeof(clientAddr);
            SOCKET activeSock = accept(passiveSockREST, (sockaddr*)&clientAddr, &clientAddrSize);

            if (activeSock == INVALID_SOCKET) {
                std::cerr << "accept failed with error " << WSAGetLastError() << std::endl;
                return 1;
            } else {
                shared_ptr<Client> newClient(new Client(activeSock, c));
                newClient->isREST = true; // REST Client임을 알린다.
                activeClients.insert(make_pair(activeSock, newClient));

                char strBuf[1024];
                inet_ntop(AF_INET, &(clientAddr.sin_addr), strBuf, sizeof(strBuf));
                std::cout << "New REST client from " << strBuf << ":" << ntohs(clientAddr.sin_port) << ". "
                    << "REST Socket: " << activeSock << std::endl;
            }
        }

        // 오류 이벤트가 발생하는 소켓의 클라이언트는 제거한다.
        // activeClients 를 순회하는 동안 그 내용을 변경하면 안되니 지우는 경우를 위해 별도로 list 를 쓴다.
        list<SOCKET> toDelete;
        for (auto& entry : activeClients) {
            SOCKET activeSock = entry.first;
            shared_ptr<Client> client = entry.second;

            if (FD_ISSET(activeSock, &exceptionSet)) {
                std::cerr << "Exception on socket " << activeSock << std::endl;

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
    for (shared_ptr<thread>& slimeThread : slimeThreadList) {
        slimeThread->join();
    }
    gameManagerThread->join();
    
    // RedisContext 정리
    redisFree(c);

    // 연결을 기다리는 passive socket 을 닫는다.
    r = closesocket(passiveSock);
    if (r == SOCKET_ERROR) {
        std::cerr << "closesocket(passive) failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // Winsock 을 정리한다.
    WSACleanup();
    return 0;
}