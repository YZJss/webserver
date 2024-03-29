#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include<netinet/in.h>
#include "../log/log.h"

/*
client_data 结构体 存储客户端地址和文件描述符 指向定时器
util_timer 定时器 ：超时时间 回调函数 指向前一个/后一个定时器
sort_timer_lst 有序链表定时器：添加/调整/删除/插入定时器  定时任务处理函数 tick() 调用回调函数
*/

class util_timer;
struct client_data
{
    sockaddr_in address;    // 存储客户端的地址信息
    int sockfd;              // 存储客户端的文件描述符
    util_timer *timer;      // 指向定时器
};

class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;      // 定时器超时时间
    void (*cb_func)(client_data *);     // 定时器回调函数
    client_data *user_data;         // 定时器回调函数的参数
    util_timer *prev;       // 指向前一个定时器
    util_timer *next;       // 指向后一个定时器
};

class sort_timer_lst
{
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    ~sort_timer_lst()
    {
        util_timer *tmp = head;
        while (tmp)
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
    void add_timer(util_timer *timer)       // 添加定时器
    {
        if (!timer) // 如果定时器为空，直接返回
        {
            return;
        }
        if (!head)  // 如果链表为空，直接将定时器添加到头部
        {
            head = tail = timer;
            return;
        }
        //如果新的定时器超时时间小于当前头部结点
        //直接将当前定时器结点作为头部结点
        if (timer->expire < head->expire)
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        //否则调用私有成员，调整内部结点
        add_timer(timer, head);
    }
    //调整定时器，任务发生变化时，调整定时器在链表中的位置
    void adjust_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        util_timer *tmp = timer->next;
        //被调整的定时器在链表尾部
        //定时器超时值仍然小于下一个定时器超时值，不调整
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        //被调整定时器是链表头结点，将定时器取出，重新插入
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        //被调整定时器在内部，将定时器取出，重新插入
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }
    //删除定时器
    void del_timer(util_timer *timer)
    {
        if (!timer)
        {
            return;
        }
        //链表中只有一个定时器，需要删除该定时器
        if ((timer == head) && (timer == tail))
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        //链表中只有一个定时器，需要删除该定时器
        if (timer == head)
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        //被删除的定时器为尾结点
        if (timer == tail)
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        //被删除的定时器在链表内部，常规链表结点删除
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    //定时任务处理函数
    void tick()
    {
        if (!head)
        {
            return;
        }
        //printf( "timer tick\n" );
        LOG_INFO("%s", "timer tick");
        Log::get_instance()->flush();
        //获取当前时间
        time_t cur = time(NULL);
        util_timer *tmp = head;
        //遍历定时器链表
        while (tmp)
        {
            //链表容器为升序排列
            //当前时间小于定时器的超时时间，后面的定时器也没有到期
            if (cur < tmp->expire)
            {
                break;
            }
            //当前定时器到期，则调用回调函数，执行定时事件
            tmp->cb_func(tmp->user_data);
            //将处理后的定时器从链表容器中删除，并重置头结点
            head = tmp->next;
            if (head)
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    void add_timer(util_timer *timer, util_timer *lst_head) // 用于将定时器插入定时器链表中
    {
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;
        while (tmp)
        {
            if (timer->expire < tmp->expire)
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
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer *head;
    util_timer *tail;
};

#endif
