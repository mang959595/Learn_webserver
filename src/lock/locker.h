#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 信号量封装（ P V 操作，可用于同步）
class sem
{
private:
    sem_t m_sem;

public:
    // 默认构造函数将信号量的值设为0
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
            throw std::exception();
    }
    // 信号量的值设为num
    sem(int num)
    {
        if (sem_init(&m_sem, 0, 0) != 0)
            throw std::exception();
    }

    ~sem()
    {
        sem_destroy(&m_sem);
    }

    // P
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }
    // V
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }
};

// 互斥锁
class locker
{
private:
    pthread_mutex_t m_mutex;

public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
            throw std::exception();
    }

    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get()
    {
        return &m_mutex;
    }
};

// 条件变量，与互斥锁结合来实现同步
// 注意当 wait 阻塞的时候，会将 互斥锁 解锁；
// 等到 wait 被唤醒之后，又会在原处重新 对 互斥锁 加锁
// 通常的应用场景下，当前线程执行pthread_cond_wait时，处于临界区访问共享资源，存在一个mutex与该临界区相关联，这是理解pthread_cond_wait带有mutex参数的关键
class cond
{
private:
    pthread_cond_t m_cond;

public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
            throw std::exception();
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    // 原函数返回0表示调用成功
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }

    bool timewait(pthread_mutex_t *m_mutex, timespec t)
    {
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }

    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }
};

#endif