// learning/ch06/state_machine_server.cpp
//
// Matches the PDF book edition you're following, NOT the newer live
// website version (see event_server.cpp in this same folder for that
// one). Same event loop idea, different concrete design:
//   - an explicit 3-state enum per connection: STATE_REQ (reading),
//     STATE_RES (writing), STATE_END (tear down)
//   - fixed-size uint8_t buffers (rbuf/wbuf) instead of std::vector
//   - memmove() to shift leftover bytes to the front after consuming
//     a message, instead of vector::erase()
//
// Build: g++ -std=c++17 -Wall -Wextra -o state_machine_server state_machine_server.cpp
// Run:   ./state_machine_server

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

const size_t k_max_msg = 4096;

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
    STATE_REQ = 0,  // reading a request
    STATE_RES = 1,  // sending a response
    STATE_END = 2,  // connection is done, tear it down
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

static void conn_put(std::vector<Conn*>& fd2conn, Conn* conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn*>& fd2conn, int fd) {
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
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);
    return 0;
}

// Flushes as much of wbuf as the socket will currently accept.
static bool try_flush_buffer(Conn* conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = write(conn->fd, &conn->wbuf[conn->wbuf_sent], remain);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        return false;  // socket isn't writable right now, try again later
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }

    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);

    if (conn->wbuf_sent == conn->wbuf_size) {
        // Fully flushed — go back to reading the next request.
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    return true;  // still more to send, worth trying again immediately
}

static void state_res(Conn* conn) {
    while (try_flush_buffer(conn)) {
        // keep flushing
    }
}

// Pulls exactly one complete framed message out of rbuf, if one is
// fully present, builds its echo response into wbuf, and switches the
// connection into STATE_RES to send it.
static bool try_one_request(Conn* conn) {
    if (conn->rbuf_size < 4) {
        return false;  // don't even have the length prefix yet
    }

    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) {
        return false;  // header arrived, body hasn't fully arrived yet
    }

    printf("client says: %.*s\n", (int)len, &conn->rbuf[4]);

    // Build the echo response straight into wbuf.
    memcpy(&conn->wbuf[0], &len, 4);
    memcpy(&conn->wbuf[4], &conn->rbuf[4], len);
    conn->wbuf_size = 4 + len;

    // Shift any leftover bytes (the start of a pipelined next request,
    // if the client sent more than one back to back) to the front.
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    conn->state = STATE_RES;
    state_res(conn);

    // If state_res immediately finished (common case: small reply,
    // socket writable right away), we're back in STATE_REQ and should
    // let the outer loop try to parse another pipelined request.
    return (conn->state == STATE_REQ);
}

// Reads whatever is currently available into rbuf, looping until
// EAGAIN, and tries to parse as many complete pipelined requests as
// have arrived.
static bool try_fill_buffer(Conn* conn) {
    assert(conn->rbuf_size < sizeof(conn->rbuf));

    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = read(conn->fd, &conn->rbuf[conn->rbuf_size], cap);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0 && errno == EAGAIN) {
        return false;  // nothing more available right now
    }
    if (rv < 0) {
        msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // A client can pipeline several requests without waiting for
    // replies in between, so keep extracting until there isn't a full
    // one left in the buffer.
    while (try_one_request(conn)) {
        // loop
    }
    return (conn->state == STATE_REQ);
}

static void state_req(Conn* conn) {
    while (try_fill_buffer(conn)) {
        // loop
    }
}

static void connection_io(Conn* conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);  // not expected
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket()");

    int opt_val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5558);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        die("bind()");
    }
    if (listen(fd, SOMAXCONN) < 0) die("listen()");

    std::vector<Conn*> fd2conn;
    fd_set_nb(fd);

    printf("[server] listening on port 5558\n");

    std::vector<pollfd> poll_args;
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
