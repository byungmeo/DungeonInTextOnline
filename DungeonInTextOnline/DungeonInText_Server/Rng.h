#pragma once
#include <random>

namespace Rng {
	// 시드값을 얻기 위한 random_device 생성.
	static std::random_device rd;
	// random_device 를 통해 난수 생성 엔진을 초기화 한다.
	static std::mt19937 gen(rd());
	// 0 부터 99 까지 균등하게 나타나는 난수열을 생성하기 위해 균등 분포 정의.
	static std::uniform_int_distribution<int> dis(0, 99);
}