#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

class connection_pool
{
public:
	MYSQL *GetConnection();				 	//获取数据库连接
	bool ReleaseConnection(MYSQL *conn); 	//释放连接
	int GetFreeConn();					 	//获取连接
	void DestroyPool();					 	//销毁所有连接，没有直接调用，而是同过 RAII 机制完成自动释放

	//单例模式
	static connection_pool *GetInstance();

	//创建连接
	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); 

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;  			//最大连接数
	int m_CurConn;  			//当前已使用的连接数
	int m_FreeConn; 			//当前空闲的连接数
	locker lock;				//锁
	list<MYSQL *> connList; 	//连接池
	sem reserve;				//信号量

public:
	string m_url;			 	//主机 IP 地址
	string m_Port;		 		//主机端口号
	string m_User;		 		//登陆数据库用户名
	int m_PassWord;	 			//登陆数据库密码
	string m_DatabaseName; 		//使用数据库名
	int m_close_log;			//日志开关
};

class connectionRAII{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif
