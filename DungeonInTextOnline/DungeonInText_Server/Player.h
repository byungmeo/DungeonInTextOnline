#pragma once
#include "Client.h"
#include "JsonParser.h"
#include "Rng.h"
#include "ServerCore.h"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using namespace std;

class Player {
public:
    string name;
    int hp, x, y, str;
    map<string, int> inventory;

    bool isActivatedStrBuff;
    chrono::system_clock::time_point strBuffStartTime;

    int getStr();

    int hitBy(int damage) {
        this->hp -= damage;
        if (hp < 0) return damage + hp;
        else return damage;
    }

    bool isDie() {
        return (this->hp <= 0);
    }

    bool isRange(int slimeX, int slimeY) {
        return (abs(x - slimeX) <= 1 && abs(y - slimeY) <= 1);
    }

    void move(int x, int y) {
        this->x += x;
        this->y += y;
        if (this->x < 0) this->x = 0;
        else if (this->x > 30) this->x = 30;

        if (this->y < 0) this->y = 0;
        else if (this->y > 30) this->y = 30;
    }

    void getItem(string item) {
        inventory[item]++;
    }

    int useItem(string item) {
        if (item.compare("hp") == 0) {
            if (inventory["hp"] > 0) {
                inventory["hp"]--;
                this->hp += 10;
                return 10;
            }
        }
        else if (item.compare("str") == 0) {
            if (inventory["str"] > 0) {
                if (isActivatedStrBuff) {
                    chrono::duration<double>sec = chrono::system_clock::now() - strBuffStartTime;
                    if (sec.count() <= 60.0) {
                        // 버프가 아직 유효하면 사용하지 않는다
                        return -1;
                    }
                }
                this->strBuffStartTime = chrono::system_clock::now();
                isActivatedStrBuff = true;
                inventory["str"]--;
                return 3;
            }
        }
        else {
            return -1;
        }

        return 0;
    }

    void rebirth(mutex& activeClientsMutex, const map<SOCKET, shared_ptr<Client>>& activeClients) {
        this->hp = 30; // 기본값 30
        this->x = Rng::dis(Rng::gen) % 31; // 0 ~ 30
        this->y = Rng::dis(Rng::gen) % 31; // 0 ~ 30
        this->str = 3; // 기본값 3
        this->inventory["hp"] = 1;

        unique_lock<mutex> ul(activeClientsMutex);
        for (auto& pair : activeClients) {
            SendMsg(pair.second, JsonParser::rebirthToJson(this->name));
        }
    }
};