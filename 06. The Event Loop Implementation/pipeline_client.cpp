// learning/ch06/pipeline_client.cpp
//
// Demonstrates pipelining: fire off several requests back to back
// WITHOUT waiting for each reply first, then read all the replies
// afterward. This is exactly the scenario that forces
// state_machine_server.cpp's try_one_request() to loop internally —
// several complete messages can be sitting in the read buffer at once.
//
// Build: g++ -std=c++17 -Wall -Wextra -o pipeline_client pipeline_client.cpp
// Run:   ./pipeline_client   (with state_machine_server already running)

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static const size_t k_max_msg = 4096;

static void die(const char* what) {
    perror(what);
    exit(1);
}

static int read_full(int fd, char* buf, size_t n) {
    while (n > 0) {
        ssize_t got = read(fd, buf, n);
        if (got <= 0) return -1;
        buf += got;
        n -= (size_t)got;
    }
    return 0;
}

static int write_all(int fd, const char* buf, size_t n) {
    while (n > 0) {
        ssize_t sent = write(fd, buf, n);
        if (sent <= 0) return -1;
        buf += sent;
        n -= (size_t)sent;
    }
    return 0;
}

static int send_req(int fd, const char* text) {
    uint32_t len = (uint32_t)strlen(text);
    char buf[4 + k_max_msg];
    memcpy(buf, &len, 4);
    memcpy(buf + 4, text, len);
    return write_all(fd, buf, 4 + len);
}

static int read_res(int fd) {
    char header[4];
    if (read_full(fd, header, 4) < 0) return -1;

    uint32_t len = 0;
    memcpy(&len, header, 4);
    if (len > k_max_msg) return -1;

    char body[k_max_msg + 1];
    if (read_full(fd, body, len) < 0) return -1;
    body[len] = '\0';
    printf("[client] server replied: %s\n", body);
    return 0;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5558);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        die("connect()");
    }

    const char* query_list[3] = {"hello1", "hello2", "hello3"};

    // Send all three WITHOUT reading any reply in between — this is
    // the pipelining part. By the time the server gets around to
    // reading the socket, all three requests (plus their length
    // prefixes) may well already be sitting in its receive buffer as
    // one continuous blob.
    printf("[client] sending all 3 requests without waiting...\n");
    for (const char* q : query_list) {
        if (send_req(fd, q) < 0) {
            fprintf(stderr, "[client] send failed\n");
            close(fd);
            return 1;
        }
    }

    // Only now do we read the replies.
    printf("[client] now reading all 3 replies...\n");
    for (int i = 0; i < 3; i++) {
        if (read_res(fd) < 0) {
            fprintf(stderr, "[client] read failed\n");
            break;
        }
    }

    close(fd);
    return 0;
}
