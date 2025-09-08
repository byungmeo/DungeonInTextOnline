#include "mylib.h"
#include <sstream>

// 특정 문자를 기준으로 문자열을 나눔
std::vector<std::string> split(std::string input, char delimiter) {
    std::vector<std::string> answer;
    std::stringstream ss(input);
    std::string temp;

    while (getline(ss, temp, delimiter)) {
        answer.push_back(temp);
    }

    return answer;
}

// 문자열 왼쪽의 공백을 제거
std::string& ltrim(std::string& s) {
    const char* t = " \t\n\r\f\v";
    s.erase(0, s.find_first_not_of(t));
    return s;
}
// 문자열 오른쪽의 공백을 제거
std::string& rtrim(std::string& s) {
    const char* t = " \t\n\r\f\v";
    s.erase(s.find_last_not_of(t) + 1);
    return s;
}
// 문자열 양쪽의 공백을 제거 
std::string& trim(std::string& s) {
    const char* t = " \t\n\r\f\v";
    return ltrim(rtrim(s));
}