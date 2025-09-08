#pragma once
#include <memory>
#include <string>
#include <WinSock2.h>

using namespace std;

class Client;

static const int BUFFER_SIZE = 8192;

SOCKET CreatePassiveSocket();
bool SendMsg(shared_ptr<Client> client, string message);
bool RecvLength(shared_ptr<Client> client);
bool RecvPacket(shared_ptr<Client> client);