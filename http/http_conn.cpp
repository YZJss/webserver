#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空 需要根据情况改
const char *doc_root = "/home/ubuntu/tinywebserver/root";

//将表中的用户名和密码放入map
map<string, string> users;
locker m_lock;

//将数据库中的用户名和密码载入到服务器的map中来
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞 使得在读取或写入数据时，不会一直阻塞等待数据的到来或写入完成。
int setnonblocking(int fd)
{
    /*fcntl
    对已打开的文件描述符进行各种控制操作以改变已打开文件的的各种属性
    */
    int old_option = fcntl(fd, F_GETFL);    // 获取fd的文件状态标志
    int new_option = old_option | O_NONBLOCK;   // 添加非阻塞标志
    fcntl(fd, F_SETFL, new_option); // 将新的标志设置为fd的文件状态标志
    return old_option;  // 返回以前的文件状态标志
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
/*当一个文件描述符被设置为一次性事件时，当它触发一次监听事件后，监听就会被自动删除，因此需要再次添加到 epoll 监听队列中才能继续监听该文件描述符的事件。 */

void addfd(int epollfd, int fd, bool one_shot)  //epoll 文件描述符，待监听文件描述符fd，是否为一次性事件
{
    epoll_event event;  // epoll 事件结构体
    event.data.fd = fd; // 设置事件结构体的文件描述符

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  // ET 模式下添加监听读事件，边缘触发和连接关闭事件
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;    // LT 模式下添加监听读事件和连接关闭事件
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot)
        event.events |= EPOLLONESHOT;   // 如果是一次性事件，则将事件设置为 EPOLLONESHOT

    /*
    int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    功能：对epoll实例进行管理：添加文件描述符信息，删除信息，修改信息
    epfd：epoll实例对应的文件描述符
    op：要进行什么操作 添加 删除 修改
    fd：要检测的文件描述符
    event：检测文件描述符什么事情，通过设置`epoll_event.events`
    */
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  // 将文件描述符添加到 epoll 监听中
    setnonblocking(fd);     // 设置文件描述符为非阻塞模式
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);   // 通过epoll_ctl函数将要移除的文件描述符从epoll实例中删除
    close(fd);      // 关闭文件描述符
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);  // 使用epoll_ctl函数，将修改后的事件重新加入到epoll实例中
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close) //bool real_close：表示是否真正关闭连接。如果为 true，则关闭连接，否则不关闭。
{
    if (real_close && (m_sockfd != -1)) // 如果需要真正关闭连接，并且当前套接字有效
    {
        removefd(m_epollfd, m_sockfd);  // 从 epoll 监听的事件中删除该套接字
        m_sockfd = -1;  // 将套接字置为无效
        m_user_count--; // 客户端数量减一
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr)   //HTTP 连接的套接字描述符 HTTP 连接的远程地址
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true); // 将套接字添加到 epoll 监听的事件中
    m_user_count++; // 客户端数量加一
    init(); // 初始化 HTTP 连接
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;   // 初始化 MySQL 连接
    bytes_to_send = 0;  // 待发送字节数清零
    bytes_have_send = 0;    // 已发送字节数清零
    m_check_state = CHECK_STATE_REQUESTLINE;    // 解析客户请求的当前状态
    m_linger = false;   // 是否保持连接
    m_method = GET; // HTTP 请求方法，默认为 GET
    m_url = 0;  // 请求的 URL
    m_version = 0;  // HTTP 版本号，默认为 HTTP/1.1
    m_content_length = 0;   // 请求的消息体长度
    m_host = 0; // 请求的主机名
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE); //缓存清零
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        //temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //下一个字符是\n，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }
        //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {
            //前一个字符是\r，则接收完整
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) // 检查读取缓冲区是否已满
    {
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT // 对于 LT 模式，一次性读取所有数据
    // 读取客户端套接字中的数据到读取缓冲区中
    /*
    int recv( SOCKET s, char *buf, int len, int flags);
    s:指定接收端套接字描述符
    buf：指向存储接收数据的缓冲区的指针
    len：缓冲区的长度
    flags：控制接收操作的标志
    函数的返回值为接收到的字节数，如果发生错误，则返回-1。
    */
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    // 如果读取失败或已到达文件末尾，则返回 false
    if (bytes_read <= 0)
    {
        return false;
    }
    // 更新已经读取的字节数
    m_read_idx += bytes_read;
    // 返回读取成功
    cout << "请求报文" <<endl;
    cout << m_read_buf << endl;
    return true;

#endif

#ifdef connfdET // 对于 ET 模式，需要使用循环读取所有数据
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0); // 读取客户端套接字中的数据到读取缓冲区中
        if (bytes_read == -1)   // 如果读取失败，则判断是否是暂时无法读取数据的情况
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;   // 其他情况表示出现错误，返回 false
        }
        else if (bytes_read == 0)   // 如果已经到达文件末尾，则返回 false
        {
            return false;
        }
        m_read_idx += bytes_read;   // 更新已经读取的字节数
    }
    return true;    // 返回读取成功
#endif
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//主状态机解析报文中的请求头数据
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//主状态机解析报文中的请求内容
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//用于处理客户端发送的 HTTP 请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:   // 解析请求行
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) // 如果请求行有误，返回 BAD_REQUEST
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:    // 解析请求头
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)    // 如果请求头已经解析完毕，且请求类型是 GET，返回 do_request 的返回值
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:   // 解析请求内容
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;    // 设置行解析状态为 OPEN，以便下次继续解析
            break;
        }
        default:
            return INTERNAL_ERROR;  // 如果解析状态错误，返回 INTERNAL_ERROR
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);  //将src复制到dest字符数组中
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');    //从str的右侧开始查找字符c首次出现的位置，并返回这个位置的地址。未能找到指定字符，那么函数将返回NULL。

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);  //将src指向的字符串追加到dest指向的字符串的结尾
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); //以src地址开始的前n个字节复制到dest所指的数组中，并返回dest
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    cout << "发送响应报文" <<endl;
    cout << m_write_buf << endl;
    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
//用于处理响应数据的发送
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:    // 如果出现内部错误，则发送 500 错误页面
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:   // 如果请求无法找到相应资源，则发送 404 错误页面
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:     // 如果请求资源被禁止，则发送 403 错误页面
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:  // 如果请求成功，则发送响应数据
    {
        add_status_line(200, ok_200_title);     // 发送 200 响应状态码和 OK 消息
        if (m_file_stat.st_size != 0)   // 如果请求的文件不为空，则发送文件内容
        {
            add_headers(m_file_stat.st_size);   // 添加响应头
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2; // 采用两块缓冲区发送数据
            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        }
        else    // 如果请求的文件为空，则返回空白页面
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    // 更新待发送数据信息
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();    // 进行读取请求处理，并返回相应的状态码
    if (read_ret == NO_REQUEST)             //NO_REQUEST，表示请求不完整，需要继续接收请求数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);   // 调用process_write完成报文响应
    if (!write_ret) // 如果写入失败，则关闭连接
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);   // 将套接字加入到epoll中等待下次写入响应
}
