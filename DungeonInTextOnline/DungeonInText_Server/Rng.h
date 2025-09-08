#pragma once
#include <random>

namespace Rng {
	// �õ尪�� ��� ���� random_device ����.
	static std::random_device rd;
	// random_device �� ���� ���� ���� ������ �ʱ�ȭ �Ѵ�.
	static std::mt19937 gen(rd());
	// 0 ���� 99 ���� �յ��ϰ� ��Ÿ���� �������� �����ϱ� ���� �յ� ���� ����.
	static std::uniform_int_distribution<int> dis(0, 99);
}