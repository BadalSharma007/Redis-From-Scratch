
// learning/ch07/kv_server.cpp
//
// Same as before, rewritten with `using namespace std;` so you don't
// need the std:: prefix everywhere. Fine for a single-file program
// like this one -- for multi-file projects (like our real src/ code),
// it's best kept out of header files to avoid name collisions leaking
// into every file that includes them.
//
// Build: g++ -std=c++17 -Wall -Wextra -o kv_server kv_server.cpp
// Run:   ./kv_server

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace std;

const size_t k_max_msg = 4096;
const size_t k_max_args = 1024;

static void msg(const char* text) {
    fprintf(stderr, "%s\n", text);
}

static void die(const char* text) {
    perror(text);
    exit(1);
}

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

enum {
    STATE_REQ = 0,
    STATE_RES = 1,
    STATE_END = 2,
};

struct Conn {
    int fd = -1;
    uint32_t state = STATE_REQ;
    size_t rbuf_size = 0;
    uint8_t rbuf[4 + k_max_msg];
    size_t wbuf_size = 0;
    size_t wbuf_sent = 0;
    uint8_t wbuf[4 + k_max_msg];
};

// ---------------------------------------------------------------
// The "database." A plain map is a stand-in on purpose -- the next
// chapter replaces this with a hand-built hash table.
// ---------------------------------------------------------------
static map<string, string> g_map;

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,  // key not found
};

static bool cmd_is(const string& word, const char* cmd) {
    return word == cmd;
}

static uint32_t do_get(const vector<string>& cmd, uint8_t* res, uint32_t* reslen) {
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }
    const string& val = g_map[cmd[1]];
    assert(val.size() <= k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = (uint32_t)val.size();
    return RES_OK;
}

static uint32_t do_set(const vector<string>& cmd, uint8_t*, uint32_t*) {
    g_map[cmd[1]] = cmd[2];
    return RES_OK;
}

static uint32_t do_del(const vector<string>& cmd, uint8_t*, uint32_t*) {
    g_map.erase(cmd[1]);
    return RES_OK;
}

// Unpacks [nstr][len+str]... from the request body into a string list.
static int32_t parse_req(const uint8_t* data, size_t len, vector<string>& out) {
    if (len < 4) return -1;

    uint32_t n = 0;
    memcpy(&n, &data[0], 4);
    if (n > k_max_args) return -1;

    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) return -1;
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) return -1;
        out.push_back(string(reinterpret_cast<const char*>(&data[pos + 4]), sz));
        pos += 4 + sz;
    }
    if (pos != len) return -1;  // trailing garbage after the last string
    return 0;
}

static int32_t do_request(const uint8_t* req, uint32_t reqlen,
                           uint32_t* rescode, uint8_t* res, uint32_t* reslen) {
    vector<string> cmd;
    if (parse_req(req, reqlen, cmd) != 0) {
        msg("bad req");
        return -1;
    }

    if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
    } else {
        *rescode = RES_ERR;
        const char* err_text = "Unknown cmd";
        strcpy(reinterpret_cast<char*>(res), err_text);
        *reslen = (uint32_t)strlen(err_text);
    }
    return 0;
}

static void conn_put(vector<Conn*>& fd2conn, Conn* conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(vector<Conn*>& fd2conn, int fd) {
    sockaddr_in client_addr = {};
    socklen_t socklen = sizeof(client_addr);
    int connfd = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;
    }
    fd_set_nb(connfd);
    Conn* conn = new Conn();
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn_put(fd2conn, conn);
    return 0;
}

static bool try_flush_buffer(Conn* conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) return false;
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);

    if (conn->wbuf_sent == conn->wbuf_size) {
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    return true;
}

static void state_res(Conn* conn) {
    while (try_flush_buffer(conn)) {
    }
}

static bool try_one_request(Conn* conn) {
    if (conn->rbuf_size < 4) return false;

    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) return false;

    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(&conn->rbuf[4], len, &rescode, &conn->wbuf[4 + 4], &wlen);
    if (err) {
        conn->state = STATE_END;
        return false;
    }

    wlen += 4;
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], &rescode, 4);
    conn->wbuf_size = 4 + wlen;

    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn* conn) {
    assert(conn->rbuf_size < sizeof(conn->rbuf));

    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) return false;
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    while (try_one_request(conn)) {
    }
    return (conn->state == STATE_REQ);
}

static void state_req(Conn* conn) {
    while (try_fill_buffer(conn)) {
    }
}

static void connection_io(Conn* conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    int opt_val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5559);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        die("bind()");
    }
    if (listen(fd, SOMAXCONN) < 0) die("listen()");

    vector<Conn*> fd2conn;
    fd_set_nb(fd);

    printf("[server] key-value store listening on port 5559\n");

    vector<pollfd> poll_args;
    while (true) {
        poll_args.clear();
        pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        for (Conn* conn : fd2conn) {
            if (!conn) continue;
            pollfd cpfd = {};
            cpfd.fd = conn->fd;
            cpfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            cpfd.events = cpfd.events | POLLERR;
            poll_args.push_back(cpfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), 1000);
        if (rv < 0) {
            if (errno == EINTR) continue;
            die("poll");
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {
            if (poll_args[i].revents) {
                Conn* conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    fd2conn[conn->fd] = nullptr;
                    close(conn->fd);
                    delete conn;
                }
            }
        }

        if (poll_args[0].revents) {
            accept_new_conn(fd2conn, fd);
        }
    }

    return 0;
}
