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
	connection_pool(const connection_pool&) = delete;				// 单例模式: 禁用拷贝构造
    connection_pool& operator=(const connection_pool&) = delete;	// 单例模式: 禁用赋值(原来写的不是单例, 我把它改正了)
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	static connection_pool *GetInstance();	// 单例模式: 静态函数创建对象

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); 

private:
	connection_pool();		// 单例模式: 私有构造函数
	~connection_pool();

	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	locker lock;
	list<MYSQL *> connList; // 空闲连接(被使用则从链表中移除)
	sem reserve;

public:
	string m_url;           // 主机地址
	string m_Port;          // 数据库端口号
	string m_User;          // 登陆数据库用户名
	string m_PassWord;      // 登陆数据库密码
	string m_DatabaseName;  // 使用数据库名
	int m_close_log;        // 日志开关
};

// 数据库连接API封装成类
class connectionRAII{

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif
