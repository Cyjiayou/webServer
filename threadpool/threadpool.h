/**
 * @file threadpool.h
 * @author cy (you@domain.com)
 * @brief 
 * @version 0.1
 * @date 2022-03-30
 * 
 * @copyright Copyright (c) 2022
 * 
 * 
 * 线程池会先创建多个线程，并且每个线程调用一个静态成原函数 work【静态的原因？】，参数是线程池对象，为了在 work 函数中调用线程执行程序 run
 * run 程序主要失去请求队列中找任务，并处理该任务；
 * 线程池的 append 函数主要就是将任务添加进请求队列中；
 * 
 * 互斥：
 * 同一个时间段只有一个线程可以处理请求队列；同一个时间段线程池只能访问一次请求队列；
 * 
 * 同步：
 * 请求队列必须先有队列，线程才可以取出队列进行处理
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);

    //向请求队列中插入任务请求
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    //pthread_create() 函数的第三个参数要求传入一个处理函数，这个函数必须是静态函数，如果函数在类中，则需要将其设置为静态成员函数
    //原因是静态成员函数是没有 *this 指针的，而普通函数在运行时会将 *this 指针默认传入函数；
    static void *worker(void *arg);                         
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    int m_max_requests;         //请求队列中允许的最大请求数
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
    connection_pool *m_connPool;  //数据库【需要数据库是为了获得一个数据库连接，传递给http进行用户检测】
    int m_actor_model;          //模型切换
};
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    
    //线程数组初始化
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();

    
    for (int i = 0; i < thread_number; ++i)
    {
        //将对象作为参数传递给 worker 函数，为了调用运行函数
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)     //成功时返回 0，失败时释放数组空间
        {
            delete[] m_threads;
            throw std::exception();
        }
        //pthread_detach 是非阻塞的，成功时返回 0
        if (pthread_detach(m_threads[i]))       
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}


template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}


//生产者
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();             
    return true;
}

//当监听的 socket 发生了读写事件，将任务插入到请求队列中
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    //添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //提醒有任务需要处理
    m_queuestat.post();
    return true;
}


template <typename T>
void *threadpool<T>::worker(void *arg)      //参数类型是线程池 
{
    threadpool *pool = (threadpool *)arg;   
    pool->run();                            //每个线程都取出请求队列中的任务，执行任务；run 函数将这个操作封装
    return pool;
}

//消费者
//工作线程从请求队列中取出某个任务进行处理
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();             //判断是否有任务
        m_queuelocker.lock();           //对请求队列进行处理，加队列锁
        if (m_workqueue.empty())        //如果队列为空
        {
            m_queuelocker.unlock();
            continue;
        }
        //request 是 http_conn 对象
        T *request = m_workqueue.front();       //取出第一个事件
        m_workqueue.pop_front();                
        cout << request->m_sockfd << " get pool"<< endl;
        m_queuelocker.unlock();                 //释放队列锁
        if (!request)
            continue;
        if (1 == m_actor_model)                 //模式选择，reactor 模式
        {
            if (0 == request->m_state)          //事件的状态，0 代表读
            {
                if (request->read_once())       //读取事件内容，读取成功
                {
                    request->improv = 1;        
                    //从连接池中去除一个连接传递给 http_conn 的 mysql 对象
                    //处理 http_conn 的 process
                    connectionRAII mysqlcon(&request->mysql, m_connPool);   //连接数据库
                    request->process();         //事件处理
                }
                else
                {
                    request->improv = 1;        
                    request->timer_flag = 1;   
                }
            }
            else                                //1 代表写
            {
                if (request->write())           //写成功
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else                                    //如果是 proactor 
        {
            //线程池不仅要处理 process，还要给 http_conn 分配一个数据库连接，所以需要数据库连接池这个变量
            connectionRAII mysqlcon(&request->mysql, m_connPool);       //连接数据库
            request->process();                                         //请求报文处理
        }
    }
}
#endif
