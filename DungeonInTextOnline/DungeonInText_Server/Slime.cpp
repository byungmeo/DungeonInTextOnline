#include "Client.h"
#include "JsonParser.h"
#include "Player.h"
#include "Rng.h"
#include "ServerCore.h"
#include "Slime.h"
#include <cmath>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <WinSock2.h>

Slime::Slime(int slimeId) : slimeId(slimeId) {
    this->hp = Rng::dis(Rng::gen) % 6 + 5; // 5 ~ 10
    this->x = Rng::dis(Rng::gen) % 31; // 0 ~ 30
    this->y = Rng::dis(Rng::gen) % 31; // 0 ~ 30
    this->str = Rng::dis(Rng::gen) % 3 + 3; // 3 ~ 5

    // HP 포션 또는 STR 포션을 갖고 태어난다
    if (Rng::dis(Rng::gen) % 2 == 0) {
        haveHpPotion = true;
        haveStrPotion = false;
    }
    else {
        haveHpPotion = false;
        haveStrPotion = true;
    }
}

void Slime::attack(mutex& activeClientsMutex, const map<SOCKET, shared_ptr<Client>>& activeClients)
{
    list<shared_ptr<Player>> toDie;
    {
        unique_lock<mutex> ul(activeClientsMutex);
        for (auto& pair : activeClients) {
            shared_ptr<Client> client = pair.second;
            {
                unique_lock<mutex> ul(client->playerInfoMutex);
                shared_ptr<Player> player = client->playerInfo;
                if (!player->isDie() && player->isRange(x, y)) {
                    int damage = player->hitBy(str);
                    if (player->isDie()) toDie.push_back(player);
                    for (auto& pair : activeClients) {
                        SendMsg(pair.second, JsonParser::attackToJson(this->slimeId, player->name, damage));
                    }
                }
            }
        }
    }

    for (shared_ptr<Player> player : toDie) {
        {
            unique_lock<mutex> ul(activeClientsMutex);
            for (auto& pair : activeClients) {
                SendMsg(pair.second, JsonParser::killLogToJson(slimeId, player->name));
            }
        }
        cout << player->name << " 님이 슬라임에게 맞아 쓰러졌습니다.." << endl;

        player->rebirth(activeClientsMutex, activeClients);
        cout << player->name << " 님이 다시 깨어났습니다.." << endl;
    }
}

int Slime::hitBy(int damage) {
    this->hp -= damage;
    if (hp < 0) return damage + hp;
    else return damage;
}

bool Slime::isDie() {
    return (this->hp <= 0);
}

bool Slime::isRange(int playerX, int playerY) {
    return (abs(x - playerX) <= 1 && abs(y - playerY) <= 1);
}