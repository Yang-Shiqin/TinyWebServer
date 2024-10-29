#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

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

locker m_lock;
map<string, string> users;

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

    //从结果集中不断获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    // fcntl: 获取或设置fd的属性(int, 属性通过位来存储)
    int old_option = fcntl(fd, F_GETFL);    // 获取fd的属性
    int new_option = old_option | O_NONBLOCK;   // 新属性增加非阻塞设置
    fcntl(fd, F_SETFL, new_option);         // 设置fd新属性
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)  // ET
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  // EPOLLET: 使用边缘触发模式(ET)
    else                // LT
        event.events = EPOLLIN | EPOLLRDHUP;    // EPOLLIN: 监听可读事件, EPOLLRDHUP: 监听被挂起事件

    if (one_shot)
        event.events |= EPOLLONESHOT;   // 只监听一次事件, 因为我们希望每个socket在任意时刻都只被一个线程处理
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);      // 注册事件
    setnonblocking(fd);
}

// epfd内核对象删除socket
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    // [ ] TODO: 为啥不是ev | EPOLLONESHOT就行了
    if (1 == TRIGMode)  // ET
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else                // LT
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;    // 客户量(除了0、1还有别的值吗)
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接, 外部调用初始化socket地址(为啥不写在构函里啊)
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 重载私有, 初始化默认初值的私有成员变量, check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容是否完整或错误(通过查找\r\n)
// 返回值为行的读取状态，有LINE_OK(完整的一行),LINE_BAD(错误),LINE_OPEN(未完整需继续读)
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)  // \r后面没有字符, 需要继续读
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n') // 完整的\r\n, 则为完整一行, 把\r\n变成\0\0
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;    // \r后面不是\n, 语法错误
        }
        else if (temp == '\n')  // 上次读到\r就结束了会遇到这种情况
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') // \r\n
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;   // 没有找到\r\n, 需要继续读
}

// 读取一次用户数据, 存入m_read_buf(非阻塞ET工作模式会在这次读完; LT只读一次, 下次调用再继续读)
// 返回读取成功与否
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;       // buffer放不下了, 读取失败
    }
    int bytes_read = 0;

    //LT读取数据(一次读一部分, 剩下的下次调用read_once()再读)
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)    // 读取失败
        {
            return false;
        }
    }
    //ET读数据(一次性读完)
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                // 如果错误原因是 EAGAIN 或 EWOULDBLOCK 则不算读取失败
                // EAGAIN: 在非阻塞模式下, 当前没有可用数据(就是此时数据已经被读完了)
                // EWOULDBLOCK: 与 EAGAIN 等价
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
    }
    return true;
}

// 解析http请求行, 相应值放入m_url, m_method, m_version, 返回解析结果
// 请求行: `请求方法|空格|url|空格|http版本号|\r\n`
// text是经过parse_line()处理后的一行数据, \r\n已经被处理成\0了
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");   // 在text中找到第一个匹配' '或'\t'的指针
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';    // 空格变成0, 指向下一位(method变成一个字符串了)
    char *method = text;
    if (strcasecmp(method, "GET") == 0) // 忽略大小写判断字符串是否相等
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    } else          // 目前只支持GET和POST
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");  // 去掉多余的空格(返回 str1 中第一个不在字符串 str2 中出现的字符下标)
    m_version = strpbrk(m_url, " \t");  // 找到http版本号前的第一个空格
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';    // 空格变成0, 指向下一位(m_url变成一个字符串了)
    m_version += strspn(m_version, " \t");  // 去掉多余的空格
    if (strcasecmp(m_version, "HTTP/1.1") != 0) // 目前只支持HTTP/1.1
        return BAD_REQUEST;
    // 获取url的协议部分
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');     // 跳过host:port
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1) // 如果是根路径, 显示judge.html
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; // 状态迁移: 请求行解析 -> 请求头解析
    return NO_REQUEST;
}

// 解析一行的http请求头
// 一行请求头(请求头可以有多行): `字段名|:|值|\r\n`
// text是经过parse_line()处理后的一行数据, \r\n已经被处理成\0了
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 空行, 表示请求头解析完毕
    if (text[0] == '\0')
    {
        if (m_content_length != 0)  // POST请求, 还有请求体要读
        {
            m_check_state = CHECK_STATE_CONTENT;    // 状态迁移: 请求头解析 -> 请求体解析
            return NO_REQUEST;
        }
        return GET_REQUEST; // 没有请求体, 则解析完毕
    }
    // 继续解析请求头
    else if (strncasecmp(text, "Connection:", 11) == 0) // 保持连接
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0) // 请求体长度
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)    // 主机名
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else    // 其他字段名不解析
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 解析请求体(直接复制就行, 就是用户数据)判断http请求是否被完整读入, 内容放入m_string
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

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 等待到接收到完整的一行(请求体没读完整也会一直parse_line)
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();  // 获取本行行首位置
        m_start_line = m_checked_idx;   // 更新为下行行首位置
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:   // 解析请求行
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:        // 解析请求头
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)    // GET, 没有请求体, 请求完毕
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:       // 解析请求体
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;    // 没读完整, 继续读
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

/* 根据解析内容确定响应页面路径m_real_file, 返回http响应码
 * m_url为请求报文中解析出的请求资源，以/开头，即ip:port/xxx中的/xxx。项目中分为以下8种：
 * /            : GET请求，跳转到judge.html，即欢迎访问页面
 * /0           : POST请求，跳转到register.html，即注册页面
 * /1           : POST请求，跳转到log.html，即登录页面
 * /2CGISQL.cgi : POST请求，进行登录校验. 验证成功跳转到welcome.html，即资源请求成功页面; 验证失败跳转到logError.html，即登录失败页面
 * /3CGISQL.cgi : POST请求，进行注册校验. 注册成功跳转到log.html，即登录页面; 注册失败跳转到registerError.html，即注册失败页面
 * /5           : POST请求，跳转到picture.html，即图片请求页面
 * /6           : POST请求，跳转到video.html，即视频请求页面
 * /7           : POST请求，跳转到fans.html，即关注页面
 */
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi(`/2CGISQL.cgi` 登录校验或 `/3CGISQL.cgi` 注册校验)
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");        // m_url_real="/"
        strcat(m_url_real, m_url + 2);  // m_url_real="/CGISQL.cgi"
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); // m_real_file="<代码运行根目录>/root/CGISQL.cgi"
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        // 提取name
        int i;
        for (i = 5; m_string[i] != '&'; ++i)    // 跳过 `user=`
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        // 提取password
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //如果是注册，先检测数据库中是否有重名的
        //没有重名的，进行增加数据
        if (*(p + 1) == '3')
        {
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

    // 跳转到register.html，即注册页面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 跳转到log.html，即登录页面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 跳转到picture.html，即图片请求页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 跳转到video.html，即视频请求页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 跳转到fans.html，即关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else    // 其他请求, 跳转到对应页面; /则跳转judge.html; cgi完的请求跳转到对应页面
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    // 失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
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
// 发送响应报文
bool http_conn::write()
{
    int temp = 0;

    // 若要发送的数据长度为0, 表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端, temp为已发送数据长度
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            // 判断缓冲区是否满了
            if (errno == EAGAIN)
            {
                // 重新注册写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }

        // 更新已发送字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;
        // 第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // 继续发送第一个iovec头部信息的数据
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();
            // 在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            // 浏览器的请求为长连接
            if (m_linger)
            {
                // 重新初始化HTTP对象
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

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
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
// 根据服务器处理HTTP请求的结果，生成响应报文
// 第一个iovec存放响应报文, 第二个iovec存放请求文件(如果有)
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            // 第一个iovec指针指向响应报文缓冲区
            m_iv[0].iov_base = m_write_buf; // iov_base指向存放writev将要发送数据的缓冲区
            m_iv[0].iov_len = m_write_idx;  // iov_len表示实际长度
            // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;  // 请求文件存在则用第二个指向请求文件地址
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            // 发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
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
    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// http报文解析与响应
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();    // 报文解析
    // NO_REQUEST表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        // 注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);   // 报文响应
    if (!write_ret)
    {
        close_conn();
    }
    // 注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
