#include "Player.h"
#include <chrono>
#include <iostream>

int Player::getStr()
{
    if (isActivatedStrBuff) {
        chrono::duration<double>sec = chrono::system_clock::now() - strBuffStartTime;
        if (sec.count() > 60.0) {
            // ������ �ߵ����� 60�ʰ� �������� ������ ���� ���� ���ݷ��� ��ȯ�Ѵ�.
            cout << "���� ����" << endl;
            isActivatedStrBuff = false;
        }
        else {
            return this->str + 3;
        }
    }

    return this->str;
}