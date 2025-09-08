#include "Player.h"
#include <chrono>
#include <iostream>

int Player::getStr()
{
    if (isActivatedStrBuff) {
        chrono::duration<double>sec = chrono::system_clock::now() - strBuffStartTime;
        if (sec.count() > 60.0) {
            // 버프가 발동된지 60초가 지났으면 버프를 끄고 원래 공격력을 반환한다.
            cout << "버프 끝남" << endl;
            isActivatedStrBuff = false;
        }
        else {
            return this->str + 3;
        }
    }

    return this->str;
}