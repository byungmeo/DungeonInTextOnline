#include "Client.h"
#include "ServerCore.h"
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <WinSock2.h>

static unsigned short SERVER_PORT = 27015;

SOCKET CreatePassiveSocket() {
    // TCP socket �� �����.
    SOCKET passiveSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (passiveSock == INVALID_SOCKET) {
        std::cerr << "socket failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // socket �� Ư�� �ּ�, ��Ʈ�� ���ε� �Ѵ�.
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int r = bind(passiveSock, (sockaddr*)&serverAddr, sizeof(serverAddr));
    if (r == SOCKET_ERROR) {
        std::cerr << "bind failed with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    // TCP �� ������ �޴� passive socket �� ���� ����� �� �� �ִ� active socket ���� ���еȴ�.
    // passive socket �� socket() �ڿ� listen() �� ȣ�������ν� ���������.
    // active socket �� passive socket �� �̿��� accept() �� ȣ�������ν� ���������.
    r = listen(passiveSock, 10);
    if (r == SOCKET_ERROR) {
        std::cerr << "listen faijled with error " << WSAGetLastError() << std::endl;
        return 1;
    }

    return passiveSock;
}

bool SendMsg(shared_ptr<Client> client, string message)
{
    // ���� �޽����� �������� ����� REST Client�� ��� ������ �ʽ��ϴ�.
    if (client->isREST) return true;

    SOCKET activeSock = client->sock;
    int r;

    string data = message;
    int dataLen = data.length() + 1; // ���ڿ��� ���� �ǹ��ϴ� NULL ���� ����

    // ���̸� ���� ������.
    // binary �� 4bytes �� ���̷� encoding �Ѵ�.
    // �� �� network byte order �� ��ȯ�ϱ� ���ؼ� htonl �� ȣ���ؾߵȴ�.
    int dataLenNetByteOrder = htonl(dataLen);
    int offset = 0;
    while (offset < 4) {
        {
            unique_lock<mutex> ul(client->socketMutex);
            r = send(activeSock, ((char*)&dataLenNetByteOrder) + offset, 4 - offset, 0);
        }
        if (r == SOCKET_ERROR) {
            std::cerr << "failed to send length: " << WSAGetLastError() << std::endl;
            return false;
        }
        offset += r;
    }
    //std::cout << "Sent length info: " << dataLen << std::endl;

    // send �� ��ɾ ������.
    offset = 0;
    while (offset < dataLen) {
        {
            unique_lock<mutex> ul(client->socketMutex);
            r = send(activeSock, data.c_str() + offset, dataLen - offset, 0);
        }
        if (r == SOCKET_ERROR) {
            std::cerr << "send failed with error " << WSAGetLastError() << std::endl;
            return false;
        }
        //std::cout << "Sent " << r << " bytes" << std::endl;
        offset += r;
    }

    return true;
}

bool RecvLength(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;
    int r;

    // ������ ������ �۾��ߴ����� ���� �ٸ��� ó���Ѵ�.
    // ������ packetLen �� �ϼ����� ���ߴ�. �װ� �ϼ��ϰ� �Ѵ�.
    if (client->lenCompleted == false) {
        // ���� ������ �ޱ� ���ؼ� 4����Ʈ�� �д´�.
        // network byte order �� �����Ǳ� ������ ntohl() �� ȣ���Ѵ�.
        {
            unique_lock<mutex> ul(client->socketMutex);
            r = recv(activeSock, (char*)&(client->packetLen) + client->offset, 4 - client->offset, 0);
        }
        if (r == SOCKET_ERROR) {
            std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
            return false;
        }
        else if (r == 0) {
            // �޴����� ���� recv() �� ������ ���� ��� 0 �� ��ȯ���� �� �� �ִ�.
            // ���� r == 0 �� ��쵵 loop �� Ż���ϰ� �ؾߵȴ�.
            std::cerr << "Socket closed: " << activeSock << std::endl;
            return false;
        }
        client->offset += r;

        // �ϼ� ���ߴٸ� �������� ��� �õ��� ���̴�.
        if (client->offset < 4) {
            return true;
        }

        // network byte order �� �����߾���.
        // ���� �̸� host byte order �� �����Ѵ�.
        int dataLen = ntohl(client->packetLen);
        //std::cout << "[" << activeSock << "] Received length info: " << dataLen << std::endl;
        client->packetLen = dataLen;

        // �츮�� Client class �ȿ� packet ���̸� �ִ� 64KB �� �����ϰ� �ִ�.
        // Ȥ�� �츮�� ���� �����Ͱ� �̰ͺ��� ū�� Ȯ���Ѵ�.
        // ���� ũ�ٸ� ó�� �Ұ����̹Ƿ� ������ ó���Ѵ�.
        if (client->packetLen > sizeof(client->packet)) {
            std::cerr << "[" << activeSock << "] Too big data: " << client->packetLen << std::endl;
            return false;
        }

        // ���� packetLen �� �ϼ��ߴٰ� ����ϰ� offset �� �ʱ�ȭ���ش�.
        client->lenCompleted = true;
        client->offset = 0;
    }

    return true;
}

bool RecvPacket(shared_ptr<Client> client) {
    SOCKET activeSock = client->sock;
    int r;

    {
        unique_lock<mutex> ul(client->socketMutex);
        r = recv(activeSock, client->packet + client->offset, client->packetLen - client->offset, 0);
    }
    if (r == SOCKET_ERROR) {
        std::cerr << "recv failed with error " << WSAGetLastError() << std::endl;
        return false;
    }
    else if (r == 0) {
        // �޴����� ���� recv() �� ������ ���� ��� 0 �� ��ȯ���� �� �� �ִ�.
        // ���� r == 0 �� ��쵵 loop �� Ż���ϰ� �ؾߵȴ�.
        return false;
    }
    client->offset += r;

    // �ϼ��� ���� partial recv �� ��츦 �����ؼ� �α׸� ��´�.
    if (client->offset == client->packetLen) {
        //std::cout << "[" << activeSock << "] Received " << client->packetLen << " bytes" << std::endl;

        // ���� ��Ŷ�� ���� ��Ŷ ���� ������ �ʱ�ȭ�Ѵ�.
        client->lenCompleted = false;
        client->offset = 0;
        client->packetLen = 0;
    }
    else {
        //std::cout << "[" << activeSock << "] Partial recv " << r << "bytes. " << client->offset << "/" << client->packetLen << std::endl;
    }

    return true;
}