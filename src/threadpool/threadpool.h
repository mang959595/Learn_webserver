#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template <typename T>
class threadpool
{
private:
    int m_thread_num;            // 线程数量
    int m_max_request;           // 请求队列中的最大请求数
    pthread_t *m_threads;        // 线程数组
    std::list<T *> m_workqueue;  // 请求队列
    locker m_queuelocker;        // 队列资源的互斥锁
    sem m_queuestat;             // 队列资源的信号量
    connection_pool *m_connPool; // 数据库连接池
    int m_actor_model;           // 模式选择

    static void *worker(void *arg); // 工作线程所运行的函数，这个函数不断从工作队列中取任务执行
    void run();

public:
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);
};

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000) : m_actor_model(actor_model), m_connPool(connPool), m_thread_num(thread_number), m_max_request(max_request), m_threads(nullptr)
{
    // 构造函数完成线程的创建，并把每个子线程分离出去
    if (thread_number <= 0 || max_request <= 0)
        throw std::exception();

    m_threads = new pthread_t[m_thread_num];
    if (!m_threads)
        throw std::exception();

    for (int i = 0; i < thread_number; i++)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) // 注意这里把this作为worker的参数传进去了，即用本对象做参数
        {
            delete[] m_threads;
            throw std::exception;
        }

        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception;
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    // 析构时释放资源
    delete[] m_threads;
}

// 向队列中添加时，通过互斥锁保证线程安全，添加完成后通过信号量提醒有任务要处理，最后注意线程同步。
// 向请求队列插入一个特定state的请求，唤醒正在等待队列的线程
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_request) // 请求数已满，请求失败，注意返回前要记得解锁mutex即locker
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;       // 设置请求的state
    m_workqueue.push_back(request); // 向请求队列插入一个请求
    m_queuelocker.unlock();

    m_queuestat.post(); // 唤醒正在等待队列的线程
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_request) // 请求数已满，请求失败，注意返回前要记得解锁mutex即locker
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request); // 向请求队列插入一个请求
    m_queuelocker.unlock();

    m_queuestat.post(); // 唤醒正在等待队列的线程
    return true;
}

// 因为pthread_create指定的函数格式和C++中this指针的矛盾，要把传给线程的函数声明为 static 静态成员函数：这里是worker函数
// 但是这样worker就不能直接访问类中的非静态成员了，需要用类的对象作为参数传进来去间接访问
// 为了不写太多的 pool->xxx 类指针访问，又另外定义了一个函数 run 来完成 worker 即工作线程要做的主要工作
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    // 不断循环获取请求并进行处理
    while (true)
    {
        m_queuestat.wait(); // 等待请求队列资源，即 P 操作

        m_queuelocker.lock();    // 被唤醒后，要操作请求队列这个共享资源时得上锁
        if (m_workqueue.empty()) // 没抢到，没任务了，解锁后接着循环等
        {
            m_queuelocker.unlock();
            continue;
        }
        else // 抢到了，获取请求队列的首个任务
        {
            T *request = m_workqueue.front();
            m_workqueue.pop_front();
            m_queuelocker.unlock(); // 解锁

            if (!request) // 空任务，continue
                continue;

            if (m_actor_model == 1) // 1模式
            {
                if (request->m_state == 0) // 0请求类型
                {
                    if (request->read_once()) // read_once
                    {
                        request->improv = 1;
                        connectionRAII mysqlcon(&request->mysql, m_connPool);
                        request->process();
                    }
                    else
                    {
                        request->improv = 1;
                        request->timer_flag = 1;
                    }
                }
                else                       // 1请求类型
                {
                    if(request->write())    // write
                    {
                        request->improv = 1;
                    }
                    else
                    {
                        request->improv = 1;
                        request->timer_flag = 1;
                    }
                }
            }
            else // 2模式
            {
                connectionRAII mysqlcon(&request->mysql, m_connPool);
                request->process();
            }
        }
    }
}

#endif