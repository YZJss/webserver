#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*connPool是数据库连接池指针，thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    //向请求队列中插入任务请求
    bool append(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列 链表实现
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    bool m_stop;                //是否结束线程
    connection_pool *m_connPool;  //数据库
};
template <typename T>
threadpool<T>::threadpool( connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        //        int pthread_create (pthread_t *thread_tid,                 //返回新生成的线程的id
        //                            const pthread_attr_t *attr,         //指向线程属性的指针,通常设置为NULL
        //                            void * (*start_routine) (void *),   //处理线程函数的地址
        //                            void *arg);                         //start_routine()中的参数
        //          成功:0;失败:错误号
        //  void*可以指向任何类型的地址，但是带类型的指针不能指向void*的地址
        // 每调用一次pthread_create就会调用一次run(),因为每个线程是相互独立的，都睡眠在工作队列上，仅当信号变量更新才会唤醒进行任务的竞争。
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        cout << "create " << i+1 <<"th thread" << endl;
        //主线程与子线程分离，子线程结束后，资源自动回收
        // 成功:0;失败:错误号
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
    cout  << "del threadpool" << endl;
}
template <typename T>
bool threadpool<T>::append(T *request)
{
    int sval = 0;
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();     // 解锁 +1
    
    cout <<"信号量="<< m_queuestat.sem_value() <<" 将任务添加到请求队列" <<endl;
    

    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    //将参数强转为线程池类，调用成员方法
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    int sval = 0;
    while (!m_stop)
    {
        //信号量等待
        m_queuestat.wait();     // 加锁 -1 =0阻塞
        //被唤醒后先加互斥锁
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        //从请求队列中取出第一个任务
        //将任务从请求队列删除
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        //从连接池中取出一个数据库连接
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        //process(模板类中的方法,这里是http类)进行处理
        request->process();
        
        cout << "信号量="<< m_queuestat.sem_value() <<" 从请求队列取出任务并执行" <<endl;
        
        
    }
}
#endif
