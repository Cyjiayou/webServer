#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

//生成日志文件 && 判断写入方式
//异步需要设置阻塞队列的长度，同步不需要设置
//日志文件根据日期和路径决定
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;

        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    m_close_log = close_log;

    //输出内容的长度
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);

    //日志的最大行数
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);          //struct tm* localtime(timr_t* time)
    struct tm my_tm = *sys_tm;

    // struct tm {
    //     int tm_sec;
    //     int tm_min;
    //     int tm_hour;
    //     int tm_mday;
    //     int tm_mon;         //月份从 0 开始，所以要加 1
    //     int tm_yead;        //年份要加上 1900
    // }

    //从后往前找第一个 /
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    //相当于自定义日志名
    //若输入的文件名没有 /，则直接将 时间 + 文件名作为日志名
    if (p == NULL)
    {
        //strcpy(log_name, filename);
        //memset(dir_name, 0, sizeof(dir_name))
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        //将/的位置向后移动一个位置，然后复制到logname中
        //p - file_name + 1是文件所在路径文件夹的长度
        //dirname相当于./
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

/*
write_log 的工作过程：
    首先会判断日志等级
    其次会根据现在的时间判断是否是同一个文件
    如果是同一个文件则继续写，如果不是则立即将缓存中的字符输出，创建新的文件
    其次判断行数，如果当前的行数大于一个文件的最大行数，则立即将缓存中的字符输出
    然后创建新的文件，此时的文件是在之前文件后面添加序号
    最后判断日志输出方式：
        如果是异步，则将字符添加到队列中
        如果是同步，则直接输出
*/
void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
        case 0:
            strcpy(s, "[debug]:");
            break;
        case 1:
            strcpy(s, "[info]:");
            break;
        case 2:
            strcpy(s, "[warn]:");
            break;
        case 3:
            strcpy(s, "[erro]:");
            break;
        default:
            strcpy(s, "[info]:");
            break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();

    //更新行
    m_count++;

    //日志不是今天或写入的日志行数是否超过最大行限制
    //若为创建日志的时间，写入日志，否则按当前时间创建新 log，更新创建时间和行数
    //若行数超过最大行限制，在当前日志的末尾加 count /max_lines 为后缀创建新 log
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
       
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
       
        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
