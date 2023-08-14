# WebServer
Linux下C++轻量级Web服务器
1.使用 线程池 + 非阻塞socket + epoll + 事件处理(模拟Proactor均) 的并发模型
2.使用状态机解析HTTP请求报文，支持解析GET和POST请求
3.实现同步/异步日志系统，记录服务器运行状态
4.经Webbench压力测试可以实现上万的并发连接数据交换
