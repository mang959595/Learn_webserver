#include "lst_timer.h"
#include "../httprequest/http_conn.h"

// 定时器回调函数
void cb_func(client_data *user_data)
{
    // 删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    // 关闭socketfd
    close(user_data->sockfd);
    http_conn::m_user_count--;
}


void sort_timer_lst::add_timer(util_timer *timer)
{
    if(!timer)
        return ;

    // 空链表
    if(!head) 
    {
        head = tail = timer;
        return;
    }

    // 放队首的情况
    if(timer->expire < head->expire) 
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }

    // 其他情况
    add_timer(timer,head);
}

void sort_timer_lst::add_timer( util_timer*timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;

    while(tmp)
    {   
        // 寻找适合的位置插入
        if(timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }

    // 放队尾的情况
    if(!tmp)
    {
        prev->next=timer;
        timer->next=nullptr;
        timer->prev=prev;
        tail = timer;
    }
}

void sort_timer_lst::adjust_timer(util_timer* timer)
{
    if(!timer)
        return;

    util_timer *tmp = timer->next;
    // timer处在队尾 或 timer的超时时间小于下一个timer，不用改变位置
    if(!tmp || (timer->expire<tmp->expire))
    {
        return ;
    }

    // 需要改变位置：先取出timer，再重新放回去
    if(timer == head)   // 处于队头
    {
        head = head->next;      // 更新队头
        head->prev = nullptr;   
        timer->next = nullptr;
        add_timer(timer, head);
    }
    else                // 中间位置
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer,timer->next);
    }

}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if(!timer)
        return;
    
    if((timer == head)&&(timer==tail))
    {
        delete timer;
        head=nullptr;
        tail=nullptr;
        return;
    }

    if(timer == head)
    {
        head=head->next;
        head->prev=nullptr;
        delete timer;
        return;
    }

    if(timer == tail)
    {
        tail=tail->prev;
        tail->next=nullptr;
        delete timer;
        return;
    }

    timer->prev->next=timer->next;
    timer->next->prev=timer->prev;
    delete timer;
}

void sort_timer_lst::tick()
{
    if(!head)
        return ;
    
    // 当前时间
    time_t cur = time(NULL);

    util_timer *tmp = head;
    while(tmp)
    {
        if(cur<tmp->expire)
        {
            break;
        }
        // 调用回调函数关闭连接
        tmp->cb_func(tmp->user_data);
        // 删除定时器
        head = tmp->next;
        if(head)
            head->prev = nullptr;
        delete tmp;
        
        tmp=head;
    }

}


int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

// 信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响。
void  Utils::sig_handler(int sig)
{
    // 为保证信号处理函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    
    // 将信号值从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char *)&msg, 1, 0);

    // 将原来的errno赋值给当前的errno
    errno = save_errno;
}

// 注册信号sig的处理函数
void Utils::addsig(int sig, void(handler)(int),bool restart = true)
{
    // 创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;

    // SA_RESTART，使被信号打断的系统调用自动重新发起
    if(restart)
        sa.sa_flags |= SA_RESTART;

    //将所有信号添加到mask屏蔽信号集中
    sigfillset(&sa.sa_mask);

    // 注册信号sig的处理函数
    assert(sigaction(sig, &sa, NULL)!=-1);
}


void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

// 客户端连接已满的时候,发回 busy 消息并关闭
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// 设置fd非阻塞
int Utils::setnonblocking(int fd)
{
    int old_op = fcntl(fd, F_GETFL);
    int new_op = old_op | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_op);
    return old_op;
}

// 注册epoll监听事件
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if(1 == TRIGMode)
        event.events = EPOLLIN|EPOLLET|EPOLLRDHUP;
    else
        event.events = EPOLLIN|EPOLLRDHUP;

    if(one_shot)
        event.events |= EPOLLONESHOT;
    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}



