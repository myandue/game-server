// game_server_bench.cpp
// 브로드캐스트 범위 최적화 A/B/C 비교용 계측 서버.
// 원본 game_server.cpp 기반. 바뀐 점:
//   1) 브로드캐스트 방식을 환경변수 BC_MODE로 A/B/C 스위치
//        A = full snapshot  : 모든 N명을 전원에게 (scope = 전체)
//        B = AOI linear     : 수신자마다 전체를 훑어 반경 내만 (연산 O(N^2), 전송 감소)
//        C = grid bucket    : 자기 칸 + 이웃 3x3 버킷만 조회 (연산 ~O(N))
//   2) 델타 OFF: 매 tick 범위 내 전량을 changed로 전송(순수 scope 비교). 와이어 포맷은 PKT_DELTA 유지.
//   3) 충돌 검사를 점유맵으로 O(1) 화 (원본은 이동마다 전체 스캔 O(N) → tick O(N^2)).
//      broadcast만 병목이 되게 하여 "몇 명까지 50ms"를 broadcast가 결정하도록.
//   4) accept 시 서버가 분포에 맞춰 spawn 좌표 배정 (WMAX가 곧 분포 통제: uniform=큰 월드, clustered=작은 월드).
//   5) tick마다 계측 CSV 1줄: t_ms,tick,n,build_us,send_us,intended_bytes,written_bytes
//        build_us      = 이웃탐색 + 페이로드 구성 시간 (순수 연산; N^2 vs N 스토리)
//        send_us       = write() 시스템콜 시간 (localhost I/O)
//        intended_bytes= 만들어서 보내려 한 총 바이트 (전송량 스케일링의 순수 신호)
//        written_bytes = 실제 write가 받아준 바이트 (참고)
//
// 빌드:  g++ -O2 -std=c++17 -o game_server_bench game_server_bench.cpp
// 실행:  BC_MODE=C WMAX=140 VIEW=20 CSV=results/uniform_1000_C.csv ./game_server_bench

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <cstdint>
#include <fcntl.h>
#include <cerrno>
#include <csignal>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <string>
#include <vector>

enum PacketType : uint16_t {
    PKT_MOVE = 2,
    PKT_DELTA = 4,
};

// --- 환경설정 (실행 시 통제/기록되는 변수들) ---
static char   G_MODE   = 'C';        // BC_MODE: A/B/C
static int    WORLD_MIN = 0;
static int    WORLD_MAX = 100;       // WMAX
static int    VIEW      = 20;        // AOI 반경 == 버킷 칸 크기
static const int MAX_STEP = 1;       // tick당 최대 이동 ±1
static const long TICK_NS = 50L * 1000000L; // 50ms

// 빅엔디안 인코딩
static inline void put_u16(std::string& b, uint16_t v){ b.push_back((v>>8)&0xFF); b.push_back(v&0xFF); }
static inline void put_u32(std::string& b, uint32_t v){ b.push_back((v>>24)&0xFF); b.push_back((v>>16)&0xFF); b.push_back((v>>8)&0xFF); b.push_back(v&0xFF); }
static inline uint16_t get_u16(const std::string& b, int off){ return (uint16_t)(((unsigned char)b[off]<<8)|(unsigned char)b[off+1]); }

struct Client { std::string recv_buf; int x=0; int y=0; };
static std::unordered_map<int, Client> clients;

// O(1) 충돌: 점유된 셀 집합 (좌표 <= ~수백 이므로 x*100000+y 로 유일 키)
static inline long long cellkey(int x,int y){ return (long long)x*100000LL + y; }
static std::unordered_set<long long> occupied;

// 격자 버킷 (모드 C에서만 사용)
static std::map<std::pair<int,int>, std::vector<int>> buckets;

static int epfd = -1;
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int){ g_stop = 1; }

static void set_nonblocking(int fd){ int f=fcntl(fd,F_GETFL,0); fcntl(fd,F_SETFL,f|O_NONBLOCK); }

static inline long long now_ns(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long long)t.tv_sec*1000000000LL + t.tv_nsec; }

int main(){
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if(const char* e=getenv("BC_MODE")) G_MODE = e[0];
    if(const char* e=getenv("WMAX"))    WORLD_MAX = atoi(e);
    if(const char* e=getenv("VIEW"))    VIEW = atoi(e);
    const char* csv_path = getenv("CSV");
    unsigned seed = 12345;
    if(const char* e=getenv("SEED")) seed = (unsigned)atoi(e);

    FILE* csv = nullptr;
    if(csv_path && csv_path[0]){
        csv = fopen(csv_path, "w");
        if(csv) fprintf(csv, "t_ms,tick,n,build_us,send_us,intended_bytes,written_bytes\n");
    }
    std::mt19937 rng(seed);

    // listen 소켓
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd<0){ perror("socket"); return 1; }
    int opt=1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // timerfd 50ms
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec ts{};
    ts.it_value.tv_nsec = TICK_NS; ts.it_interval.tv_nsec = TICK_NS;
    timerfd_settime(timer_fd, 0, &ts, nullptr);

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(9000);
    if(bind(listen_fd,(sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 1; }
    if(listen(listen_fd, 1024)<0){ perror("listen"); return 1; }
    set_nonblocking(listen_fd); // accept drain 루프가 EAGAIN으로 빠져나오게

    epfd = epoll_create1(0);
    struct epoll_event ev{};
    ev.events=EPOLLIN; ev.data.fd=listen_fd; epoll_ctl(epfd,EPOLL_CTL_ADD,listen_fd,&ev);
    ev.events=EPOLLIN; ev.data.fd=timer_fd;  epoll_ctl(epfd,EPOLL_CTL_ADD,timer_fd,&ev);

    printf("서버 대기 중... 포트 9000 | MODE=%c WMAX=%d VIEW=%d CSV=%s\n",
           G_MODE, WORLD_MAX, VIEW, csv_path?csv_path:"(none)");
    fflush(stdout);

    struct epoll_event events[256];
    long long start_ns = now_ns();
    uint32_t tick = 0;

    while(!g_stop){
        int n = epoll_wait(epfd, events, 256, 500);
        if(n<0){ if(errno==EINTR) continue; perror("epoll_wait"); break; }

        for(int i=0;i<n;i++){
            int fd = events[i].data.fd;

            // ---------------- TICK ----------------
            if(fd==timer_fd){
                uint64_t exp; ssize_t r=read(timer_fd,&exp,sizeof(exp)); (void)r;

                long long build_ns=0, send_ns=0;
                long long intended=0, written=0;
                int ncur = (int)clients.size();

                // 모드 C: 버킷 재구성
                if(G_MODE=='C'){
                    buckets.clear();
                    for(auto& [cfd,c]: clients){
                        buckets[{c.x/VIEW, c.y/VIEW}].push_back(cfd);
                    }
                }

                for(auto& [rfd, recip] : clients){
                    long long b0 = now_ns();

                    // scope 결정 → cur (id -> (x,y))
                    std::vector<std::pair<int,std::pair<int,int>>> cur;
                    if(G_MODE=='A'){
                        cur.reserve(clients.size());
                        for(auto& [cfd,c]: clients) cur.push_back({cfd,{c.x,c.y}});
                    } else if(G_MODE=='B'){
                        for(auto& [cfd,c]: clients){
                            if(std::abs(c.x-recip.x)<=VIEW && std::abs(c.y-recip.y)<=VIEW)
                                cur.push_back({cfd,{c.x,c.y}});
                        }
                    } else { // C
                        int rbx=recip.x/VIEW, rby=recip.y/VIEW;
                        for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++){
                            auto it=buckets.find({rbx+dx,rby+dy});
                            if(it==buckets.end()) continue;
                            for(int cfd: it->second){
                                Client& c=clients[cfd];
                                if(std::abs(c.x-recip.x)<=VIEW && std::abs(c.y-recip.y)<=VIEW)
                                    cur.push_back({cfd,{c.x,c.y}});
                            }
                        }
                    }

                    // 델타 OFF: removed=0, changed=cur 전량
                    std::string payload;
                    put_u16(payload, 0);                 // removed count
                    put_u16(payload, (uint16_t)cur.size()); // changed count
                    for(auto& e: cur){
                        put_u32(payload, (uint32_t)e.first);
                        put_u32(payload, (uint32_t)e.second.first);
                        put_u32(payload, (uint32_t)e.second.second);
                    }
                    std::string pkt; put_u16(pkt,(uint16_t)payload.size()); put_u16(pkt,PKT_DELTA); pkt+=payload;

                    long long b1=now_ns(); build_ns += (b1-b0);
                    intended += (long long)pkt.size();

                    ssize_t w = write(rfd, pkt.data(), pkt.size());
                    long long b2=now_ns(); send_ns += (b2-b1);
                    if(w>0) written += w;
                }

                if(csv){
                    long long t_ms=(now_ns()-start_ns)/1000000LL;
                    fprintf(csv, "%lld,%u,%d,%lld,%lld,%lld,%lld\n",
                            t_ms, tick, ncur, build_ns/1000, send_ns/1000, intended, written);
                    fflush(csv);
                }
                tick++;
                continue;
            }

            // ---------------- ACCEPT ----------------
            if(fd==listen_fd){
                while(true){
                    int cfd = accept(listen_fd, nullptr, nullptr);
                    if(cfd<0) break; // EAGAIN 포함: 다 받음
                    set_nonblocking(cfd);
                    int one=1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    struct epoll_event cev{}; cev.events=EPOLLIN|EPOLLET; cev.data.fd=cfd;
                    epoll_ctl(epfd,EPOLL_CTL_ADD,cfd,&cev);

                    // 분포대로 빈 셀 배정 (WMAX가 분포를 통제)
                    int sx=0, sy=0; bool placed=false;
                    std::uniform_int_distribution<int> dist(WORLD_MIN, WORLD_MAX);
                    for(int a=0;a<100000;a++){
                        int rx=dist(rng), ry=dist(rng);
                        if(occupied.insert(cellkey(rx,ry)).second){ sx=rx; sy=ry; placed=true; break; }
                    }
                    Client c; c.x=sx; c.y=sy; (void)placed;
                    clients[cfd]=c;
                }
                continue;
            }

            // ---------------- CLIENT DATA ----------------
            if(events[i].events & EPOLLIN){
                bool closed=false;
                while(true){
                    char buf[2048];
                    ssize_t cnt=read(fd,buf,sizeof(buf));
                    if(cnt>0){ auto it=clients.find(fd); if(it!=clients.end()) it->second.recv_buf.append(buf,cnt); }
                    else if(cnt==0){ closed=true; break; }
                    else { if(errno==EAGAIN||errno==EWOULDBLOCK) break; closed=true; break; }
                }
                auto it=clients.find(fd);
                if(it!=clients.end()){
                    Client& c=it->second;
                    while(c.recv_buf.size()>=4){
                        uint16_t length=get_u16(c.recv_buf,0);
                        if(c.recv_buf.size() < (size_t)(4+length)) break;
                        uint16_t type=get_u16(c.recv_buf,2);
                        std::string payload=c.recv_buf.substr(4,length);
                        c.recv_buf.erase(0,4+length);
                        if(type==PKT_MOVE && payload.size()==2){
                            int8_t dx=(int8_t)payload[0], dy=(int8_t)payload[1];
                            if(dx<-MAX_STEP||dx>MAX_STEP||dy<-MAX_STEP||dy>MAX_STEP) continue;
                            int nx=c.x+dx, ny=c.y+dy;
                            if(nx>WORLD_MAX)nx=WORLD_MAX; if(nx<WORLD_MIN)nx=WORLD_MIN;
                            if(ny>WORLD_MAX)ny=WORLD_MAX; if(ny<WORLD_MIN)ny=WORLD_MIN;
                            if(nx==c.x && ny==c.y) continue;
                            // O(1) 충돌: 목표 셀 점유 시 거부
                            long long nk=cellkey(nx,ny);
                            if(occupied.count(nk)) continue;
                            occupied.erase(cellkey(c.x,c.y));
                            occupied.insert(nk);
                            c.x=nx; c.y=ny;
                        }
                    }
                }
                if(closed){
                    auto it2=clients.find(fd);
                    if(it2!=clients.end()){ occupied.erase(cellkey(it2->second.x,it2->second.y)); clients.erase(it2); }
                    epoll_ctl(epfd,EPOLL_CTL_DEL,fd,nullptr);
                    close(fd);
                }
            }
        }
    }
    if(csv) fclose(csv);
    printf("종료 (tick=%u)\n", tick);
    return 0;
}
