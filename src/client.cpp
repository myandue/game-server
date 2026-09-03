#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unordered_map>

#include <termios.h>

// 서버와 공유하는 계약
enum PacketType : uint16_t {
    PKT_TICK = 1,
    PKT_MOVE = 2,
    PKT_SNAPSHOT = 3,
    PKT_DELTA = 4,
};

void put_u16(std::string& buf, uint16_t v) {
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back(v & 0xFF);
}

// 빅엔디안 2바이트 -> 숫자
uint16_t get_u16(std::string& buf, int offset) {
    return (((unsigned char)buf[offset] << 8) | (unsigned char)buf[offset+1]);
}

uint32_t get_u32(std::string& buf, int offset) {
    uint32_t payload = ((unsigned char)buf[offset] << 24);
    payload |= ((unsigned char)buf[offset+1] << 16);
    payload |= ((unsigned char)buf[offset+2] << 8);
    payload |= (unsigned char)buf[offset+3];
    return payload;
}

// [length(2)][type(2)][payload]
std::string make_packet(uint16_t type, const std::string& payload) {
    std::string pkt;
    put_u16(pkt, payload.size());
    put_u16(pkt, type);
    pkt += payload;
    return pkt;
}

struct Client {
    int x;
    int y;
};
std::unordered_map<int, Client> clients;

int epfd = epoll_create1(0);

int main() {
    signal(SIGPIPE, SIG_IGN);

    // 1) 소켓 만들고 서버에 connect (서버는 bind/listen/accept)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    // sockaddr_in vs. sockaddr
    // sockaddr_in: IPv4 전용. 필드 읽기가 좋아서 구조체 채울 때 사용.
    // sockaddr: 옛날 범용 구조체. bind/connect/accept 함수가 인자타입으로 요구하는 구조체.
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // 서버 주소 
    // inet_pton 함수를 통해 4바이트 이진 주소로 변환되어 addr.sin_addr.s_addr가 채워짐.

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }
    printf("서버 접속 완료\n");

    // 터미널 설정 변경
    // 기본 - canotical 모드: 입력을 한 줄씩 모아서 엔터를 쳐야 프로그램에 넘겨줌 + 키를 화면에 echo
    // 변경: 키 하나 누르면 엔터 없이 즉시 넘김 + echo 끄기
    // 1) 원래 설정 저장
    struct termios orig; 
    tcgetattr(0, &orig); // fd 0 = stdin: 지금 터미널(fd 0)의 현재 설정을 읽어서 orig에 담기
    // 2) 복사본 만들어서 플래그 끄기
    struct termios raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO); // 두 비트만 끔
    // 3) 즉시 적용
    tcsetattr(0, TCSANOW, &raw); // raw 세팅값을 지금 터미널(fd 0)에 즉시 적용(TCSANOW)
    
    // fd 장부 등록
    // a) 소켓
    struct epoll_event s_ev;
    s_ev.events = EPOLLIN;
    s_ev.data.fd = sock;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &s_ev);

    // b) 키보드 입력 / 0 = stdin(표준 입력, 키보드)
    struct epoll_event keyboard_ev;
    keyboard_ev.events = EPOLLIN;
    keyboard_ev.data.fd = 0;
    epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &keyboard_ev);

    struct epoll_event events[2];

    std::string recv_buf;
    char buf[4096];

    // 받기 루프 
    while (true) {
        int n = epoll_wait(epfd, events, 2, -1);
        if (n < 0) { perror("epoll_wait"); break; }

        bool closed = false;

        for (int i = 0 ; i < n ; i++) {
            int fd = events[i].data.fd;

            if (fd == sock) {
                ssize_t cnt = read(sock, buf, sizeof(buf));

                if (cnt <= 0) { closed = true; break; } // 0: 서버 끊김, <0: 에러
                recv_buf.append(buf, cnt);

                // 프레임 추출
                while (true) {
                    if (recv_buf.size() < 4) break;
                    uint16_t length = get_u16(recv_buf, 0);
                    if (recv_buf.size() < (4 + length)) break;
                    uint16_t type = get_u16(recv_buf, 2);
                    std::string payload = recv_buf.substr(4, length);
                    recv_buf.erase(0, 4 + length);

                    // 처리 - 모든 유저의 위치값 받기
                    // if (type == PKT_SNAPSHOT) {
                    //     clients.clear();
                    //     int client_cnt = get_u16(payload, 0);

                    //     for (int i = 0 ; i < client_cnt ; i++) {
                    //         int j = 3*i;
                    //         int id = get_u32(payload, 4*j + 2);
                    //         int x = get_u32(payload, 4*(j+1) + 2);
                    //         int y = get_u32(payload, 4*(j+2) + 2);

                    //         clients[id] = Client{.x = x, .y = y};
                    //     }
                    // }

                    // 처리 - 값이 변한 유저의 위치값 받기 (or 제거)
                    if (type == PKT_DELTA) {
                        int off = 0; // 오프셋

                        int removed_cnt = get_u16(payload, off);
                        off += 2;
                        for (int i = 0 ; i < removed_cnt ; i++) {
                            int id = get_u32(payload, off);
                            off += 4;

                            clients.erase(id);
                        }

                        int changed_cnt = get_u16(payload, off);
                        off += 2;
                        for (int i = 0 ; i < changed_cnt ; i++) {
                            int id = get_u32(payload, off);
                            off += 4;
                            int x = get_u32(payload, off);
                            off += 4;
                            int y = get_u32(payload, off);
                            off += 4;

                            clients[id] = Client{.x = x, .y = y};
                        }
                    }

                    // 출력 - 모든 유저의 위치값 그리기
                    // 화면 지우고 커서 맨 위로
                    printf("\033[2J\033[H"); // '\033': ESC, '[2J': 화면 클리어, '[H': 커서를 좌상단으로

                    // 실제 위치 출력
                    for (auto& [id, client] : clients) {
                        printf("id %d: (%d, %d)\n", id, client.x, client.y);
                    }

                    // 현재 버퍼 비우기
                    fflush(stdout);
                }
                
                continue;
            }

            if (fd == 0) { // 키보드 입력
                char key;
                ssize_t cnt = read(0, &key, 1);

                if (cnt <= 0) { closed = true; break; } // 0: 입력 닫힘, <0: 에러
                
                int dx = 0;
                int dy = 0;
                switch (key) {
                    case 'w': dy = -1; break;
                    case 's': dy = +1; break;
                    case 'a': dx = -1; break;
                    case 'd': dx = +1; break;
                    case 'q': closed = true; break; // 종료 키
                }

                std::string payload;
                payload.push_back((int8_t)dx); // 1바이트인 int8_t로 캐스팅해서 넣기
                payload.push_back((int8_t)dy);

                std::string pkt = make_packet(PKT_MOVE, payload);

                ssize_t w = write(sock, pkt.data(), pkt.size());
                (void) w;

                continue;
            }
        }    

        if (closed) break;
    }
    
    close(sock);

    // 터미널 원복
    tcsetattr(0, TCSANOW, &orig);

    return 0;
}