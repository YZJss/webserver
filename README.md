# WebServer

## 代码框架

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//tinywebserver.jfif"  />

### 代码架构

```c++
├─ CGImysql			// 数据库连接池
│  ├─ sql_connection_pool.cpp
│  └─ sql_connection_pool.h
├─ http				// 实现HTTP协议连接、销毁
│  ├─ http_conn.cpp
│  └─ http_conn.h
├─ lock				// 封装互斥锁、信号量等  
│  └─ locker.h
├─ log				// 日志 阻塞队列
│  ├─ block_queue.h
│  ├─ log.cpp
│  └─ log.h
├─ main.cpp			// 主函数
├─ makefile
├─ root				// 前端 网页
├─ threadpool		// 线程池
│  └─ threadpool.h
└─ timer			// 定时器
   ├─ README.md
   └─ lst_timer.h
```

### 文件描述符

Linux系统中一切皆可以看成是文件，文件又可分为：普通文件、目录文件、链接文件和设备文件。在操作这些所谓的文件的时候，我们每操作一次就找一次名字，这会耗费大量的时间和效率。所以Linux中规定每一个文件对应一个索引，这样要操作文件的时候，直接找到索引就可以对其进行操作了。

文件描述符（file descriptor）就是内核为了高效管理这些已经被打开的文件所创建的索引，其是一个非负整数（通常是小整数），用于指代被打开的文件，所有执行I/O操作的系统调用都通过文件描述符来实现。同时还规定系统刚刚启动的时候，0是标准输入，1是标准输出，2是标准错误。这意味着如果此时去打开一个新的文件，它的文件描述符会是3，再打开一个文件文件描述符就是4......

```bash
ls -l /proc/pid/fd	// 查看文件描述符 pid是代表的进程号
```

### socket

Socket又称为套接字，它是应用层与TCP/IP协议族通信的中间软件抽象层，它是一组接口。通过调用接口中已经实现的方法建立两个进程之间的连接和通信。

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//20210826152834.png" alt="socket" style="zoom:67%;" />

#### 服务器的工作流程：

**（1）创建 socket：**创建服务端的socket。

**（2）绑定 bind：**把服务端用于通信的地址和端口绑定到socket上。

**（3）监听 listen：**把socket设置为监听模式。

**（4）接受连接 accept：**接受客户端的连接。

**（5）通信 recv( ) / send( ) ：**与客户端通信，接收客户端发过来的报文后，回复处理结果，重复此过程。

**（6）关闭 close( )：**关闭socket，释放资源。

#### 客户端工作流程：

**（1）创建 socket：**创建客户端的socket。

**（2）发送连接 connect( )：**向服务器发起连接请求

**（3）通信 recv( ) / send( )：**与服务端通信，发送一个报文后等待回复，然后再发下一个报文。重复此过程，直到全部的数据被发送完。

**（4）关闭 close( )：**关闭socket，释放资源。

### 事件处理模式

#### Reactor

要求主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生（可读、可写），若有，则立即通知工作线程，将socket可读可写事件放入请求队列，**读写数据、接受新连接及处理客户请求均在工作线程中完成。(需要区别读和写事件)**

#### Proactor

主线程和内核负责处理读写数据、接受新连接等I/O操作，工作线程仅负责业务逻辑

Proactor模式将所有I/O操作都交给主线程和内核来处理，工作线程仅仅负责业务逻辑（给予相应的返回url）。如主线程读完成后`users[sockfd].read()`，选择一个工作线程来处理客户请求`pool->append(users + sockfd)`。

**同步I/O方式模拟出Proactor模式**原理：主线程执行数据读写操作，读写完成之后，主线程向工作线程通知这一“完成事件”。那么从工作线程的角度来看，它们就直接获得了数据读写的结果，接下来要做的只是对读写的结果进行逻辑处理。

工作流程如下：
1）主线程往epoll内核事件表中注册socket上的读就绪事件。
2）主线程调用epoll_wait等待socket上有数据可读。
3）当socket上有数据可读时，epoll_wait通知主线程。主线程从socket循环读取数据，直到没有更多数据可读，然后将读取到的数据封装成一个请求对象并插入请求队列。
4）睡眠在请求队列上的某个工作线程被唤醒，它获得请求对象并处理客户请求，然后往epoll内核事件表中注册socket上的写就绪事件。
5）主线程调用epoll_wait等待socket可写。
6）当socket可写时，epoll_wait通知主线程。主线程往socket上写入服务器处理客户请求的结果。

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//image-20230329161251178.png" alt="image-20230329161251178" style="zoom:67%;" />

### threadpool.h

![v2-ab874df7219895195def55a02fb390f7_1440w](https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//v2-ab874df7219895195def55a02fb390f7_1440w.webp)

```c++
threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000);
~threadpool();
bool append(T *request);		// 向请求队列中插入任务请求
static void *worker(void *arg);	// 工作线程运行的函数，它不断从工作队列中取出任务并执行之
void run();		//调用void http_conn::process()

int pthread_create (pthread_t *thread_tid,                 //返回新生成的线程的id
                    const pthread_attr_t *attr,         //指向线程属性的指针,通常设置为NULL
                    void * (*start_routine) (void *),   //处理线程函数的地址 static函数没有this指针
                    void *arg);                         //start_routine()中的参数
```

- 所谓线程池，就是一个`pthread_t`类型的普通数组，通过`pthread_create()`函数创建`m_thread_number`个**线程**，用来执行`worker()`函数以执行每个请求处理函数（HTTP请求的`process`函数），通过`pthread_detach()`将线程设置成脱离态（detached）后，当这一线程运行结束时，它的资源会被系统自动回收，而不再需要在其它线程中对其进行 `pthread_join()` 操作。
- 操作工作队列一定要加锁（`locker`），因为它被所有线程共享。
- 用信号量来标识请求队列中的请求数，通过`m_queuestat.wait();`来等待一个请求队列中待处理的HTTP请求，然后交给线程池中的空闲线程来处理。

#### 为什么要使用线程池？

当你需要限制你应用程序中同时运行的线程数时，线程池非常有用。因为启动一个新线程会带来性能开销，每个线程也会为其堆栈分配一些内存等。为了任务的并发执行，我们可以将这些任务任务传递到线程池，而不是为每个任务动态开启一个新的线程。（空间换时间）

####  线程池实现

```c++
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
    for(int i =1;i<=20;i++){
        pool->append(i);
    }
    sleep(5);   //防止子线程没有抢占到CPU且此时主线程已经执行完并退出
}
```

#### 线程池的设计思路，线程池中线程的数量由什么确定？

1. **设计思路**：

   实现线程池有以下几个步骤：
   （1）设置一个生产者消费者队列，作为临界资源。

   （2）初始化n个线程，并让其运行起来，加锁去队列里取任务运行

   （3）当任务队列为空时，所有线程阻塞。

   （4）当生产者队列来了一个任务后，先对队列加锁，把任务挂到队列上，然后使用条件变量去通知阻塞中的一个线程来处理。

2. **线程池中线程数量**：

   线程数量和哪些因素有关：CPU，IO、并行、并发

   CPU密集型应用，则线程池大小设置为：CPU数目+1
   IO密集型应用，则线程池大小设置为：2CPU数目+1
   
   所以线程等待时间所占比例越高，需要越多线程。线程CPU时间所占比例越高，需要越少线程。
   
   线程池中的线程数量最直接的限制因素是中央处理器(CPU)的处理器(processors/cores)的数量`N`：如果你的CPU是4-cores的，对于CPU密集型的任务(如视频剪辑等消耗CPU计算资源的任务)来说，那线程池中的线程数量最好也设置为4（或者+1防止其他因素造成的线程阻塞）；对于IO密集型的任务，一般要多于CPU的核数，因为线程间竞争的不是CPU的计算资源而是IO，IO的处理一般较慢，多于cores数的线程将为CPU争取更多的任务，不至在线程处理IO的过程造成CPU空闲导致资源浪费，公式：`最佳线程数 = CPU当前可使用的Cores数 * 当前CPU的利用率 * (1 + CPU等待时间 / CPU处理时间)`
   
3. **为什么要创建线程池**：

   创建线程和销毁线程的花销是比较大的，这些时间有可能比处理业务的时间还要长。这样频繁的创建线程和销毁线程，再加上业务工作线程，消耗系统资源的时间，可能导致系统资源不足。**同时线程池也是为了提升系统效率。**

### locker.h

封装了信号量、互斥锁、条件变量

```c++
sem();	// 信号量
bool wait();	// p操作 -1
bool post();	// v操作 +1

locker();	// 互斥锁 保证同一时刻只能有一个线程去操作
bool lock();	//加锁
bool unlock();	//解锁

cond();	// 条件变量
bool wait();	// 先把调用线程放入条件变量的请求队列，然后将互斥锁解锁，当函数成功返回为0时，表示重新抢到了互斥锁，互斥锁会再次被锁上
bool broadcast();	// 以广播的方式唤醒所有等待目标条件变量的线程
```

### sql_connection_pool.h

数据库连接池

单例模式、list实现连接池、连接池为静态大小、互斥锁实现线程安全

每一个HTTP连接获取一个数据库连接，获取其中的用户账号密码进行对比（有点损耗资源，实际场景下肯定不是这么做的），而后再释放该数据库连接。

### http_conn.h

HTTP的**报文处理流程**分为以下三个步骤：

- **连接处理：**浏览器端发出http连接请求，主线程创建http对象接收请求并将所有数据读入对应buffer，将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
- **处理报文请求**：工作线程取出任务后，调用process_read函数，通过主、从状态机对请求报文进行解析。
- **返回响应报文：**解析完之后，跳转do_request函数生成响应报文，通过process_write写入buffer，返回给浏览器端。

```c++
void http_conn::init(int sockfd, const sockaddr_in &addr);	// 外部调用初始化
bool http_conn::read_once();		// 读取数据存储到m_read_buf
void http_conn::process();		// 调用process_read()和process_write(read_ret)

HTTP_CODE process_read();		// 从m_read_buf读取，并处理请求报文
HTTP_CODE parse_request_line(char *text);	// 主状态机解析报文中的请求行数据 获得请求方法，目标url及http版本号
HTTP_CODE parse_headers(char *text);		// 主状态机解析报文中的请求头数据
HTTP_CODE parse_content(char *text);		// 主状态机解析报文中的请求内容

char *get_line() { return m_read_buf + m_start_line; };	// get_line用于将指针向后偏移，指向未处理的字符
http_conn::LINE_STATUS http_conn::parse_line()// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN


bool process_write(HTTP_CODE ret);	// //向m_write_buf写入响应报文数据


```

有限状态机一种逻辑单元内部的一种高效编程方法，在服务器编程中，服务器可以根据不同状态或者消息类型进行相应的处理逻辑，使得程序逻辑清晰易懂。

通过while循环，对**主从状态机**进行封装，对报文的每一行进行循环处理。这里的**主状态机**，指的是process_read()函数，**从状态机**是指parse_line()函数。

从状态机负责**读取报文的一行（并对其中的\r\n进行修改为\0\0）**，主状态机负责对该行数据进行解析，**主状态机内部调用从状态机，从状态机驱动主状态机**。它们之间的关系如下图所示：

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//640" alt="img"  />

主状态机初始状态是CHECK_STATE_REQUESTLINE，而后调用parse_request_line()解析请求行，获得HTTP的请求方法、目标URL以及HTTP版本号，状态变为CHECK_STATE_HEADER。

此时进入循环体之后，调用parse_headers()解析请求头部信息。先要判断是空行还是请求头，空行进一步区分POST还是GET。若是请求头，则更新长短连接状态、host等等。

注：GET和POST请求报文的区别之一是有无消息体部分。

当使用POST请求时，需要进行CHECK_STATE_CONTENT的解析，取出POST消息体中的**信息（用户名、密码）**。

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//640.jpg" alt="640"  />

#### HTTP报文

HTTP报文分为请求报文和响应报文两种，其中，浏览器端向服务器发送的为**请求报文**，服务器处理后返回给浏览器端的为**响应报文**。

HTTP**请求报文**由**请求行**（request line）、**请求头部**（header）、**空行**和**请求数据**四个部分组成。

**GET和POST**是最常见的HTTP请求方法，除此以外还包括DELETE、HEAD、OPTIONS、PUT、TRACE。

##### GET

请求指定的页面内容，并返回实体主体

```
GET /favicon.ico HTTP/1.1
Host: 49.232.165.212:9006
Connection: keep-alive
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/111.0.0.0 Safari/537.36
Accept: image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8
Referer: http://49.232.165.212:9006/
Accept-Encoding: gzip, deflate
Accept-Language: zh,en-US;q=0.9,en;q=0.8,zh-TW;q=0.7,zh-CN;q=0.6


```

##### POST

向指定资源提交数据进行处理请求（例如提交表单或者上传文件）。数据包含在请求体中。POST请求可能会导致新的资源的建立或者已有资源的修改。

```
POST /2CGISQL.cgi HTTP/1.1
Host: 49.232.165.212:9006
Connection: keep-alive
Content-Length: 17
Cache-Control: max-age=0
Upgrade-Insecure-Requests: 1
Origin: http://49.232.165.212:9006
Content-Type: application/x-www-form-urlencoded
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/111.0.0.0 Safari/537.36
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7
Referer: http://49.232.165.212:9006/1
Accept-Encoding: gzip, deflate
Accept-Language: zh,en-US;q=0.9,en;q=0.8,zh-TW;q=0.7,zh-CN;q=0.6

user=a&password=a
```

- **请求行**，用来说明请求类型,要访问的资源以及所使用的HTTP版本。

- **请求头部**，紧接着请求行（即第一行）之后的部分，用来说明服务器要使用的附加信息。

- - HOST，给出请求资源所在服务器的域名。
  - User-Agent，HTTP客户端程序的信息，该信息由你发出请求使用的浏览器来定义,并且在每个请求中自动发送等。
  - Accept，说明用户代理可处理的媒体类型。
  - Accept-Encoding，说明用户代理支持的内容编码。
  - Accept-Language，说明用户代理能够处理的自然语言集。
  - Content-Type，说明实现主体的媒体类型。
  - Content-Length，说明实现主体的大小。
  - Connection，连接管理，可以是Keep-Alive或close。

- **空行**，请求头部后面的空行是必须的即使第四部分的请求数据为空，也必须有空行。

- **请求数据**也叫主体，可以添加任意的其他数据。

#### 响应报文

```
HTTP/1.1 200 OK
Content-Length:360
Connection:keep-alive


```

- 状态行，由HTTP协议版本号， 状态码， 状态消息 三部分组成。
  第一行为状态行，（HTTP/1.1）表明HTTP版本为1.1版本，状态码为200，状态消息为OK。
- 消息报头，用来说明客户端要使用的一些附加信息。
  第二行和第三行为消息报头，Date:生成响应的日期和时间；Content-Type:指定了MIME类型的HTML(text/html),编码类型是UTF-8。
- 空行，消息报头后面的空行是必须的。
- 响应正文，服务器返回给客户端的文本信息。空行后面的html部分为响应正文。

#### 注册登录

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//642.jpg" alt="642"  />

### log.h

#### 单例模式

```c++
// 懒汉模式
class single{
private:
    single(){}
    ~single(){}
public:
    static single* getinstance();
};
single* single::getinstance(){
    static single obj;
    return &obj;
}
// 饿汉模式
class single{
private:
    single(){}
 		~single(){}
public:
    static single* getinstance();
private:
    static single obj;
};
single* single:: getinstance(){
  return &obj;
}
single single:: obj;
int main(){
	single* A = single::getinstance();
}
```

最常用的设计模式之一，保证一个类仅有一个实例，并提供一个访问它的全局访问点，该实例被所有程序模块共享。实现思路：私有化它的构造函数，以防止外界创建单例类的对象；使用类的私有静态指针变量指向类的唯一实例，并用一个公有的静态方法获取该实例。

- **懒汉模式**

  即非常懒，不用的时候不去初始化，所以在第一次被使用时才进行初始化（实例的初始化放在getinstance函数内部）

  - 经典的线程安全懒汉模式，使用双检测锁模式（`p == NULL`检测了两次）
  - 利用局部静态变量实现线程安全懒汉模式

- **饿汉模式**：即迫不及待，在程序运行时立即初始化（实例的初始化放在`getinstance`函数外部，`getinstance`函数仅返回该唯一实例的指针）。

**日志系统的运行机制**

- 日志文件
  - 局部变量的懒汉模式获取实例
  - 生成日志文件，并判断同步和异步写入方式
- 同步
  - 判断是否分文件
  - 直接格式化输出内容，将信息写入日志文件
- 异步
  - 判断是否分文件
  - 格式化输出内容，将内容写入**阻塞队列**（循环数组、条件变量实现），创建一个写线程，从阻塞队列取出内容写入日志文件



<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//643.jpg" alt="643"  />

### block_queue.h

阻塞队列(循环数组实现队列，STL queue也可以)封装了生产者-消费者模型，push为生产者，pop为消费者。

### lst_timer.h

服务器首先**创建定时器容器有序链表**，然后用统一事件源将（统一事件源是指将信号事件与其他事件一样被处理。）异常事件、读写事件和信号事件统一处理，根据不同事件的对应逻辑使用定时器。

具体的，浏览器与服务器连接时，创建该连接对应的定时器，并将该定时器添加到定时器容器链表上；处理异常事件时，执行定时事件，服务器关闭连接，从链表上移除对应定时器；处理定时信号时，将定时标志设置为true，以便执行定时器处理函数；处理读/写事件时，若某连接上发生读事件或某连接给浏览器发送数据，将对应定时器向后移动，否则，执行定时事件。

<img src="https://cdn.jsdelivr.net/gh/YZJss/tuchuang@main//v2-50f8a9c898447e79c04810883e7ed332_1440w.webp" alt="v2-50f8a9c898447e79c04810883e7ed332_1440w" style="zoom:80%;" />

### 压力测试

WebBench首先fork出多个子进程，每个子进程都循环做web访问测试。子进程把访问的结果通过pipe告诉父进程，父进程做最终的统计结果。

QBS是一台服务器每秒能够相应的查询次数







