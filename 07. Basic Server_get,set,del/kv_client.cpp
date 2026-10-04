// learning/ch07/kv_client.cpp
//
// Same client as before, rewritten with `using namespace std;`.
//
// Build: g++ -std=c++17 -Wall -Wextra -o kv_client kv_client.cpp
// Run:   ./kv_client set foo bar     (with kv_server already running)
//        ./kv_client get foo
//        ./kv_client del foo

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace std;

const size_t k_max_msg = 4096;

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

// Encodes the command list as: [outer_len][nstr][len1][str1]...[lenN][strN]
static int32_t send_req(int fd, const vector<string>& cmd) {
    uint32_t len = 4;  // the nstr field itself
    for (const string& s : cmd) {
        len += 4 + (uint32_t)s.size();
    }
    if (len > k_max_msg) return -1;

    char wbuf[4 + k_max_msg];
    memcpy(&wbuf[0], &len, 4);

    uint32_t n = (uint32_t)cmd.size();
    memcpy(&wbuf[4], &n, 4);

    size_t cur = 8;
    for (const string& s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }
    return write_all(fd, wbuf, 4 + len);
}

static int32_t read_res(int fd) {
    char rbuf[4 + k_max_msg + 1];
    if (read_full(fd, rbuf, 4) < 0) {
        fprintf(stderr, "read() error on header\n");
        return -1;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > k_max_msg) {
        fprintf(stderr, "too long\n");
        return -1;
    }
    if (read_full(fd, &rbuf[4], len) < 0) {
        fprintf(stderr, "read() error on body\n");
        return -1;
    }

    if (len < 4) {
        fprintf(stderr, "bad response\n");
        return -1;
    }
    uint32_t rescode = 0;
    memcpy(&rescode, &rbuf[4], 4);
    printf("server says: [%u] %.*s\n", rescode, (int)(len - 4), &rbuf[8]);
    return 0;
}

int main(int argc, char** argv) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5559);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        die("connect()");
    }

    vector<string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }

    int32_t err = send_req(fd, cmd);
    if (!err) {
        err = read_res(fd);
    }

    close(fd);
    return err ? 1 : 0;
}
