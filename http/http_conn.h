#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;        // 读取文件的名称m_real_file长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区m_write_buf大小
    enum METHOD     // 报文的请求方法，本项目只用到GET和POST
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE    // 主状态机的状态
    {
        CHECK_STATE_REQUESTLINE = 0,    // 解析请求行
        CHECK_STATE_HEADER,             // 解析请求头
        CHECK_STATE_CONTENT             // 解析消息体，仅用于解析POST请求
    };
    enum HTTP_CODE      // 报文解析的结果
    {
        NO_REQUEST,         // 报文未解析完，需要继续读取请求报文数据并解析
        GET_REQUEST,        // 获得了完整的HTTP请求
        BAD_REQUEST,        // HTTP请求报文有语法错误
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,     // 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION
    };
    enum LINE_STATUS        // 从状态机的状态
    {
        LINE_OK = 0,        // 完整读取一行
        LINE_BAD,           // 报文语法有误
        LINE_OPEN           // 读取的行不完整
    };

public:
    http_conn() {}  // 不知道为啥不用无参构函和赋值构函(然后不让调无参构函不就好了)
    ~http_conn() {}

public:
    // 赋值构函
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);            // 关闭http连接
    void process();                                     // 处理请求
    bool read_once();                                   // 读取浏览器端发来的全部数据
    bool write();                                       // 响应报文写入函数
    sockaddr_in *get_address() { return &m_address; }   // 获取socket地址
    void initmysql_result(connection_pool *connPool);   // 同步线程初始化数据库读取表
    int timer_flag;
    int improv;


private:
    // 初始化默认成员变量
    void init();
    HTTP_CODE process_read();                           // 从m_read_buf读取，并处理请求报文
    bool process_write(HTTP_CODE ret);                  // 向m_write_buf写入响应报文数据
    HTTP_CODE parse_request_line(char *text);           // 主状态机解析报文中的请求行数据
    HTTP_CODE parse_headers(char *text);                // 主状态机解析报文中的请求头数据
    HTTP_CODE parse_content(char *text);                // 主状态机解析报文中的请求内容
    HTTP_CODE do_request();                             // 生成响应报文
    // m_start_line是已经解析的字符, get_line用于将指针向后偏移，指向未处理的字符
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();                           // 从状态机读取一行，分析是请求报文的哪一部分
    void unmap();

    // 根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  // 读为0, 写为1

private:
    int m_sockfd;                                       // socket(文件描述符)
    sockaddr_in m_address;                              // socket地址
    char m_read_buf[READ_BUFFER_SIZE];                  // 存储读取的请求报文数据
    long m_read_idx;                                    // [ ] TODO: m_read_buf中数据的最后一个字节的下一个位置
    long m_checked_idx;                                 // m_read_buf读取的位置m_checked_idx
    int m_start_line;                                   // m_read_buf中已经解析的字符个数
    char m_write_buf[WRITE_BUFFER_SIZE];                // 存储发出的响应报文数据
    int m_write_idx;                                    // 指示buffer中的长度
    CHECK_STATE m_check_state;                          // 主状态机的状态
    METHOD m_method;                                    // 请求方法

    // 以下为解析请求报文中对应的6个变量, 存储读取文件的名称
    char m_real_file[FILENAME_LEN];  
    char *m_url;                                        // url
    char *m_version;                                    // http版本号
    char *m_host;                                       // 主机名
    long m_content_length;                              // 请求体长度, 由请求头的Content-length字段指定(一般是POST有)
    bool m_linger;                                      // 是否保持连接(keep-alive)

    char *m_file_address;                               // 读取服务器上的文件地址
    struct stat m_file_stat;
    struct iovec m_iv[2];                               // io向量机制iovec
    int m_iv_count;
    int cgi;                                            // 是否启用的POST
    char *m_string;                                     // 存储请求体数据
    int bytes_to_send;                                  // 剩余发送字节数
    int bytes_have_send;                                // 已发送字节数
    char *doc_root;

    map<string, string> m_users;
    int m_TRIGMode;                                     // 触发方式, 0: LT, 1: ET
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
