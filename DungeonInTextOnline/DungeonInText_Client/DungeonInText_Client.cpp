#include <hiredis/hiredis.h>
#include <iostream>
#include "rapidjson/document.h"

using namespace rapidjson;
using namespace std;

int main() {
    cout << "Client" << endl;

    redisContext* c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) {
        if (c) {
            printf("Error: %s\n", c->errstr);
            //에러 처리
        } else {
            printf("Can't allocate redis context\n");
        }
    }

    redisReply* reply = (redisReply*)redisCommand(c, "SET USER:byungmeo 1");
    if (reply->type == REDIS_REPLY_ERROR) {
        //에러처리
    }

    reply = (redisReply*)redisCommand(c, "GET USER:byungmeo");
    if (reply->type == REDIS_REPLY_STRING) {
        cout << "결과 : " << reply->str << endl;
    } else if (reply->type == REDIS_REPLY_ERROR) {
        //에러처리
    }

    freeReplyObject(reply);
    redisFree(c);
}