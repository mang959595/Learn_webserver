#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool::~connection_pool()
{
    DestroyPool();
}

// 构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;

    // 初始化创建 MaxConn 条数据库连接
    for (int i = 0; i < MaxConn; i++)
    {
        MYSQL *con = NULL;
        con = mysql_init(con);
        if (con == NULL)
        {
            LOG_ERROR("MySQL ERROR");
            exit(1);
        }

        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
        if (con == NULL)
        {
            LOG_ERROR("MySQL ERROR");
            exit(1);
        }

        connList.push_back(con);
        ++m_FreeConn;
    }

    // 设置数据库连接资源的信号量
    reserve = sem(m_FreeConn);

    m_MaxConn = m_FreeConn;
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con = NULL;

    if (connList.size() == 0)
        return NULL;

    // wait空闲连接信号量
    reserve.wait();

    // 更新可用连接链表和计数器
    lock.lock();
    con = connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;
    lock.unlock();

    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (con == NULL)
        return false;

    lock.lock();

    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;

    lock.unlock();

    reserve.post();
    
    return true;
}

// 销毁连接池
void connection_pool::DestroyPool()
{
    lock.lock();

    if(connList.size()>0)
    {
        for(auto &i:connList)
        {
            mysql_close(i);
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        // 清空 list
        connList.clear();
    }

    lock.unlock();
}

// 当前空闲的连接数
int connection_pool::GetFreeConn()
{
    return this->m_FreeConn;
}

// 在获取连接时，通过有参构造对传入的参数进行修改。其中数据库连接本身是指针类型，所以参数需要通过双指针才能对其进行修改。
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL = connPool->GetConnection();
    
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}
