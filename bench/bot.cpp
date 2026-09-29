// bot.cpp — 헤드리스 부하 클라이언트
// 봇 N명이 127.0.0.1:9000 에 접속, MOVE_MS 간격으로 각자 랜덤 ±1 이동 입력 전송.
// 수신 데이터는 계속 drain(버림) — 안 읽으면 서버 write가 EAGAIN/backpressure로 tick이 오염됨.
// 단일 스레드 + epoll 로 N소켓을 관리(2000 스레드 회피).
//
// 빌드: g++ -O2 -std=c++17 -o bot bot.cpp
// 실행: ./bot <N> [PORT=9000] [MOVE_MS=50]
//   SIGTERM/SIGINT 받으면 전부 정리하고 종료.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <random>
#include <vector>

static volatile sig_atomic_t g_stop=0;
static void on_sig(int){ g_stop=1; }
static void set_nonblocking(int fd){ int f=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,f|O_NONBLOCK); }
static inline long long now_ms(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long long)t.tv_sec*1000+t.tv_nsec/1000000; }

int main(int argc, char** argv){
    signal(SIGPIPE,SIG_IGN); signal(SIGINT,on_sig); signal(SIGTERM,on_sig);
    if(argc<2){ fprintf(stderr,"usage: %s <N> [PORT=9000] [MOVE_MS=50]\n",argv[0]); return 1; }
    int N=atoi(argv[1]);
    int port = (argc>=3)?atoi(argv[2]):9000;
    int move_ms = (argc>=4)?atoi(argv[3]):50;

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);

    int epfd=epoll_create1(0);
    std::vector<int> fds; fds.reserve(N);

    // 접속 (블로킹 connect — localhost라 즉시), 그 뒤 논블로킹 전환
    int fail=0;
    for(int i=0;i<N;i++){
        int fd=socket(AF_INET,SOCK_STREAM,0);
        if(fd<0){ fail++; continue; }
        if(connect(fd,(sockaddr*)&addr,sizeof(addr))<0){ close(fd); fail++; continue; }
        set_nonblocking(fd);
        int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        struct epoll_event ev{}; ev.events=EPOLLIN; ev.data.fd=fd;
        epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&ev);
        fds.push_back(fd);
    }
    printf("[bot] connected=%zu fail=%d port=%d move_ms=%d\n",fds.size(),fail,port,move_ms);
    fflush(stdout);

    std::mt19937 rng((unsigned)now_ms());
    std::uniform_int_distribution<int> step(-1,1);
    char drain[8192];
    struct epoll_event evs[512];
    long long next_move=now_ms();

    while(!g_stop){
        int timeout=(int)(next_move-now_ms()); if(timeout<0) timeout=0;
        int n=epoll_wait(epfd,evs,512,timeout);
        for(int i=0;i<n;i++){
            int fd=evs[i].data.fd;
            // 수신 drain
            while(true){ ssize_t c=read(fd,drain,sizeof(drain)); if(c<=0) break; }
        }
        long long t=now_ms();
        if(t>=next_move){
            // 모든 봇 이동 입력 1회
            for(int fd: fds){
                int8_t dx=(int8_t)step(rng), dy=(int8_t)step(rng);
                if(dx==0&&dy==0) dx=1; // 정지만 계속되지 않게 약간의 churn
                unsigned char pkt[6]={0,2,0,2,(unsigned char)dx,(unsigned char)dy}; // len=2,type=2(MOVE)
                ssize_t w=write(fd,pkt,6); (void)w; // EAGAIN이면 이번 tick 스킵
            }
            next_move += move_ms;
            if(next_move<t) next_move=t+move_ms; // 밀렸으면 리셋
        }
    }
    for(int fd: fds) close(fd);
    printf("[bot] done\n");
    return 0;
}
