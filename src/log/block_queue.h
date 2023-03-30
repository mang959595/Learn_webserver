#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

using namespace std;


// 线程安全，每个操作前都要先加互斥锁，操作完后，再解锁

// 循环数组实现的阻塞队列，push时更新 m_back = (m_back + 1) % m_max_size;  
template<class T>
class block_queue
{
private:
    locker m_mutex;
    cond m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;        // 队首索引
    int m_back;         // 队尾索引

public:
    block_queue(int max_size = 1000)
    {
        if(max_size<=0)
            exit(-1);
        
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    ~block_queue()
    {
        m_mutex.lock();
        if(m_array!=NULL)
            delete[] m_array;
        m_mutex.unlock;
    }

    void clear()
    {
        m_mutex.lock();
        m_size=0;
        m_front=-1;
        m_back=-1;
        m_mutex.unlock();
    }

    bool full();    // 队列是否满了
    bool empty();   // 队列是否为空
    bool front(T&value);
    bool back(T&value);
    int size();
    int max_size();

    // 生产者: push往队列添加元素，需要将所有使用队列的线程先唤醒  (若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item);
    // 消费者: pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T&item);
    // pop增加超时处理
    bool pop(T&item, int ms_timeout);
};


template<typename T>
bool block_queue<T>::push(const T &item)
{
    m_mutex.lock();
    // 满了就直接广播唤醒消费者, 并且不能往队列里加item了
    if(m_size >= m_max_size)
    {
        m_cond.broadcast();
        m_mutex.unlock();
        return false;
    }

    // 队尾加入 item
    m_back = (m_back+1)%m_max_size;
    m_array[m_back] = item;

    m_size++;

    // 广播唤醒
    m_cond.broadcast();
    m_mutex.unlock();

    return true;
}

template<typename T>
bool block_queue<T>::pop(T&item)
{
    m_mutex.lock();
    // 资源条件不成立, cond_wait
    while(m_size<=0)
    {
        // 条件变量的wait需要配合mutex使用, 函数内部会进行解锁(阻塞)和加锁(被唤醒后竞争得到)的操作
        if(!m_cond.wait(m_mutex.get()))
        {   // 出错的情况
            m_mutex.unlock();
            return false;
        }
    }

    // 从队头取出
    m_front = (m_front+1) % m_max_size;
    item = m_array[m_front];
    
    m_size--;
    
    m_mutex.unlock();
    return true;
}

template<typename T>
bool block_queue<T>::pop(T&item, int ms_timeout)
{
    // 时间实参的设置
    struct timespec t = {0, 0};
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);

    m_mutex.lock();
   
    // 此处使用 if , 因为wait一次就够了.    timewait的阻塞是有限的, 即本pop函数也是有限阻塞, 本次消费请求最终可能成功也可能失败
    if(m_size<=0)
    {
        t.tv_sec = now.tv_sec + ms_timeout / 1000;
        t.tv_nsec = (ms_timeout % 1000) * 1000;
        
        if(!m_cond.timewait(m_mutex.get(),t))
        {   // 出错的情况
            m_mutex.unlock();
            return false;
        }
    }
    // 有限的等待并取得锁之后, 资源还是不够, 本次消费宣告失败;  相比之下的while: 若取得锁之后资源还是不够,会继续while循环wait资源
    if (m_size <= 0)
    {
        m_mutex.unlock();
        return false;
    }


    // 从队头取出
    m_front = (m_front+1) % m_max_size;
    item = m_array[m_front];
    
    m_size--;
    
    m_mutex.unlock();
    return true;
}

template<typename T>
bool block_queue<T>::full()
{
    m_mutex.lock();
    if(m_size>= m_max_size)
    {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return true;
}

template<typename T>
bool block_queue<T>::empty()
{
    m_mutex.lock();
    if (0 == m_size)
    {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template<typename T>
bool block_queue<T>::front(T &value)
{
    m_mutex.lock();
    if (0 == m_size)
    {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_front];
    m_mutex.unlock();
    return true;
}

template<typename T>
bool block_queue<T>::back(T &value) 
{
    m_mutex.lock();
    if (0 == m_size)
    {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_back];
    m_mutex.unlock();
    return true;
}

template<typename T>
int block_queue<T>::size() 
{
    int tmp = 0;

    m_mutex.lock();
    tmp = m_size;

    m_mutex.unlock();
    return tmp;
}

template<typename T>
int block_queue<T>::max_size()
{
    int tmp = 0;

    m_mutex.lock();
    tmp = m_max_size;

    m_mutex.unlock();
    return tmp;
}




#endif