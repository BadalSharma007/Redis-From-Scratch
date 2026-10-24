// learning/ch09/kv_server.cpp
//
// Chapter 8's server, with a new RESPONSE FORMAT ("serialization").
//
// Before: every reply was [status code][string]. That can't express
// "a list of keys" or "a number". Now every reply is a TYPED value:
//
//   1 byte type, then a payload that depends on the type:
//     SER_NIL  (0)  nothing                       -> "no value" (like null)
//     SER_ERR  (1)  [4B code][4B len][message]    -> an error
//     SER_STR  (2)  [4B len][bytes]               -> a string
//     SER_INT  (3)  [8B int64]                    -> a number
//     SER_ARR  (4)  [4B count][item][item]...     -> a list; each item is
//                                                    itself a typed value
//                                                    (so arrays can nest)
//
// The whole reply is still wrapped in the same 4-byte length prefix
// from chapter 4, so the event loop code does not change at all.
//
// New command: keys  (returns every key as an array of strings)
//
// Build (two .cpp files compiled together):
//   g++ -std=c++17 -Wall -Wextra -O2 -o kv_server kv_server.cpp hashtable.cpp
// Run:   ./kv_server
// Test:  with THIS chapter's kv_client (the chapter 7 client can no
//        longer read the new reply format).

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
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "hashtable.h"

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
// Serialization: the typed reply format
// ---------------------------------------------------------------

// Error codes carried inside a SER_ERR reply.
enum {
    ERR_UNKNOWN = 1,   // command not recognized
    ERR_2BIG = 2,      // reply would not fit in the buffer
};

// The 1-byte type tag at the start of every value.
enum {
    SER_NIL = 0,
    SER_ERR = 1,
    SER_STR = 2,
    SER_INT = 3,
    SER_ARR = 4,
};

// Each out_* function APPENDS one value to the string `out`.
// A std::string is just a convenient growable byte buffer here.
// (A byte is appended with push_back; raw bytes of a number are
// appended by pointing at the number and saying "copy 4 or 8 bytes".)

static void out_nil(string& out) {
    out.push_back(SER_NIL);
}

static void out_str(string& out, const string& val) {
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)val.size();
    out.append((char*)&len, 4);   // length first, so the reader knows how much to read
    out.append(val);
}

static void out_int(string& out, int64_t val) {
    out.push_back(SER_INT);
    out.append((char*)&val, 8);
}

static void out_err(string& out, int32_t code, const string& text) {
    out.push_back(SER_ERR);
    out.append((char*)&code, 4);
    uint32_t len = (uint32_t)text.size();
    out.append((char*)&len, 4);
    out.append(text);
}

// An array header only says HOW MANY items follow. The caller then
// appends exactly that many values right after it.
static void out_arr(string& out, uint32_t n) {
    out.push_back(SER_ARR);
    out.append((char*)&n, 4);
}

// ---------------------------------------------------------------
// The database (unchanged from chapter 8 except do_* now write
// typed values into `out` instead of returning a status code)
// ---------------------------------------------------------------

static bool cmd_is(const string& word, const char* cmd) {
    return word == cmd;
}

struct Entry {
    HNode node;      // intrusive hashtable link
    string key;
    string val;
};

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static struct {
    HMap db;
} g_data;

static uint64_t str_hash(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;   // FNV-1a
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool entry_eq(HNode* lhs, HNode* rhs) {
    Entry* le = container_of(lhs, Entry, node);
    Entry* re = container_of(rhs, Entry, node);
    return lhs->hcode == rhs->hcode && le->key == re->key;
}

// get: reply with the string, or nil if the key doesn't exist
static void do_get(vector<string>& cmd, string& out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        out_nil(out);
        return;
    }
    const string& val = container_of(node, Entry, node)->val;
    out_str(out, val);
}

// set: store the value, reply nil
static void do_set(vector<string>& cmd, string& out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } else {
        Entry* ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    out_nil(out);
}

// del: reply with an integer, 1 if something was deleted, 0 if not
static void do_del(vector<string>& cmd, string& out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());

    HNode* node = hm_pop(&g_data.db, &key.node, &entry_eq);
    if (node) {
        delete container_of(node, Entry, node);
    }
    out_int(out, node ? 1 : 0);
}

// ---- keys: walk the whole hashtable and list every key ----

// Visit every node in one fixed table, calling f(node, arg) for each.
// f is a "callback": a function we are handed and call back later.
// `arg` is a void* so the caller can pass along anything it needs
// (here: the output string).
static void h_scan(HTab* tab, void (*f)(HNode*, void*), void* arg) {
    if (tab->size == 0) {
        return;
    }
    for (size_t i = 0; i < tab->mask + 1; ++i) {   // every bucket
        HNode* node = tab->tab[i];
        while (node) {                              // every node in the chain
            f(node, arg);
            node = node->next;
        }
    }
}

// The callback for keys: append this entry's key as a string value.
static void cb_scan(HNode* node, void* arg) {
    string& out = *(string*)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

static void do_keys(vector<string>& cmd, string& out) {
    (void)cmd;
    // Array header first (it needs the count), then that many strings.
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    // During a resize, keys can live in either table, so scan both.
    h_scan(&g_data.db.ht1, &cb_scan, &out);
    h_scan(&g_data.db.ht2, &cb_scan, &out);
}

// ---------------------------------------------------------------
// Request parsing and dispatch
// ---------------------------------------------------------------

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
    if (pos != len) return -1;
    return 0;
}

// Pick the right command. Whatever it appends to `out` IS the reply.
static void do_request(vector<string>& cmd, string& out) {
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys")) {
        do_keys(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        do_del(cmd, out);
    } else {
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

// ---------------------------------------------------------------
// Event loop (unchanged from chapters 6-8 except try_one_request)
// ---------------------------------------------------------------

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

// The part that changed: parse the request, run it to get a typed
// reply in `out`, then copy [length][reply bytes] into wbuf.
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

    // 1. parse the request bytes into a list of strings
    vector<string> cmd;
    if (parse_req(&conn->rbuf[4], len, cmd) != 0) {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    // 2. run the command; it appends the typed reply to `out`
    string out;
    do_request(cmd, out);

    // 3. pack the reply into the write buffer
    if (4 + out.size() > k_max_msg) {
        // too big to send (e.g. `keys` on a huge database): replace it
        // with a small error instead
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }
    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
    conn->wbuf_size = 4 + wlen;

    // 4. remove this request from the read buffer
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

    printf("[server] listening on port 5559\n");

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
