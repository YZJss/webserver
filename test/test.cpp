#include <list>
#include <cstdio>
#include <pthread.h>
#include <semaphore.h>
#include <iostream>
#include <unistd.h>
using namespace std;

template <typename T>
class threadpool
{
public:
    threadpool(int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T request);

private:
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大请求数
    pthread_t *m_threads;       // 描述线程池的数组，其大小为m_thread_number
    list<T> m_workqueue;      // 请求队列 链表实现
    pthread_mutex_t m_mutex;    // 互斥锁
    sem_t m_sem;                // 信号量
    int sval;                   // 信号量值
    bool m_stop;                //是否结束线程

};
template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL)
{
    m_threads = new pthread_t[m_thread_number];
    sem_init(&m_sem, 0, 0);
    pthread_mutex_init(&m_mutex, NULL);
    for (int i = 0; i < thread_number; ++i)
    {
        pthread_create(m_threads + i, NULL, worker, this);
        // cout << "create " << i+1 <<"th thread" << endl;
        pthread_detach(m_threads[i]);
    }
}
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}
template <typename T>
bool threadpool<T>::append(T request)
{
    pthread_mutex_lock(&m_mutex);
    if (m_workqueue.size() > m_max_requests)
    {
        pthread_mutex_unlock(&m_mutex);
        return false;
    }
    m_workqueue.push_back(request);
    pthread_mutex_unlock(&m_mutex);
    sem_post(&m_sem);     // 解锁 +1
    sem_getvalue(&m_sem,&sval);
    cout <<"sval="<< sval <<" append" <<endl;
    return true;
}
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();

}
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        sem_wait(&m_sem);     // 加锁 -1 =0阻塞
        pthread_mutex_lock(&m_mutex);
        if (m_workqueue.empty())
        {
            pthread_mutex_unlock(&m_mutex);
            continue;
        }
        T request = m_workqueue.front();  
        m_workqueue.pop_front();
        sem_getvalue(&m_sem,&sval);
        cout << "sval="<< sval <<" run" << request <<endl;
        pthread_mutex_unlock(&m_mutex); 
    }
}

int main()
{
    threadpool<int> *pool = NULL;
    pool = new threadpool<int>;
    for(int i =1;i<=200;i++){
        pool->append(i);
    }
    sleep(5);   //防止子线程没有抢占到CPU且此时主线程已经执行完并退出
}