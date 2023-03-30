#include "webserver.h"

WebServer::WebServer()
{
    // http连接对象数组
    users = new http_conn[MAX_FD];

    // /root路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器对象数组
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName,
                     int log_write, int opt_linger, int trigmode, int sql_num,
                     int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

//  epoll触发模式
void WebServer::trig_mode()
{
    if (m_TRIGMode == 0) // listenfd: LT;    connfd: LT
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    else if (m_TRIGMode == 1)   // LT + ET
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    else if (2 == m_TRIGMode)   // ET + LT
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    else if (3 == m_TRIGMode)   // ET + ET
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

// 日志
void WebServer::log_write()
{
    if(m_close_log == 0)
    {
        // 初始化日志
        if(m_log_write == 1)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

// 数据库
void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

// 线程池
void WebServer::thread_pool()
{
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// 创建listenfd, 启动监听, 以及其他相关设置
void WebServer::eventListen()
{
    // 创建监听fd
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd>=0);

    // SO_LINGER 优雅关闭连接
    if( m_OPT_LINGER == 0)
    {
        struct linger tmp = {0,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }
    else if( m_OPT_LINGER == 1)
    {
        struct linger tmp = {1,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    // SO_REUSEADDR 端口复用? SO_REUSEADDR是一个很有用的选项，一般服务器的监听socket都应该打开它。
    // 它的大意是允许服务器bind一个地址，即使这个地址当前已经存在已建立的连接
    int flag = 1;
    setsockopt(m_listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));

    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret>=0);
    ret = listen(m_listenfd,5);
    assert(ret>=0);

    // 工具对象
    utils.init(TIMESLOT);

    // epoll事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd!=-1);
    // 为监听socket注册epoll读事件
    utils.addfd(m_epollfd,m_listenfd,false,m_LISTENTrigmode);   // 只用在主线程的socket不需要one_shot
    http_conn::m_epollfd = m_epollfd;
    
    // 创建管道, 并注册epoll读事件
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret!=-1);
    utils.setnonblocking(m_pipefd[1]);                          // 非阻塞的写端, alarm的定时事件就是往管道里面写入时钟信号或者异常信号
    utils.addfd(m_epollfd,m_pipefd[0],false,0);                 // 只用在主线程的socket不需要one_shot

    // 注册信号处理函数
    utils.addsig(SIGPIPE, SIG_IGN);         // 对端connfd关闭后, 再往里面写会产生的信号, 默认行为是终止程序; SIG_IGN 表示交给系统处理
    utils.addsig(SIGALRM, utils.sig_handler, false);    // 时钟信号
    utils.addsig(SIGTERM, utils.sig_handler, false);    // kill

    // 开启定时信号
    alarm(TIMESLOT);

    Utils::u_epollfd = m_epollfd;
    Utils::u_pipefd = m_pipefd;
}

// 初始化http连接对象的同时设置定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    // 初始化http连接对象
    users[connfd].init(connfd,client_address, m_root, m_CONNTrigmode, m_close_log,m_user,m_passWord,m_databaseName);

    // 初始化定时器,包括conn数据,回调函数和超时时间
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3*TIMESLOT;
    users_timer[connfd].timer = timer;
    // 将定时器加入链表
    utils.m_timer_lst.add_timer(timer);
}

// 调整定时器, 计时往后延3个单位; 并调整定时器在链表中的位置
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);

    // 定时时间重置为 cur + 3*TIMESLOT
    timer->expire = cur +3*TIMESLOT;
    // 调整链表
    utils.m_timer_lst.add_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

// 主线程处理 定时器超时 事件
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    // 删除connfd的epoll事件并close关闭connfd连接
    timer->cb_func(&users_timer[sockfd]);
    if(timer)
    {   // 释放timer并调整链表
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);   
}

// 主线程处理listenfd读事件
bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    if( m_LISTENTrigmode == 0 ) // listenfd LT, 不用一次读完
    {
        int connfd = accept(m_listenfd,(struct sockaddr *)&client_address, &client_addrlength);
        // error
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d","acceept error", errno);
            return false;
        }
        // 连接数已满
        if(http_conn::m_user_count>=MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s","Internal server busy");
            return false;
        }

        timer(connfd, client_address);
    }
    // listenfd ET, 要确保读完
    else
    {
        while(1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d","acceept error", errno);
                break;
            }
            if(http_conn::m_user_count>=MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s","Internal server busy");
                break;
            }

            timer(connfd,client_address);
        }
        return false ;
    }

    return true;
}

// 主线程处理管道传来的信号
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024]; // 管道中的数据, 这里一般是信号值
    ret = recv(m_pipefd[0],signals, sizeof(signals), 0);    
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for(int i=0;i<ret;i++)
        {
            switch (signals[i]) // char
            {   //      int
                case SIGALRM:       // 发生定时事件 (alarm的SIGALRM信号注册的回调函数是 sig_handler ,里面是给管道的写端 send(pipefd[1], 信号值))
                {
                    timeout = true; // 发生定时事件
                    break;
                }
                case SIGTERM:       // 程序终止信号
                {
                    stop_server = true; // 关闭服务器
                }
            }
        }
    }
    return true;
}

// 主线程处理 connfd 读事件
void WebServer::dealwithread(int sockfd)
{
    // 可能需要重置该connfd对应的定时器
    util_timer *timer = users_timer[sockfd].timer;

    // reactor  主线程不需要做I/O工作,只是往线程池的请求队列里面添加一个请求,由子线程竞争获取请求后做I/O操作
    if(m_actormodel == 1)
    {
        if(timer)
        {   // 更新定时器
            adjust_timer(timer);
        }
        // 添加请求
        m_pool->append(users+sockfd, 0);

        // 这素?
        while(true)
        {
            if(users[sockfd].improv == 1)
            {
                if(users[sockfd].timer_flag == 1)
                {
                    // 关闭连接
                    deal_timer(timer,sockfd);
                    users[sockfd].timer_flag = 0 ;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    // 模拟Procator  主线程需要做I/O工作,准备好数据之后,往线程池的请求队列里面添加一个请求,由子线程竞争获取请求后对数据进行加工处理
    else
    {
        // 成功读到数据
        if(users[sockfd].read_once())
        {
            if(timer)
            {   // 更新定时器
                adjust_timer(timer);
            }

            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            m_pool->append(users+sockfd, 0);
        }
        // 读取失败, 删除epoll事件, 关闭连接
        {
            deal_timer(timer,sockfd);
        }
    }
}

// 主线程处理 connfd 写事件                     
// 子线程生成响应报文并设置好缓冲区数据后(process_read), 注册epoll写事件, 待下一步将数据搬到socket缓冲区发送(Reactor子线程来搬, Proacotr主线程来搬) 
void WebServer::dealwithwrite(int sockfd)
{
    // 可能需要更新定时器, 或销毁定时器
    util_timer *timer = users_timer[sockfd].timer;

    // Reactor
    if (m_actormodel == 1)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    // Proactor
    else
    {
        if (users[sockfd].write())
        {
            if (timer)
            {
                adjust_timer(timer);
            }
            
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}


// 服务器主线程的事件循环
void WebServer::eventLoop()
{
    bool timeout = false;       // 超时事件
    bool stop_server = false;   // 服务器停止运行

    while(!stop_server)
    {
        // 获取epoll事件
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if(number<0 && errno != EINTR)  // 若epoll_wait阻塞过程中被中断, 中断结束后不再阻塞, 返回 -1 和errno EINTR
        {
            // 这里是非EINTR的情况, 即出错
            LOG_ERROR("%s","epoll failure");
            break;
        }

        for(int i=0;i<number;i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == m_listenfd)   // listenfd的读事件, 即accept可用, 接收新的连接
            {
                bool flag = dealclinetdata();
                if (false == flag)
                    continue;
            }
            else if(events[i].events & (EPOLLRDHUP|EPOLLHUP|EPOLLERR))  // EPOLLRDHUP 和 EPOLLHUP 是socket关闭事件, 
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer,sockfd);
            }
            else if((sockfd == m_pipefd[0])&&(events[i].events & EPOLLIN))  // 处理管道中的信号
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if(flag==false)
                    LOG_ERROR("%s", "deal signal failure");
            }
            else if (events[i].events&EPOLLIN)      // 处理 connfd 的读事件
            {
                dealwithread(sockfd);
            }
            else if (events[i].events&EPOLLOUT)     // 处理 connfd 的写事件
            {
                dealwithwrite(sockfd);
            }
        }

        // 处理定时器为非必须事件，收到信号并不是立马处理
        // 处理完epoll监听的socket事件之后, 再根据timeout值,判断是否有超时的连接需要关闭
        if(timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}


