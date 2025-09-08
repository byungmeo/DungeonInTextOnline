#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "Client.h"
#include "JsonParser.h"
#include "Player.h"
#include "ServerCore.h"
#include "Slime.h"
#include <cstdio>
#include <iostream>
#include <list>
#include <string>

using namespace rapidjson;

string JsonParser::welcomeToJson(string userName)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData), "{\"tag\": \"notice\", \"msg\": \"[ 시스템 ] : [ %s ] 님이 게임에 접속하였습니다.\"}", userName.c_str());
    return jsonData;
}

string JsonParser::rebirthToJson(string userName)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData), "{\"tag\": \"notice\", \"msg\": \"[ 시스템 ] : [ %s ] 님이 기운을 차렸습니다. 여긴 어디..?\"}", userName.c_str());
    return jsonData;
}

string JsonParser::attackToJson(string attacker, string target, int damage)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData),
        "{\"tag\": \"damage\", \"attacker\": \"%s\", \"target\": \"%s\", \"damage\": %d}",
        attacker.c_str(), target.c_str(), damage);
    return jsonData;
}

string JsonParser::attackToJson(int slimeId, string userName, int damage)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData),
        "{\"tag\": \"damage\", \"attacker\": \"슬라임(%d)\", \"target\": \"%s\", \"damage\": %d}",
        slimeId, userName.c_str(), damage);
    return jsonData;
}

string JsonParser::attackToJson(string attacker, int slimeId, int damage)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData),
        "{\"tag\": \"damage\", \"attacker\": \"%s\", \"target\": \"슬라임(%d)\", \"damage\": %d}",
        attacker.c_str(), slimeId, damage);
    return jsonData;
}

string JsonParser::killLogToJson(int slimeId, string userName)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData),
        "{\"tag\": \"killLog\", \"killer\": \"슬라임(%d)\", \"killed\": \"%s\"}",
        slimeId, userName.c_str());
    return jsonData;
}

string JsonParser::killLogToJson(string killer, int slimeId)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData),
        "{\"tag\": \"killLog\", \"killer\": \"%s\", \"killed\": \"슬라임(%d)\"}",
        killer.c_str(), slimeId);
    return jsonData;
}

string JsonParser::whisperToJson(string sender, string target, string msg)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData),
        "{\"tag\": \"whisper\", \"sender\": \"%s\", \"target\": \"%s\", \"msg\": \"%s\"}",
        sender.c_str(), target.c_str(), msg.c_str());
    return jsonData;
}

string JsonParser::positionToJson(int current_x, int current_y)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData), "{\"tag\": \"position\", \"x\": %d, \"y\": %d}", current_x, current_y);
    return jsonData;
}

string JsonParser::userListToJson(mutex& activeClientsMutex, const map<SOCKET, shared_ptr<Client>>& activeClients)
{
    Document d(kObjectType);
    Value v(kArrayType);
    {
        unique_lock<mutex> ul(activeClientsMutex);
        for (auto& pair : activeClients) {
            string name;
            int x, y, hp;
            {
                unique_lock<mutex> ul(pair.second->playerInfoMutex);
                name = pair.second->playerInfo->name;
                x = pair.second->playerInfo->x;
                y = pair.second->playerInfo->y;
                hp = pair.second->playerInfo->hp;
            }

            char temp[BUFFER_SIZE];
            sprintf_s(temp, sizeof(temp), "name\t: %s\n(x, y)\t: (%d, %d)\nHP\t: %d", name.c_str(), x, y, hp);
            string info = temp;

            v.PushBack(
                Value().SetString(info.c_str(), info.length(), d.GetAllocator()),
                d.GetAllocator()
            );
        }
    }

    d.AddMember("tag", "userList", d.GetAllocator());
    d.AddMember("userList", v, d.GetAllocator());

    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    std::cout << buffer.GetString() << std::endl;
    return buffer.GetString();
}

string JsonParser::monsterListToJson(shared_ptr<Client> client, mutex& slimeListMutex, const list<shared_ptr<Slime>>& slimeList)
{
    string attackable;
    int x, y;
    {
        unique_lock<mutex> ul(client->playerInfoMutex);
        x = client->playerInfo->x;
        y = client->playerInfo->y;

    }

    Document d(kObjectType);
    Value v(kArrayType);
    {
        unique_lock<mutex> ul(slimeListMutex);
        for (shared_ptr<Slime> slime : slimeList) {
            std::cout << "슬라임(" << slime->slimeId << ") 정보" << std::endl;
            std::cout << "HP : " << slime->hp << std::endl;
            std::cout << "위치 : (" << slime->x << ", " << slime->y << ")" << std::endl;
            std::cout << "공격력 : " << slime->str << std::endl;
            attackable = (slime->isRange(x, y)) ? "Attackable!" : "Out Of Range";
            char temp[BUFFER_SIZE];
            sprintf_s(temp, sizeof(temp), "Slime%d(hp : %d)\n(x, y)\t: (%d, %d) -> %s", slime->slimeId, slime->hp, slime->x, slime->y, attackable.c_str());
            string info = temp;

            v.PushBack(
                Value().SetString(info.c_str(), info.length(), d.GetAllocator()),
                d.GetAllocator()
            );
        }
    }

    d.AddMember("tag", "monsterList", d.GetAllocator());
    d.AddMember("monsterList", v, d.GetAllocator());

    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    d.Accept(writer);
    std::cout << buffer.GetString() << std::endl;
    return buffer.GetString();
}

string JsonParser::itemEffectToJson(string item, int effect)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData), "{\"tag\": \"itemEffect\", \"item\": \"%s\", \"effect\": %d}", item.c_str(), effect);
    return jsonData;
}

string JsonParser::getItemToJson(string item)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData), "{\"tag\": \"getItem\", \"item\": \"%s\"}", item.c_str());
    return jsonData;
}

string JsonParser::failCommandToJson(string msg)
{
    char jsonData[BUFFER_SIZE];
    sprintf_s(jsonData, sizeof(jsonData), "{\"tag\": \"fail\", \"msg\": \"%s\"}", msg.c_str());
    return jsonData;
}

string JsonParser::completeAttackToJson()
{
    string jsonData = "{\"tag\": \"attack\", \"msg\": \"Success Attack!\"}";
    return jsonData;
}
