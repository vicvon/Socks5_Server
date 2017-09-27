#pragma once
#include <event2/bufferevent.h>

enum SOCKS_STATE
{
    STAGE_INIT,
    STAGE_REQUEST_CONNECT,
    STAGE_CONNECTED
};

struct socks_client
{
    SOCKS_STATE state;
    struct bufferevent * client;
    struct bufferevent * proxy;
};