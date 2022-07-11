#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	//初始化数据库信息
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	//创建 MaxConn 条数据库链接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		// MYSQL* mysql_init(MYSQL* mysql);
		con = mysql_init(con);		//初始化一个 MySQL 对象

		if (con == NULL)
		{
			//日志输出
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		//连接一个 MySQL 服务器
		//MYSQL* musql_real_connect(MYSQL* mysql, const char* host, const char* user, const char* password, const char* db, unsigned int port, const char* UNIX_SOCKET, unsigned long client_flag);
		//host 为 NULL 表示连接的是本地主机，root 用户默认没有密码，db 为空表示连接到默认的数据库
		//unix_socket 为 NULL，表明不使用 socket 或管道机制，最后一个参数常设置为 0
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		//更新连接池和空闲连接数量
		connList.push_back(con);
		++m_FreeConn;
	}

	//将信号量初始值设置为控线连接数量，又最大连接数量
	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	//这里应该加锁
	lock.lock();
	if (0 == connList.size()) {
		lock.unlock();
		return NULL;
	}

	reserve.wait();
	
	lock.lock();

	con = connList.front();
	connList.pop_front();

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接，将使用完的连接放入队列中
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);			//关闭一个 MySQL 服务器连接
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}


//RAII 机制销毁线程池
connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}