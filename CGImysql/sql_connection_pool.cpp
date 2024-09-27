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
	static connection_pool connPool;	// 懒汉式，第一次调用时创建
	return &connPool;
}

// 数据库连接池初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;				// 数据库url(localhost, 因为服务器运行在本地)
	m_Port = Port;				// 数据库端口号(mysql是3306)
	m_User = User;				// 数据库用户名
	m_PassWord = PassWord;		// 数据库密码
	m_DatabaseName = DBName;	// 数据库名称
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		// 初始化连接
		con = mysql_init(con);
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		// 建立一个到mysql数据库的连接
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}

		connList.push_back(con);	// 放入数据库连接池
		++m_FreeConn;		// 信号量初值++
	}

	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();	// P, await, reserve--
	
	lock.lock();	// connList是全局的, 需要加锁

	con = connList.front();
	connList.pop_front();	// 删除链表中第一个元素

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();		// V, release, reserve++
	return true;
}

//销毁数据库连接池(感觉这样写很容易泄露啊(可能池就是这样？取一瓢取一瓢不还就是会干，是取而不是借))
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			// 销毁数据库连接
			mysql_close(con);
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

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();	// 出参
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}
