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

//将数据库中的用户名和密码载入到服务器的 map 中来
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    //bool mysql_query(MYSQL* mysql, const char* sql);
    //sql 是一条 mysql 查询语句
    //mysql_query 只能判断查询语句是否有效
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    //MYSQL_RES* mysql_store_result(MYSQL*)
    //将 mysql_query 查询到的结果保存到 MYSQL_RES 变量中
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    //int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    //MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    //MYSQL_ROW mysql_fetch_row(MYSQL_RES*)
    //将 MYSQL_RES 变量中的一行赋值给 MYSQL_ROW
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//下面 4 个函数不属于 http_conn 类
//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT，ePOLLONESHOT 针对客户端
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);     //客户端设置为非阻塞
}

//从内核事件表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);          //关闭套接字
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    //重置的时候，不需要设置 in 和 out
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}


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

//初始化连接,外部调用初始化套接字地址
//root是工作目录
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;
    m_TRIGMode = TRIGMode;
    
    addfd(m_epollfd, sockfd, true, m_TRIGMode);     //one_shot = true
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空  ？
    doc_root = root;
    
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;        //默认是处理请求
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
    m_state = 0;            //状态，输入还是输出
    timer_flag = 0;         //
    improv = 0;             //是否完成

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容；每一行的数据由 \r\n 结尾，空行则是 \r\n；
//从状态机负责读取 buffer 中的数据，将每行数据末尾的 \r\n 设置为 \0\0，并更新从状态机在 buffer 中读取的位置 m_checked_index
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')                                       //从 m_read_buf 中逐行读取，判断 /r
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';             //其实 m_checked_idx 指向下一行的开头
                return LINE_OK;
            }
            return LINE_BAD;
        }
        //这种情况一般是上一次读取到 \r 就到末尾了，没有接收完整，此时判断上一个字符是不是 \r
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
//read_once 读取浏览器发送来的请求报文，直到无数据可读或对方关闭连接，读取到 m_read_buffer 中
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
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
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text, " \t");           //strpbrk(const char* s1, const char* s2)       返回 s1 中首次出现 s2 中元素的下标
    if (!m_url)                     //如果没有的话，说明格式错误
    {
        return BAD_REQUEST;
    }

    *m_url++ = '\0';    
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;                            //post 默认 cgi 为 1
    }
    else
        return BAD_REQUEST;
    
    //这里是为了将空格个 \t 取出，得到下一个部分
    m_url += strspn(m_url, " \t");          //size_t strspn(char *str1, char *str2);    返回 str1 中连续出现 str2 中元素的个数
    m_version = strpbrk(m_url, " \t");      //和上面一行
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';                    //此时 m_url 指向 url

    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)     //确定版本号
        return BAD_REQUEST;

    if (strncasecmp(m_url, "http://", 7) == 0)      
    {
        m_url += 7;
        //http:// 后面紧跟着就是目标地址和目标端口号，我们这里不需要知道，需要知道 目录和额外数据
        m_url = strchr(m_url, '/');         //char * strchr(char * str, char c);	返回字符 c 在 str 中第一次出现的位置
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    //一般都是 / 后面直接跟访问的资源位置，至少是一个 /
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
        
    //当url为 / 时，直接追加目标文件名
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;          //请求不完整
}

//解析http请求的一个头部信息
//分析请求头部和分析空行使用的是一个函数
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //这里是判断是不是空行
    if (text[0] == '\0')
    {
        //判断是不是 POST 方式，POST 方式 content_length 不是 0
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }

    //解析头部
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        //跳过空格和 \t 字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)        //长连接
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);              //获得 content 长度
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else            //其他字段打印日志
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
//服务器端解析浏览器的请求报文，当解析为 POST 请求时，cgi 标志位设置为 1，并将请求报文的消息体
//赋值给 m_string，进而取出用户名和密码
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))       //这里可以使用 m_checked_idx，也可以使用 m_start_idx
    {
        text[m_content_length] = '\0';                          //将最后一个元素设置为 0
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//请求报文解析，有限状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    //设置从状态机的状态
    LINE_STATUS line_status = LINE_OK;
    //解析结果
    HTTP_CODE ret = NO_REQUEST;

    char *text = 0;

    //这里为什么要写两个判断条件？第一个判断条件为什么这样写？
    //为了 post 进入循环，对于 content，我们不需要读取行了【内容中可能存在 \r\n】
    //为了 post 退出循环
    //content 之前，line_status 是 LINE_Ok
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();                  //读取一行，return m_read_buf + m_start_line;   m_start_line 是每一行的开头

        //parse_line() 执行后，m_checked_idx 指向下一行的开头，这里将 m_start_line 重新赋值
        //分析 content 的时候，不会调用 parse_line，此时 m_checked_idx 并不会发生变化，和 start_line 指向 content 的第一行
        m_start_line = m_checked_idx;

        LOG_INFO("%s", text);

        //主状态机的三种状态转换逻辑
        //解析函数会自动更新主状态机的状态
        //主状态机只会返回 BAD_REQUEST、GET_REQUEST、NO_REQUEST
        //返回 NO_REQUEST 表示这个主机状态解析完毕，继续下一个状态的解析
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                //分析请求行
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) 
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:
            {
                //分析请求头和空行
                //第一次执行的时候只是分析头部，此时主要记录 content_length，并不发生状态变化
                //下一次循环会继续执行这里，此时会根据 content_length 判断是 get 还是 post
                ret = parse_headers(text);      
                if (ret == BAD_REQUEST)         //语法有误
                    return BAD_REQUEST;

                //完整解析 GET 请求后，跳转到报文响应函数，如果是 get 直接调用 do_request()
                else if (ret == GET_REQUEST)    
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:       //处理 POST 请求
            {
                //解析消息体
                ret = parse_content(text);

                //完整解析POST请求后，跳转到报文响应函数
                if (ret == GET_REQUEST)
                    return do_request();

                //解析完消息体后即完成报文解析，避免进入循环，更新 line_status【很重要】
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
            }
    }
    return NO_REQUEST;
}

//将网站根目录和 url 文件拼接起来，然后通过 stat 判断文件属性
//doc_root  文件夹内存放请求的资源和跳转的 html 文件
http_conn::HTTP_CODE http_conn::do_request()
{
    //初始化 m_read_file 为网站根目录
    strcpy(m_real_file, doc_root);      
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    //这里是为了找文件名
    const char *p = strrchr(m_url, '/');        //strtchr(const char* str, const char* ch) 用于查找 ch 在 str 中最后一次出现的位置

    //实现登录和注册
    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        //char flag = m_url[1];

        //char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        //strcat(m_url_real, m_url + 2);      //m_url + 2 后面是 cgi 程序 
        //strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        //free(m_url_real);

        //将用户名和密码提取出来
        //content 的内容是 name=123&password=123
        char name[100], password[100];
        int i;
        //从第5位开始是名称
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        //从 & 处加 10 位就是密码位置
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        //注册的情况
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //没找到同名的
            if (users.find(name) == users.end())
            {
                //向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                //校验成功跳转到登录界面
                if (!res)
                    strcpy(m_url, "/log.html");
                //校验失败跳转到注册失败界面
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
            //在 user 中找到，并且密码是对的，跳转到欢迎页面
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            //否则跳转到登陆失败的页面
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //如果请求资源为 /0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //如果请求资源为 /1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //如果请求资源为 /5，即图片请求页面
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //如果请求资源为 /6，即视频请求页面
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //如果请求资源为 /7，即图关注页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }

    //这里的情况是 url 是 /，此时是 GET 请求，跳转到judge.html，即欢迎访问页面
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过 stat 获取请求资源文件信息，成功则将信息更新到 m_file_stat 结构体
    //失败返回 NO_RESOURCE 状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回 FORBIDDEN_REQUEST 状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    //判断文件类型，如果是目录，则返回 BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
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

/* 
    服务器子线程调用 proces_write 完成响应报文，随后注册 epollout 事件；服务器主线程检测写事件，并调用 http_conn::write 函数将响应报文发送给浏览器
*/
bool http_conn::write()
{
    int temp = 0;

    //要发送的数据长度为 0，表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //发送不成功，判断是不是写缓冲区满了
        //如果写缓冲区满了，此时电平为低电平
        //等待缓冲区能发送数据，此时为高电平
        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                //这句话可以没有？
                //modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //如果是其他错误，则退出
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        //更改 iov_base 和 iov_len
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }


        //发完了
        if (bytes_to_send <= 0)
        {
            unmap();
            //关闭写事件，监听读事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                //初始化客户端信息
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

//调用 add_response 函数更新 m_write_idx 指针和缓冲区 m_write_buf 中的内容
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    
    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);

    //将数据 format 从可变参数列表写入换冲区，返回写入长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    //更新 m_write_idx 位置
    m_write_idx += len;
    va_end(arg_list);       //回收参数列表指针 arg_list

    LOG_INFO("request:%s", m_write_buf);

    return true;
}


bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn:: add_content_type()
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

//根据返回类型，服务器子线程调用 process_write 向 m_write_buf 中写入相应报文，如果有资源
//add_status_line 函数，添加状态行：http/1.1 状态码 消息体
//add_headers 函数添加消息报文，内部调用 add_content_length 和 add_linger 函数
//  content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
//  connection记录连接状态，用于告诉浏览器端保持长连接
//add_blank_line 添加空行
//add_content 函数添加内容，这里的内容放在 m_write_buf 中
//如果是正常的情况，内容是保存在一个 text 中，然后将其映射到一块内存中 m_file_address
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
            //如果请求的资源存在
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                //第一个 iovec 指针指向响应报文缓冲区，长度指向 m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //第二个 iovec 指针指向 mmap 返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                //如果请求的资源为 0，则返回空白 html 文件
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    //报文解析
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST 表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST)
    {
        //将数据重置为 EPOLLONESHOT 并继续接收
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }

    //注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}