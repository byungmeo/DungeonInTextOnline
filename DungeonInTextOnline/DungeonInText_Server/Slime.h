#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <WinSock2.h>

class Client;

class Slime {
public:
    int hp, x, y, str, slimeId;
    bool haveHpPotion;
    bool haveStrPotion;

    // inventory
    Slime(int slimeId);

    void attack(mutex& activeClientsMutex, const map<SOCKET, shared_ptr<Client>>& activeClients);

    int hitBy(int damage);

    bool isDie();

    bool isRange(int playerX, int playerY);
};