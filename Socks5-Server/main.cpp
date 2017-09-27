#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

#include <vector>
#include <string>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "Socks5Client.h"

using namespace std;

static void agent_read_callback(struct bufferevent * bev, void * ctx)
{
    socks_client *socksClient = (socks_client *)ctx;

    struct evbuffer *input = bufferevent_get_input(bev);
    size_t buffer_len = evbuffer_get_length(input);

    vector<char> buffer(buffer_len);

    size_t len = bufferevent_read(bev, buffer.data(), buffer_len);

    /* 多了一次拷贝 */
    bufferevent_write(socksClient->client, buffer.data(), len);
}

static void agent_event_callback(struct bufferevent * bev, short what, void * ctx)
{
    /* 与代理请求的服务端连接出现问题,需要断开与客户端的连接 */
    socks_client *socksClient = (socks_client *)ctx;
    if (what & BEV_EVENT_EOF)
    {
        cout << "Agent Connect Closed!" << endl;
    }
    else if (what & BEV_EVENT_ERROR)
    {
        cout << "Agent Got an error on the connection: " << evutil_socket_error_to_string(errno) << endl;
    }
    else if (what & BEV_EVENT_CONNECTED)
    {
        return;
    }
    
    bufferevent_free(bev);
    bufferevent_free(socksClient->client);
    delete socksClient;
}

static void read_client_callback(struct bufferevent * bev, void * ctx)
{
    socks_client *socksClient = (socks_client *)ctx;

    struct evbuffer *input = bufferevent_get_input(bev);
    size_t buff_len = evbuffer_get_length(input);

    vector<char> buffer(buff_len);

    size_t len = bufferevent_read(bev, buffer.data(), buff_len);

    if (len <= 0)
    {
        printf("recv data empty!\n");
        return;
    }

    if ((socksClient->state == STAGE_INIT || socksClient->state == STAGE_REQUEST_CONNECT)&& buffer[0] != 0x05)
    {
        printf("Protocol error!\n");
        return;
    }

    switch (socksClient->state)
    {
        case STAGE_INIT:
            buffer[1] = 0;
            bufferevent_write(bev, buffer.data(), 2);
            socksClient->state = STAGE_REQUEST_CONNECT;
            break;
        case STAGE_REQUEST_CONNECT:
        {
            char response[] = "\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00";

            if (buffer[1] != 0x01)
            {
                printf("not support command!\n");
                return;
            }

            struct bufferevent * agent = bufferevent_socket_new(bufferevent_get_base(bev), -1, BEV_OPT_CLOSE_ON_FREE);
            if (!agent)
            {
                printf("Agent create failed!\n");
                goto FAIL;
            }

            if (buffer[3] == 0x01)
            {
                char ipstr[16] = { 0 };
                evutil_snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", buffer[4], buffer[5], buffer[6], buffer[7]);
                struct sockaddr_in sin;
                memset(&sin, 0, sizeof(sockaddr_in));
                sin.sin_family = AF_INET;
                sin.sin_port = *((uint16_t *)(buffer.data() + 8));
                sin.sin_addr.s_addr = inet_addr(ipstr);

                if (bufferevent_socket_connect(agent, (sockaddr *)&sin, sizeof(sin)) != 0)
                {
                    bufferevent_free(agent);
                    printf("Agent connect %s:%d failed!\n", ipstr, *((uint16_t *)(buffer.data() + 8)));
                    goto FAIL;
                }
            }
            else if (buffer[3] == 0x03)
            {
                u_char domain_len = buffer[4];
                string domain(buffer.data() + 5, buffer.data() + 5 + domain_len);
                int port = *((uint16_t *)(buffer.data() + 5 + domain_len));
                if (0 != bufferevent_socket_connect_hostname(agent, NULL, AF_INET, domain.c_str(), ntohs(port)))
                {
                    bufferevent_free(agent);
                    printf("Agent connect %s:%d failed!\n", domain.c_str(), port);
                    goto FAIL;
                }
            }
            else
            {
                bufferevent_free(agent);
                printf("not support ip protocol!\n");
                goto FAIL;
            }
            /* connect success */
            socksClient->proxy = agent;
            socksClient->state = STAGE_CONNECTED;
            bufferevent_setcb(agent, agent_read_callback, NULL, agent_event_callback, ctx);
            bufferevent_enable(agent, EV_READ | EV_WRITE);

            bufferevent_write(bev, response, sizeof(response) - 1);
            return;
        FAIL:
            response[1] = 0x05;
            bufferevent_write(bev, response, sizeof(response) - 1);
        }
            break;
        case STAGE_CONNECTED:
            bufferevent_write(socksClient->proxy, buffer.data(), len);
            break;
        default:
            break;
    }
}

static void event_callback(struct bufferevent * bev, short what, void * ctx)
{
    /* 客户端连接出现问题,需要断开与代理请求服务端的连接 */
    socks_client *socksClient = (socks_client *)ctx;
    if (what & BEV_EVENT_EOF)
    {
        cout << "Connect Closed!" << endl;
    }
    else if (what & BEV_EVENT_ERROR)
    {
        cout << "Got an error on the connection: " << evutil_socket_error_to_string(errno) << endl;
    }

    bufferevent_free(bev);
    bufferevent_free(socksClient->proxy);
    delete socksClient;
}

static void listener_callback(struct evconnlistener * listener, evutil_socket_t fd,
                              struct sockaddr * sa, int socklen, void * user_data)
{
    struct event_base *base = (struct event_base *)user_data;
    struct bufferevent *bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev)
    {
        cout << "Could not create bufferevent!" << endl;
        event_base_loopbreak(base);
        return;
    }

    socks_client * socksClient = new socks_client;
    socksClient->state = STAGE_INIT;
    socksClient->client = bev;

    bufferevent_setcb(bev, read_client_callback, NULL, event_callback, (void *)socksClient);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

int main()
{
    /* initialize main eventLoop, listening to client connect */
    struct event_base *base = event_base_new();
    if (!base)
    {
        cout << "Could not initialize libevent!" << endl;
        return 1;
    }

    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(sockaddr_in));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(11080);
    sin.sin_addr.s_addr = INADDR_ANY;

    struct evconnlistener *listener = evconnlistener_new_bind(base, listener_callback, (void *)base,
                                                              LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
                                                              -1, (sockaddr *)&sin, sizeof(sin));
    if (!listener)
    {
        cout << "Could not create a listener! " << evutil_socket_error_to_string(errno) << endl;
        return 1;
    }

    event_base_dispatch(base);
    evconnlistener_free(listener);
    event_base_free(base);
    return 0;
}