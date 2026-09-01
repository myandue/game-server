#include <cstdio>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h> // epoll_create1, epoll_ctl, epoll_wait
#include <sys/eventfd.h> // eventfd
#include <sys/timerfd.h> //timerfd
#include <cstdint> // uint64_t

#include <fcntl.h> // fcntl, O_NONBLOCK
#include <cerrno> // errno, EAGAIN

#include <csignal>
#include <unordered_map>
#include <map>
#include <string>
#include <queue>
#include <vector>
#include <mutex>

enum PacketType : uint16_t {
    PKT_TICK = 1, // 서버 -> 클라: 지금 몇 tick 째인지
    PKT_MOVE = 2, // 클라 -> 서버: client의 이동 수신
    PKT_SNAPSHOT = 3, // 서버 -> 클라: 모든 플레이어의 좌표
};

// 상수 설정
const int WORLD_MIN = 0;
const int WORLD_MAX = 100;
const int MAX_STEP = 1; // 한 tick 최대 이동량 (±1)
const int VIEW = 20; // 유저의 반경 

// uint16을 빅엔디안 2바이트로 buf 끝에 붙이기
void put_u16(std::string& buf, uint16_t v) {
    buf.push_back((v >> 8) & 0xFF); // 상위 바이트 먼저
    buf.push_back(v & 0xFF); // 하위 바이트
}

// uint32를 빅엔디안 4바이트로
void put_u32(std::string& buf, uint32_t v) {
    buf.push_back((v >> 24) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back(v & 0xFF);
}

// [lenth(2)][type(2)][payload] 완성된 패킷 바이트 만들기
std::string make_packet(uint16_t type, const std::string& payload) {
    std::string pkt;
    put_u16(pkt, payload.size()); // length = payload 바이트 수 (헤더 제외)
    put_u16(pkt, type);
    pkt += payload; // 페이로드 이어붙이기 
    return pkt;
}


// offset 위치 바이트와 offset+1 위치 바이트를 합쳐서 하나의 숫자로 만드는 작업
// char 는 부호가 있는 타입이라 값이 오염될 수 있기 때문에 'unsigned char'로 캐스팅 후 연산 진행 
uint16_t get_u16(std::string& buf, int offset) {
    return (((unsigned char)buf[offset] << 8) | (unsigned char)buf[offset+1]);
}

struct Client {
    std::string send_buf;
    std::string recv_buf;
    
    //위치
    int x;
    int y;
};
std::unordered_map<int, Client> clients;

// 버킷
std::map<std::pair<int, int>, std::vector<int>> buckets; // <x, y> - 버킷 좌표 / vector<int> - 해당 버킷의 fd 리스트

int epfd = epoll_create1(0); // 장부 개설

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0); // 현재 플래그 읽고
    fcntl(fd, F_SETFL, flags | O_NONBLOCK); // O_NONBLOCK 켜기, 현재 플래그 + 논블록 플래그 
} 
// fcntl: "이 fd의 속성을 바꿔줘" / 여기서는 소켓을 "읽을 게 없으면 잠들지 말고 즉시 EAGAIN 반환"모드로 바꾸는 것 

int main() {
    signal(SIGPIPE, SIG_IGN); // SIGPIPE 무시 (프로세스 안 죽음)

    // 1) 듣기 소켓 준비
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {perror("socket"); return 1;}

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // timer_fd 만들어서 준비
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    // CLOCK_MONOTONIC: 시스템 시간 바뀌어도(NTP 보정 등) 영향 안 받는 단조 증가 시계

    struct itimerspec ts{};
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 50 * 1000000; // 첫 발사까지 50ms (tv_sec + tv_nsec)
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 50 * 1000000; // 그 뒤 반복 간격 50mc (tv_sec + tv_nsec)
    timerfd_settime(timer_fd, 0, &ts, nullptr);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(9000);

    if(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {perror("bind"); return 1;}
    if(listen(listen_fd, 16) < 0) {perror("listen"); return 1;}
    printf("서버 대기 중... 포트 9000\n");

    // 2) epoll 장부 만들고 listen_fd 등록
    // <-> select의 fd_set master; FD_SET(listen_fd, &master); 자리
    struct epoll_event l_ev; 
    l_ev.events = EPOLLIN; // "읽기 가능해지면 알려줘" 
    l_ev.data.fd = listen_fd; // 누구인지 메모
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &l_ev); // 등록 (딱 한 번)

    // 만들어둔 timer_fd를 장부에 등록
    struct epoll_event t_ev;
    t_ev.events = EPOLLIN;
    t_ev.data.fd = timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, timer_fd, &t_ev);

    struct epoll_event events[64]; // 준비된 것 받아올 배열 

    // 3) 이벤트 루프
    while (true) {
        // <-> select 자리. 준비된 것만 events[]에 담고 개수 n 리턴
        int n = epoll_wait(epfd, events, 64, -1); // -1 = 무한 대기
        if(n < 0) { perror("epoll_wait"); break;}

        // <-> 준비된 n개만 순회
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd; // 이 이벤트 주인 fd

            if (fd == timer_fd) {
                uint64_t expirations;
                ssize_t n = read(timer_fd, &expirations, sizeof(expirations));
                (void) n; // n으로 안받아도 되는데, read는 반환값을 받지 않고, 처리하지 않으면 경고를 발생시킴.
                // read의 두번째 인자는 주소값이어야하는데, expirations의 경우 값 하나짜리(uint64_t)라서 주소형태('&')로 받는다.

                // 초기화는 이 줄에 처음 도달했을 때 딱 한 번
                static uint32_t tick = 0;

                // 각 client의 버킷 위치 체크
                buckets.clear();
                for (auto& [cfd, client]: clients) {
                    int bx = client.x / VIEW;
                    int by = client.y / VIEW;
                    buckets[{bx, by}].push_back(cfd);
                }

                for (auto& [rfd, recipient] : clients) { // 받는 사람마다 전용 payload 새로 만들기
                    // payload 처음에 cnt가 들어가야해서, payload_tmp를 먼저 만들기
                    int client_cnt = 0;
                    std::string payload_tmp;

                    // 수신자의 버킷 위치 
                    int rbx = recipient.x / VIEW;
                    int rby = recipient.y / VIEW;

                    // 주변 버킷만 순회 (9개)
                    for (int ddx = -1 ; ddx <= 1; ddx++) { // 왼쪽으로 한칸 ~ 오른쪽으로 한칸
                        for (int ddy = -1 ; ddy <= 1; ddy++) { // 위로 한칸 ~ 밑으로 한칸
                            auto it = buckets.find({rbx+ddx, rby+ddy}); // 이터레이터(map 안의 원소 하나를 가리키는 포인터 같은 것)
                            if (it == buckets.end()) continue; // 버킷 없음 의미. 그 버킷 비었으면 건너뜀.

                            for (int cfd: it -> second) { // 해당 버킷에 해당하는 fd 리스트 순회
                                Client& client = clients[cfd];
                                if (abs(client.x - recipient.x) <= VIEW && abs(client.y - recipient.y) <= VIEW) {
                                    put_u32(payload_tmp, cfd);
                                    put_u32(payload_tmp, client.x);
                                    put_u32(payload_tmp, client.y);
                                    client_cnt +=1;
                                }
                            }
                        }
                    }

                    std::string payload;
                    put_u16(payload, client_cnt);
                    payload += payload_tmp;

                    std::string pkt = make_packet(PKT_SNAPSHOT, payload);

                    ssize_t w = write(rfd, pkt.data(), pkt.size());
                    (void) w;
                }

                tick++;
                continue;
            }

            if (events[i].events & EPOLLIN) {
                if (fd == listen_fd) {
                    int client_fd = accept(listen_fd, NULL, NULL);
                    if (client_fd < 0) { perror("accept"); continue;}

                    set_nonblocking(client_fd); // 논블로킹

                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLET; // ET 켜기 (EPOLLET: ET 키는 옵션 / 도착 순간 한 번만 알림 / 안 키면 기본값(LT: 남아있으면 계속 알림))
                    cev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev); // <-> FD_SET

                    clients[client_fd] = Client{};

                    continue;
                } else {
                    // fd == client_fd
                    bool closed = false;
                    while (true) { // 수신버퍼를 다 비울 때까지 recv_buf에 받기
                        char buf[1024];
                        ssize_t cnt = read(fd, buf, sizeof(buf));

                        if (cnt > 0) {
                            if (clients.count(fd)) {
                                Client& c= clients[fd];
                                c.recv_buf.append(buf, cnt);
                            }
                        } else if (cnt == 0) { // 끊김
                            closed = true;
                            break;
                        } else { // 'cnt < 0': 에러
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 에러X. 다 읽었음 표시. -> 루프 종료

                            // 그 외 -> 실제 에러. client와 연결 회수 필요.
                            closed = true;
                            break;
                        }
                    }

                    // recv_buf에 받은 것들을 처리
                    if (clients.count(fd)) {
                        Client& c = clients[fd];

                        while (true) { // recv_buf에서 한 패킷씩 잘라서 처리
                            if (c.recv_buf.size() < 4) break; // 헤더가 온전히 들어오지 않아 루프 종료

                            // 패킷의 length: 패킷의 첫번째 두번째 바이트
                            uint16_t length = get_u16(c.recv_buf, 0);
                            
                            // 패킷이 온전히 도착했는지 체크. 안왔으면 루프 종료
                            if (c.recv_buf.size() < 4 + length) break;

                            // type: 패킷의 두번째 세번째 바이트
                            uint16_t type = get_u16(c.recv_buf, 2);

                            // payload: 네번째 바이트부터 length 길이 만큼
                            std::string payload = c.recv_buf.substr(4, length);

                            // recv_buf에서 제거
                            c.recv_buf.erase(0, 4 + length);

                            // 처리
                            if (type == PKT_MOVE) {
                                if (payload.size() == 2) {
                                    int8_t dx = (int8_t)payload[0];
                                    int8_t dy = (int8_t)payload[1];

                                    // 이상값 거부
                                    if (dx < -MAX_STEP || dx > MAX_STEP || dy < -MAX_STEP || dy > MAX_STEP) continue;

                                    int nx = c.x + dx;
                                    int ny = c.y + dy;

                                    // 세계보다 크거나 작을 경우 세계 사이즈 적용
                                    if (nx > WORLD_MAX) nx = WORLD_MAX;
                                    if (nx < WORLD_MIN) nx = WORLD_MIN;
                                    if (ny > WORLD_MAX) ny = WORLD_MAX;
                                    if (ny < WORLD_MIN) ny = WORLD_MIN;

                                    // 다른 유저와 충돌 체크 (이미 그 위치에 유저 존재하는지)
                                    for (auto& [cfd, client] : clients) {
                                        if (client.x == nx && client.y == ny) {
                                            nx = c.x;
                                            ny = c.y;
                                            break;
                                        }
                                    }

                                    c.x = nx;
                                    c.y = ny;
                                }
                            }
                        }
                    }
                    
                    if (closed) {
                        if (clients.count(fd)) {
                            clients.erase(fd);
                        }
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                    }
                }
            }
        }
    }
    return 0;
}