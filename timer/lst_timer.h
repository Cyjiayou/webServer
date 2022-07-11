#ifndef LST_TIMER
#define LST_TIMER

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

#include <time.h>
#include "../log/log.h"

//连接资源结构体成员需要用到定时器类
//需要向前声明
class util_timer;

//连接资源
struct client_data
{
    //客户端 sokcet 地址
    sockaddr_in address;

    //客户端文件描述符
    int sockfd;

    //定时器
    util_timer *timer;
};


//定时器类
//将资源连接、超时时间、定时事件封装为定时器类；定时事件为回调函数，这里是删除非活动 socket 上的注册事件，并关闭
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //超时时间
    time_t expire;
    
    //回调函数，从内核事件表删除事件，关闭文件描述符，释放连接资源；
    void (* cb_func)(client_data *);
    
    //连接资源
    client_data *user_data;
    
    //前向定时器
    util_timer *prev;

    //后继定时器
    util_timer *next;
};

//定时器容器【带头尾结点的升序双向链表】，为每个连接创建一个定时器，将其添加到链表中，并按照超时时间升序排列。执行定时任务时，将到期的定时器从链表中删除
//添加特定定时器的复杂度 O(n)，删除定时器的复杂度 O(n)
class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);

    //定时任务处理函数，处理链表容器中到期的定时器
    void tick();

private:
    //从 lst_head 处插入 timer 
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;       //头结点
    util_timer *tail;       //尾结点
};


//信号处理
class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;           //管道号
    sort_timer_lst m_timer_lst;     //排好序的定时容器
    static int u_epollfd;           //epoll 套接字
    int m_TIMESLOT;                 //时间
};

void cb_func(client_data *user_data);

#endif
