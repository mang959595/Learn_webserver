#include "http_conn.h"
#include <mysql/mysql.h>
#include <fstream>

// http响应中的状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock; // mutex
map<string, string> users;

// 载入数据库表的用户名密码数据到内存
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 设置fd为非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// epoll设置相关
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) // 在内核事件表中注册读事件，ET模式，选择开启EPOLLONESHOT
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}
void removefd(int epollfd, int fd) // 从内核事件表删除描述符
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
void modfd(int epollfd, int fd, int ev, int TRIGMode) // 将事件重置为EPOLLONESHOT
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 类共享的 用户数 和 epollfd
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd); // epoll 不再监听这个socket的事件
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接, 外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接受的连接
// check_state 默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 线程run()中运行的
void http_conn::process()
{
    // 解析本地读缓存区中的数据
    HTTP_CODE read_ret = process_read();

    // NO_REQUEST，表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    // 生成响应报文到本地缓冲区，创建文件的内存映射
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }

    // 当socket的写缓冲区从不可写变为可写，触发epollout
    // epoll 注册本 socket 的写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

// 循环读取客户数据, socket读缓冲 >> 本地读缓冲，直到无数据可读或对方关闭连接
bool http_conn::read_once()
{
    // 本地读缓冲放不下了
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET读数据 非阻塞ET工作模式下，需要一次性将数据读完
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                // 数据读完了，返回-1 非阻塞的 errno 会被置为 EAGAIN or EWOULDBLOCK
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;

                return false;
            }
            else if (bytes_read == 0) // 连接已关闭
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}


/* =============== 解析报文相关 ================ */

// 从状态机，用于读取buffer中一行的内容，并把行之间的'\r''\n'换为'\0''\0'
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // 循环读到行尾或buffer末端
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {

        // 逐个char分析，temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];

        // 当前字符为 '\r'
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx) // '\r'已到达buffer末尾，说明下一个字符'\n'还没被接收，即接受不完整
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n') // 下一个字符为'\n'，说明读到行尾
            {
                // 每行之间的'\r','\n' 换成 '\0','\0' ，方便后续操作
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 除上面外都是格式不对
            return LINE_BAD;
        }
        // 当前字符为 '\n'
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') // 前一个字符为'\r'，说明读到行尾
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    // 读到buffer末尾也没有找到\r\n，需要继续recv到buffer
    return LINE_OPEN;
}

// 主状态机，解析从状态机处理后的buffer中的每一个请求行
// 返回值为请求的解析状态，有NO_REQUEST,GET_REQUEST，BAD_REQUEST 等
http_conn::HTTP_CODE http_conn::process_read()
{
    // 初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // while 条件中 || 前面的是针对解析 POST 请求的Content
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) 
    {
        text = get_line(); // { return m_read_buf + m_start_line; }
                           // m_start_line 是行在buffer中的起始位置，将该位置后面的数据赋给text

        m_start_line = m_checked_idx; // m_checked_idx 在从状态机中会移到每行的行首位置

        LOG_INFO("%s", text); // 日志信息

        // 主状态机的状态转移和运行逻辑
        switch (m_check_state)  // 初始为CHECK_STATE_REQUESTLINE
        {
        case CHECK_STATE_REQUESTLINE:
        {
            // 解析请求行
            ret = parse_request_line(text); 
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {   
            // 解析请求头
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // 完整解析GET请求后，跳转到报文响应函数
            else if (ret == GET_REQUEST)
                return do_request();

            break;
        }
        case CHECK_STATE_CONTENT:
        {
            // 解析消息体，只有 POST 请求需要
            ret = parse_content(text);

            // 完整解析POST请求后，跳转到报文响应函数
            if (ret == GET_REQUEST)
                return do_request();

            // ret!=GET_REQUEST说明还没全部解析完成
            // 更新 line_status 为 LINE_OPEN ，还有数据要读，并跳出循环
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }

    return NO_REQUEST;
}

// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{ // 在HTTP报文中，请求行用来说明请求类型，要访问的资源以及所使用的HTTP版本，其中各个部分之间通过 \t 或 空格 分隔。

    m_url = strpbrk(text, " \t"); // strpbrk: 检索字符串 text 中第一个匹配字符串 " \t" 的字符(' ' 或 '\t')的位置

    // 如果没有空格或\t，则报文格式有误
    if (!m_url)
    {
        return BAD_REQUEST;
    }

    // 将该位置的' '或'\t'改为\0，用于将前面数据取出
    *m_url++ = '\0';

    // 取出数据，并通过与GET和POST比较，以确定请求方式
    char* method = text;
    if(strcasecmp(method, "GET")==0)
        m_method = GET;
    else if(strcasecmp(method,"POST")==0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    // m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    // 将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url," \t");  // strspn: 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标

    // 类似上面获取m_url的操作,获取m_version
    m_version = strpbrk(m_url," \t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    // 仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    
    // 这里主要是有些报文的请求资源中会带有http:// or https://，这里需要对这种情况进行单独处理
    // 对请求资源前7个字符进行判断，http
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');         // strchr：用于查找字符串中的一个字符，并返回该字符在字符串中第一次出现的位置。
    }
    // 对请求资源前8个字符进行判断，https
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    
    //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 状态转移为 CHECK_STATE_HEADER
    m_check_state = CHECK_STATE_HEADER;
    
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')    // 当前是空行
    {
        if (m_content_length != 0)  // content 有内容，说明是 POST 请求
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;         // content 无内容，说明是 GET 则解析完成
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) // connction 字段
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)    // 决定是长连接还是短连接
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0) // content-length 字段
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)    // host 字段
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);    // 其他的放到日志输出
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/* ============================================ */

//网站根目录，文件夹内存放请求的资源和跳转的html文件
const char* doc_root="/root/CPP_Projects/webserver/Learn_webserver/root";

// 处理解析内容，设置实际文件路径，创建mmap内存映射；处理cgi
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    
    printf("m_url:%s\n", m_url);

    const char *p = strrchr(m_url, '/');     // 最后一次出现字符 '/' 的位置

    // 处理cgi，登录和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123 & passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 注册
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }


    // 请求资源为 /0 ，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        // 把 "/register.html" 拼接到 m_real_file
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为/5，表示跳转图片界面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为/6，表示跳转视频界面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果请求资源为/7，表示跳转关注界面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 如果以上均不符合，即不是登录和注册和其他的，直接将url与网站目录拼接
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);


    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 文件权限不满足
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 文件类型是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    /*
        start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址
        PROT_READ 表示页内容可以被读取
        MAP_PRIVATE 建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
    */
    
    // 避免文件描述符的浪费和占用
    close(fd);

    // 表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

// 解除内存映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

/* ============================================ */

// do_reques() 已经把请求的文件映射到了用户内存，下面就把 响应报文 写到本地的写buffer，并设置好 iovec 分段缓冲区

// 把响应报文写到本地写buffer，并根据情况，把本地写buffer(和 mmap 内存映射区)放到iovec
bool http_conn::process_write(HTTP_CODE ret)
{
    // 根据 报文解析 和 do_request 得到的HTTP_CODE状态分别响应不同内容
    switch (ret)
    {
    case INTERNAL_ERROR: // 报文语法有误，404
    {
        add_status_line(500, error_500_title);  // 状态行
        add_headers(strlen(error_500_form));    // 内容长度、连接模式、空行
        if (!add_content(error_500_form))       // 内容
            return false;
        break;
    }
    case BAD_REQUEST:   // 报文语法有误，404
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST: // 资源没有访问权限，403
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST: // 文件存在，200
    {
        add_status_line(200, ok_200_title);

        // 如果请求的资源大小不为0，即文件存在，则要返回 响应报文头部信息 + 响应内容即文件内容
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);

            m_iv_count = 2;
            // 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;

            // 发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        }
        else
        {
            // 如果请求的资源大小为0，则返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    
    // 除 FILE_REQUEST 状态外，其余状态只申请一个iovec，指向响应报文缓冲区（不需要返回 mmap 文件映射区的内容）
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 统一接口：向本地写buffer写入响应行，参数为格式化字符串，供下面的写状态行、写响应头等接口调用
bool http_conn::add_response(const char *format, ...)
{
    // 如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    // 定义可变参数列表
    va_list arg_list;
    // 将变量arg_list初始化为传入参数
    va_start(arg_list, format);

    // 将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);   // 清空可变参数列表
        return false;
    }

    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

// 状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 响应头部分：包括content_length, linger, blank_line
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

// 响应内容长度字段
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

// 添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

// 添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

/* ============================================ */

// （当socket的写缓冲区从不可写变为可写，触发epollout）

// 处理好响应内容并设置好本地缓冲区之后，就可以发送给socket，这里使用 iovec + writev 的方法来输出
// 对标 read_once

// 将响应报文发送给浏览器端
bool http_conn::write()
{
    int temp = 0;

    // 若要发送的数据长度为0，表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        // 返回传输的字节数，出错或发完返回-1
        temp = writev(m_sockfd, m_iv, m_iv_count);
        
        // 异常终止情况，缓冲区满了 或 出错
        if (temp < 0)
        {
            // 缓冲区满
            if (errno == EAGAIN)    
            {
                // if (bytes_have_send >= m_iv[0].iov_len) // 响应消息已发送完，更新文件内容的iov_base
                // {
                //     m_iv[0].iov_len = 0;
                //     m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
                //     m_iv[1].iov_len = bytes_to_send;
                // }
                // else   // 消息体未发送完，更新响应消息体的iov_base
                // {
                //     m_iv[0].iov_base = m_write_buf + bytes_have_send;
                //     m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                // }
                // 重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 出错，解除文件映射
            unmap();
            return false;
        }

        // 正常发送则更新相关字节数 和 iov_base
        bytes_have_send += temp;    // 用来比较和计算偏移
        bytes_to_send -= temp;

        // 0 是响应消息体，1 是文件的内存映射
        if (bytes_have_send >= m_iv[0].iov_len) // 响应消息已发送完，更新文件内容的iov_base
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else   // 消息体未发送完，更新响应消息体的iov_base
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 数据正常发送完毕后，关闭内存映射区，重置连接对象
        if (bytes_to_send <= 0)
        {
            unmap();
            // 写完了，下一次操作是读，注册读事件（读取新的http请求或socket连接关闭的消息）
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)    // 长连接对连接对象进行重置
            {
                init();
                return true;
            }
            else             // 短链接则准备关闭连接
            {
                return false;
            }
        }
    }
}

