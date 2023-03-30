#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

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
#include <string>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

using namespace std;

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;

    // http请求类型,只实现了get和post
    enum METHOD {
        GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH
    };

    // 主状态机状态
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    // 从状态机状态
    enum LINE_STATUS {
        LINE_OK =0,
        LINE_BAD,
        LINE_OPEN
    };

    // 请求的解析状态码
    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

public:
    static int m_epollfd;   // epoll对应的fd
    static int m_user_count; // 总连接数
    MYSQL* mysql;
    int m_state;    // 读0，写1

private:
    int m_sockfd;   // socket连接
    sockaddr_in m_address;
    
    // 读数据缓冲区，及相关索引
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;    // 缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    long m_checked_idx; // m_read_buf读取的位置m_checked_idx
    int m_start_line;   // m_read_buf中已经解析的字符个数

    // 写数据缓冲区，及相关索引
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;    // 指示buffer中数据的长度

    CHECK_STATE m_check_state;  // 主状态机状态
    METHOD m_method;            // 请求类型

    // 请求解析后得到的数据，对应下面6个变量
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;

    char *m_file_address;   // 服务器文件内存映射后得到的内存地址
    struct stat m_file_stat;
    struct iovec m_iv[2];   // io向量机制iovec
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;  // 剩余发送字节数
    int bytes_have_send;// 已发送字节数

    char *doc_root;

    map<string,string> m_users;
    int m_TRIGMode;
    int m_close_log;

    // mysql连接相关
    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];

public:
    http_conn();
    ~http_conn();

    void init(int sockfd, const sockaddr_in&addr, char *, int, int, string user, string passwd, string sqlname);    // 设置sockfd和数据库账号
    void close_conn(bool real_close = true);    // 关闭sock连接
    void process();     // 
    bool read_once();   // 非阻塞读取socket中的数据，放到对象的数据成员中
    bool write();       // 将对象生成的响应数据发送到socket缓冲区

    sockaddr_in *get_address() {return &m_address;}
    void initmysql_result(connection_pool *connPool);

    int timer_flag;
    int improv;

private:
    void init(); // 设置sockfd等

    // 解析请求相关
    HTTP_CODE process_read();           // 解析本地读缓存区中的数据
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();

    void unmap();

    // 生成响应相关
    HTTP_CODE do_request();             // 生成响应报文到本地写缓冲区
    bool process_write(HTTP_CODE ret);  // 本地写缓冲区  》》》 socket写缓冲区
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

};

#endif