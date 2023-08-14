#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <string>
#include <iostream>
#include "locker.h"     // 锁
#include "threadpool.h" // 线程池
#include "http_conn.h"  // 有限状态机实现HTTP协议的请求与解析 
#include "lst_timer.h"  // 升序双向链表实现定时器
#include "log.h"        // 同步/异步日志系统

#define MAX_FD 65535      // 最多的http连接
#define MAXEVENTS 10000   // 最大的事件数
#define TIME_SLOT 5       // 定时器触发时间片

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

static int pipefd[2];     // 管道
static sort_timer_lst timer_lst; // 定时器链表
static int epfd;         

//添加信号捕捉
int addsig(int sig,void (handler)(int))
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig,&sa,NULL);
}

// 信号处理函数
void sig_handler(int sig)
{
    int save_errno=errno;
    int msg=sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno=save_errno;
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    //定时处理任务，实际就是调用tick()函数
    timer_lst.tick();
    //一次arlarm调用只会引起一次SIGALARM信号，所以重新设闹钟
    alarm(TIME_SLOT);
}

// 定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void call_back(client_data* user_data)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    //assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    //printf("关闭连接\n");
}

//添加文件描述符到epoll中
//删除文件描述符从epoll中
//修改文件描述符从epoll中，主要是重置epoll oneshot
extern  int addfd(int epfd,int fd,uint32_t events);
extern int delfd(int epfd,int fd);
extern int modfd(int epfd,int fd,uint32_t events);
extern void setnoblocking(int fd);

int main(int argc,char* argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if(argc!=2)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        exit(-1);
    }
    //对SIGPIE信号处理,忽略它-客户端尝试写入一个已关闭的连接,忽略SIGPIPE信号可以促使服务器继续运行
    addsig(SIGPIPE,SIG_IGN);

    //对SIGALRM、SIGTERM设置信号处理函数
    addsig(SIGALRM,sig_handler);
    addsig(SIGTERM,sig_handler);

    /*
    尝试创建一个名为 pool 的线程池,用于管理 http_conn 类型的任务。
    如果创建线程池的过程中出现了异常，程序将会以非正常状态退出
    */
    threadpool<http_conn> * pool = NULL;
    try{
        pool=new threadpool<http_conn>;
    }
    catch(...)
    {
        exit(-1);
    } 

    // 创建一个数组保存http_conn
    http_conn *user=new http_conn[MAX_FD];

    // 创建监听套接字
    /*
    使用 socket 函数创建一个 IPv4 的流式套接字（TCP 套接字），
    返回的 listenfd 是该套接字的文件描述符。
    */
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    
    //设置端口复用
    /*
    设置套接字选项，其中 SO_REUSEADDR 表示允许地址重用。这样做可以避免在程序重启后，
    之前绑定的端口仍然处于 TIME_WAIT 状态，从而可以立即重新绑定该端口。
    */
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    struct sockaddr_in address;   // 定义了一个 IPv4 地址结构
    memset(&address,0,sizeof(address)); // 将 address 结构体清零
    address.sin_family=AF_INET;  // 设置地址族为 IPv4
    address.sin_addr.s_addr=INADDR_ANY; // 监听任意可用的网络接口
    address.sin_port=htons(atoi(argv[1])); // 设置监听的端口号，atoi(argv[1]) 将命令行参数转换为整数作为端口号
    bind(listenfd,(struct sockaddr*)&address,sizeof(address)); // 将套接字与地址绑定

    // listen
    listen(listenfd,10); // 开始监听连接，参数 10 表示可以同时处理的等待连接队列的最大长度

    // 创建epoll实例->创建内核事件表
    epfd = epoll_create(1); // 创建一个 epoll 实例，返回的 epfd 是 epoll 的文件描述符
    epoll_event events[MAXEVENTS]; // 定义一个数组来存储 epoll 事件
   
    addfd(epfd,listenfd,EPOLLIN); // 调用自定义的 addfd 函数，将 listenfd 添加到 epoll 实例中监听输入事件（EPOLLIN）。
    http_conn:: m_epollfd=epfd; // 将 epfd 赋值给自定义的 http_conn 类中的静态成员变量 m_epollfd，可能是为了在后续的代码中使用这个 epoll 实例

    // 创建管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    //assert( ret != -1 );
    setnoblocking( pipefd[1] );
    addfd(epfd, pipefd[0] ,EPOLLIN|EPOLLET); // 边缘触发模式

    bool timeout=false; 
    client_data* client_users=new client_data[MAX_FD]; // ?
    alarm(TIME_SLOT); //定时器:在 TIME_SLOT 秒后触发一个 SIGALRM 信号。这个信号可以用于实现定时的任务，比如定时检查某种条件、执行特定的操作

    bool stop_server=false;
    
   while(!stop_server)
   {
    int num_events = epoll_wait(epfd,events,MAXEVENTS,-1);
    if(num_events==-1)
    {
        if(errno==EINTR) // 错误原因是由于信号中断,可以继续等待
        {
            continue;
        }
        LOG_ERROR("%s", "EPOLL failed.\n");
        exit(-1);
   }
   for(int i=0; i<num_events; i++)
   {
    int sockfd = events[i].data.fd;
    /*
    简单的socket网络编程步骤-listenfd LT触发
    */
    if(events[i].data.fd==listenfd) // new 连接
    {
        printf("new 连接\n");
        struct sockaddr_in conaddr;
        socklen_t conlen=sizeof(conaddr);
        int confd=accept(listenfd, (struct sockaddr*)&conaddr, &conlen);

        if(confd==-1)
        {   
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            continue;
        }

        if(http_conn::m_user_count>=MAX_FD)
        {
            //给客户端响应一个消息，正忙
            LOG_ERROR("%s", "Internal server busy");
            close(confd);
            continue;
        }
        else
        {
            user[confd].init(confd,conaddr);
            // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
            util_timer* timer = new util_timer; // 创建了一个新的 util_timer 对象，用于表示定时器
            timer->call_back = call_back; // 回调函数,即定时器到期时执行的操作
            time_t cur = time( NULL ); // 获取当前时间
            timer->expire = cur + 3 * TIME_SLOT; // 设置定时器的过期时间，当前时间加上 3 倍的 TIME_SLOT（可能是一个时间间隔常量）

            // 将定时器和客户端数据绑定
            client_users[confd].timer=timer;  // 将定时器存储在 client_users 数组中，与新的客户端连接关联
            client_users[confd].sockfd=confd; // 将客户端套接字文件描述符存储在 client_users 数组中，与新的客户端连接关联
            client_users[confd].address=conaddr; // 将客户端地址信息存储在 client_users 数组中，与新的客户端连接关联

            timer->user_data = &client_users[confd]; // 将客户端数据的指针存储在定时器的 user_data 成员中，以便在定时器到期时可以访问到客户端数据
             
            timer_lst.add_timer( timer ); // 将创建的定时器添加到定时器链表 timer_lst 中。这个链表可能是用于管理定时任务的数据结构，其中的定时器会在一段时间后触发执行回调函数
        }
    }
    else if(( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) // LT触发模式
    {
            /*
            从管道中读取信号，并根据不同的信号类型执行相应的操作。
            这种机制通常用于处理定时任务的触发以及服务器的终止信号
            */
            int sig;
            char signals[1024];
            int ret = recv( pipefd[0], signals, sizeof( signals ), 0 ); // ret表示读取的字节数
            if( ret == -1 ) { // 发生错误
                continue;
            } else if( ret == 0 ) { // 管道已经关闭
                continue;
            } else  { 
                for( int i = 0; i < ret; ++i ) {
                    switch( signals[i] )  {
                        case SIGALRM:
                        {
                             // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                             // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                             timeout = true;
                             break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true; // SIGTERM 信号是用于优雅地终止程序，让程序有机会完成一些清理操作
                        }
                        }
                    }
                }
    }
    else if(events[i].events&(EPOLLHUP|EPOLLRDHUP|EPOLLERR)) //可修改成关闭定时器来关闭服务端
    {
        // 对方异常断开 或 错误 等事件
        user[sockfd].close_conn();
    }
    else if(events[i].events & EPOLLIN) // 客户端输入事件,从客户端收到数据
    {   
        util_timer* timer=client_users[sockfd].timer; // 获取与当前客户端连接关联的定时器
        time_t cur = time( NULL ); // 获取当前时间
        timer->expire = cur + 3 * TIME_SLOT; // 更新定时器的过期时间，以确保客户端保持活跃状态
        LOG_INFO("%s", "adjust timer once");
        Log::get_instance()->flush();
        timer_lst.adjust_timer(timer); // 调整定时器的位置，以便维持定时器链表的有序性
       
        printf("有数据到来\n");
        //一次性读取数据成功
        if(user[sockfd].read()) // 尝试从客户端套接字中一次性读取数据。如果读取成功，即收到数据
        {   
            Log::get_instance()->flush();
            pool->append(user+sockfd); // 将与客户端连接关联的 http_conn 实例添加到线程池中，以便后续处理
        }
        else
        {
            timer_lst.del_timer(timer); // 从定时器链表中删除对应的定时器，因为连接出现问题需要清理定时器。
            user[sockfd].close_conn();
        }  
    }
    else if(events[i].events & EPOLLOUT) // 向客户端发送数据的情况
    {   
        util_timer* timer=client_users[sockfd].timer;
        time_t cur = time( NULL ); // 获取当前时间
        LOG_INFO("%s", "adjust timer once");
        Log::get_instance()->flush();
        timer->expire = cur + 3 * TIME_SLOT;
        timer_lst.adjust_timer(timer);

        printf("向客户端写数据\n");
        //一次性写完所有数据
        bool ret=user[sockfd].write();

        if(!ret) // 写入失败,关闭连接
        {   
            timer_lst.del_timer(timer);
            user[sockfd].close_conn();
        } 
    }
   }
   // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
    if( timeout ) {
        timer_handler();
        timeout = false;  // 表示定时任务已经处理完毕
    }
   }

   close(epfd);
   close(listenfd);
   close(pipefd[1]);
   close(pipefd[0]);
   delete []user;
   delete pool;
   return 0;
}