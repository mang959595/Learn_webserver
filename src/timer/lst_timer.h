#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer;

// 连接资源结构体：sockfd，sockaddress，timer
struct client_data
{
    sockaddr_in address;    
    int sockfd;
    util_timer *timer;
};

// 定时器结点
class util_timer
{
public:
    util_timer(): prev(NULL), next(NULL) {}

public:
    time_t expire;      // 超时时间

    //  超时回调函数，类型为函数指针
    void (*cb_func)(client_data *);

    client_data *user_data;
    util_timer *prev;
    util_timer *next;
};

// 用 双向升序链表 来组织定时器
class sort_timer_lst
{
public:
    sort_timer_lst(): head(nullptr),tail(nullptr) {}
    ~sort_timer_lst()
    {
        util_timer* tmp = head;
        while(tmp) 
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);

    // 关闭所有超时连接, 并删除链表中的定时器
    void tick();        

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};


// 工具类
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;           // 匿名管道fd
    sort_timer_lst m_timer_lst;     // 计时器链表
    static int u_epollfd;           // epollfd
    int m_TIMESLOT;                 // 定时任务时间间隔
};

// 超时回调函数
void cb_func(client_data *user_data);

#endif