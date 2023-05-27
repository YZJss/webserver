#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

//#define SYNLOG  //同步写日志
#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;

static int epollfd = 0;

//信号处理函数 信号处理函数，将信号值写入管道中
void sig_handler(int sig)    // sig：接收到的信号值
{
    //为保证函数的可重入性，保留原来的errno 全局变量，用于指示最近发生的错误代码
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);    // 向管道的写入端写入信号值
    errno = save_errno;     // 恢复原来的errno值
}

//设置信号函数 使用 sigaction 函数注册信号处理函数
void addsig(int sig, void(handler)(int), bool restart = true)   // sig：信号值 handler：信号处理函数指针 restart：是否允许自动重启被信号打断的系统调用
{
    /*
    struct sigaction
    {
    void (*sa_handler)(int);                           函数指针，指向信号处理函数 在信号被触发时会被调用
    void (*sa_sigaction)(int, siginfo_t *, void *);     同样是信号处理函数，有三个参数，可以获得关于信号更详细的信息
    sigset_t sa_mask;                                   用来指定在信号处理函数执行期间需要被屏蔽的信号
    int sa_flags;                                       SA_RESTART，使被信号打断的系统调用自动重新发起
    void (*sa_restorer)(void);                          弃用
    } 
    */
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));  // s中当前位置开始向后的n个字节中，把n个字节的每个字节都替换成c
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    /*
    int sigfillset(sigset_t *set);
    用来将参数set信号集初始化，然后把所有的信号加入到此信号集里。
    */
    sigfillset(&sa.sa_mask);
    /*
    int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
    signum表示操作的信号
    act表示对信号设置新的处理方式
    oldact表示信号原来的处理方式
    返回值，0 表示成功，-1 表示有错误发生
    */
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

//    if (argc <= 1)
//    {
//        printf("usage: %s ip_address port_number\n", basename(argv[0]));
//        return 1;
//    }
//
//     int port = atoi(argv[1]);
    int port = 9006;
    /*
    当进程向一个已经关闭的套接字写数据时，内核会向进程发送一个 SIGPIPE 信号，通知进程当前写操作失败
    SIGPIPE 信号被设置为忽略，那么进程不会因为这个信号而被中断或终止，而是会继续执行

    套接字在连接断开之后会自动关闭，如果进程在这个时候继续向已经关闭的套接字写数据，
    就会触发 SIGPIPE 信号。为了避免进程被中断或终止，通常会将 SIGPIPE 信号设置为忽略，
    */
    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    // 上传github时删除账号密码
    connPool->init("localhost", "root", "Yzj050704.", "yourdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    //创建socket 成功：返回文件描述符 失败：-1 
    int listenfd = socket(PF_INET, SOCK_STREAM, 0); //PF_INET:IPv4 SOCK_STREAM:tcp
    assert(listenfd >= 0);

    //struct linger tmp={1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address; //存储一个IPv4地址和端口号的信息
    //创建监听socket的TCP/IP的IPV4 socket地址
    bzero(&address, sizeof(address));       //用于将指定内存区域中的前 n 个字节全部设置为 0
    address.sin_family = AF_INET;                   //IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY);    // htonl 将主机字节序转换为网络字节序（大端字节序） INADDR_ANY：将套接字绑定到所有可用的接口
    address.sin_port = htons(port);         //服务器要监听的端口号

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));    /* SO_REUSEADDR 允许端口被重复使用 */
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address)); //将套接字绑定到指定的 IP 地址和端口号 成功：0，失败：-1
    assert(ret >= 0);
    ret = listen(listenfd, 5);  //监听  未连接的和已经连接的和的最大值 5 成功：0，失败：-1
    assert(ret >= 0);


    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);  //size最多可以监听的文件描述符数量
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    //创建管道
    /*
    int socketpair(int domain, int type, int protocol, int sv[2]);
    pipefd 1写入端 0读取端
    */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    /**
    events描述事件类型，其中epoll事件类型有以下几种
    EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）
    EPOLLET：将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)而言的
    EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里
    **/
   
    while (!stop_server)
    {
        /*
        int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
        功能：检测哪些文件描述符发生了改变
        epfd：epoll实例对应的文件描述符
        events：传出参数，保存了发生了变化的文件描述符的信息
        maxevents：第二个参数结构体数组的大小
        timeout：阻塞时长 0 不阻塞 -1阻塞
        返回值: >0 成功，返回发送变化的文件描述符的个数 -1 失败
        */
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);     // 调用 epoll_wait 等待事件
        if (number < 0 && errno != EINTR)   // 出错处理
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)    // 遍历 文件描述符发生了改变的events 数组，处理事件
        {
            int sockfd = events[i].data.fd;
            // cout << sockfd << endl;

            if (sockfd == listenfd) // 1 如果该事件是新连接请求
            {
                cout << "新连接 fd=" << sockfd << endl;
                struct sockaddr_in client_address;  // 接受连接请求
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                /*
                int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
                功能：接收客户端连接，默认是一个阻塞的函数，阻塞等待客户端连接
                sockfd: 用于通信的文件描述符
                addr: 客户端要连接的服务器的地址信息
                addrlen: 指定第二个参数的对应的内存大小
                成功：用于通信的文件描述符 失败：-1
                */
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                if (connfd < 0) // accept 失败
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)  // 超过最大连接数
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                // 初始化客户连接对象
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                cout << "新连接 fd=" << sockfd << endl;
                while (1)   //需要循环读数据 非阻塞
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            /*
            EPOLLERR：表示对应的文件描述符发生错误
            EPOLLHUP：表示对应的文件描述符被挂断
            EPOLLRDHUP: 客户端直接调用close，会触发EPOLLRDHUP事件
            */
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                cout << "关闭连接" << endl; 
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            //处理信号
            //EPOLLIN：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }

            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            //EPOLLOUT：表示对应的文件描述符可以写
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
