#pragma once
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <WinSock2.h>

using namespace std;

class Client;
class Slime;

namespace JsonParser {
    string welcomeToJson(string userName);

    string rebirthToJson(string userName);

    string attackToJson(string attacker, string target, int damage);

    string attackToJson(int slimeId, string userName, int damage);

    string attackToJson(string attacker, int slimeId, int damage);

    string killLogToJson(int slimeId, string userName);

    string killLogToJson(string killer, int slimeId);

    string whisperToJson(string sender, string target, string msg);

    string positionToJson(int current_x, int current_y);

    string userListToJson(mutex& activeClientsMutex, const map<SOCKET, shared_ptr<Client>>& activeClients);

    string monsterListToJson(shared_ptr<Client> client, mutex& slimeListMutex, const list<shared_ptr<Slime>>& slimeList);

    string itemEffectToJson(string item, int effect);

    string getItemToJson(string item);

    string failCommandToJson(string msg);

    string completeAttackToJson();
}