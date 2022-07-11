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
    static const int FILENAME_LEN = 200;            //文件长度
    static const int READ_BUFFER_SIZE = 2048;       //读缓存大小
    static const int WRITE_BUFFER_SIZE = 1024;      //写缓存大小
    enum METHOD                                     //报文的请求方式，主要是 GET 和 POST
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
    enum CHECK_STATE                                //主状态机的状态【请求行、头部、消息体】
    {
        CHECK_STATE_REQUESTLINE = 0,                //解析请求行
        CHECK_STATE_HEADER,                         //解析请求头部
        CHECK_STATE_CONTENT                         //解析消息体，仅用于 POST 请求
    };
    enum HTTP_CODE                              //报文解析状态，重点
    {
        NO_REQUEST,             //请求不完整，需要继续读取请求报文
        GET_REQUEST,            //获得了完整的 HTTP 请求
        BAD_REQUEST,            //HTTP 请求报文有语法错误
        NO_RESOURCE,            //资源不存在
        FORBIDDEN_REQUEST,      //没有权限
        FILE_REQUEST,           //资源请求正常
        INTERNAL_ERROR,         //服务务器内部错误，该结果在主状态机逻辑 switch 的 default 下，一般不会触发
        CLOSED_CONNECTION       //
    };
    enum LINE_STATUS            //从状态机的状态，标识解析一行的读取状态
    {
        LINE_OK = 0,            //完整读取一行
        LINE_BAD,               //报文语法有误
        LINE_OPEN               //读取的行不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();

    //读取浏览器端发送的请求报文，直到无数据可读或对方关闭连接，读取到 m_read_buffer 中，并更新 m_read_index
    bool read_once();

    //响应报文写入函数
    bool write();
    
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    
    //同步线程初始化数据库读取表
    //将以前注册过的用户名和密码读取并保存
    void initmysql_result(connection_pool *connPool);
    int timer_flag;         //？
    int improv;             //？


private:
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();

    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);

    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);

    //生成响应报文
    HTTP_CODE do_request(); 
    
    //m_read_buf 是已经解析的字符
    //get_line 用于将指针向后偏移，指向未处理的字符
    //m_start_line 是行在 buffer 中的起始位置，将该位置后面的数据赋给 text
    char *get_line() { return m_read_buf + m_start_line; };    

    //从状态机读取一行，分析是请求报文的哪一部分 
    LINE_STATUS parse_line();

    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       //epoll文件描述符
    static int m_user_count;        //用户数量
    MYSQL *mysql;           //连接数据库
    int m_state;  //读为0, 写为1    reactor 模式

private:
    int m_sockfd;                   //客户端套接字
    sockaddr_in m_address;          //地址

    //存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    int m_read_idx;
    //m_read_buf读取的位置m_checked_idx
    int m_checked_idx;
    //每行开始
    int m_start_line;

    //存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    //指示buffer中的长度
    int m_write_idx;

    //主状态机的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;

    //以下为解析请求报文中对应的6个变量
    //存储读取文件的名称、URL、版本、主机、长度、长连接
    char m_real_file[FILENAME_LEN];  
    char *m_url;                    
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;                  //是不是长连接

    char *m_file_address;           //读取服务器上的文件地址
    struct stat m_file_stat;        //文件类型判断
    struct iovec m_iv[2];           //io向量机制iovec
    int m_iv_count;
    int cgi;                        //是否启用的POST
    char *m_string;                 //存储请求头数据
    int bytes_to_send;              //剩余发送字节数
    int bytes_have_send;            //已发送字节数
    char *doc_root;                 //网站的根目录

    map<string, string> m_users;    //记录用户名和密码
    int m_TRIGMode;                 //
    int m_close_log;                //是否开启日志

    //注意这里的类型是 char
    char sql_user[100];             //sql 用户名
    char sql_passwd[100];           //密码
    char sql_name[100];             //数据库名字
};

int http_conn::m_user_count = 0;        //静态变量初始化
int http_conn::m_epollfd = -1;

#endif
