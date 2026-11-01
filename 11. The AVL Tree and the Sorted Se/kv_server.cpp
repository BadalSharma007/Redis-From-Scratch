// learning/ch11/kv_server.cpp
//
// Chapter 9's server plus SORTED SETS. What changed:
//
//   1. A key can now hold either a STRING or a SORTED SET (Entry::type).
//   2. New reply type SER_DBL (a decimal number) for scores.
//   3. New array helpers that write the item count AFTER the items are
//      written (zquery doesn't know how many items it will return until
//      it has walked them).
//   4. New errors: ERR_TYPE (wrong kind of key) and ERR_ARG (bad number).
//   5. Four new commands:
//        zadd   key score name            -> (int) 1 if new, 0 if updated
//        zrem   key name                  -> (int) 1 if removed, 0 if absent
//        zscore key name                  -> (dbl) score, or (nil)
//        zquery key score name offset limit
//               -> array of name, score, name, score, ...
//   (The chapter calls this part "trivial" and omits it, so the exact
//    handlers are written from the behaviour of the book's test cases.)
//
// The event loop is unchanged from chapters 6-9.
//
// Build (several .cpp files compiled together):
//   g++ -std=c++17 -Wall -Wextra -O2 -o kv_server kv_server.cpp zset.cpp avl.cpp hashtable.cpp
// Run:   ./kv_server
// Test:  with THIS chapter's kv_client (it must understand the new SER_DBL type).

#include <arpa/inet.h>
#include <assert.h>
#include <cerrno>
#include <cmath>
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

#include "common.h"
#include "hashtable.h"
#include "zset.h"

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

enum {
    ERR_UNKNOWN = 1,   // command not recognized
    ERR_2BIG = 2,      // reply would not fit in the buffer
    ERR_TYPE = 3,      // the key exists but holds the wrong kind of value
    ERR_ARG = 4,       // an argument is not a valid number
};

// The 1-byte type tag at the start of every value.
// NOTE: SER_DBL is new, so SER_ARR moved from 4 to 5. The client uses
// the same numbers.
enum {
    SER_NIL = 0,
    SER_ERR = 1,
    SER_STR = 2,
    SER_INT = 3,
    SER_DBL = 4,   // NEW: [8-byte double]
    SER_ARR = 5,
};

static void out_nil(string& out) {
    out.push_back(SER_NIL);
}

// A string given as (pointer, length): used for the name inside a ZNode,
// which is not a std::string.
static void out_str(string& out, const char* s, size_t size) {
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)size;
    out.append((char*)&len, 4);
    out.append(s, size);
}

static void out_str(string& out, const string& val) {
    out_str(out, val.data(), val.size());
}

static void out_int(string& out, int64_t val) {
    out.push_back(SER_INT);
    out.append((char*)&val, 8);
}

static void out_dbl(string& out, double val) {
    out.push_back(SER_DBL);
    out.append((char*)&val, 8);
}

static void out_err(string& out, int32_t code, const string& text) {
    out.push_back(SER_ERR);
    out.append((char*)&code, 4);
    uint32_t len = (uint32_t)text.size();
    out.append((char*)&len, 4);
    out.append(text);
}

// Array whose item count is known up front.
static void out_arr(string& out, uint32_t n) {
    out.push_back(SER_ARR);
    out.append((char*)&n, 4);
}

// Array whose item count is NOT known yet:
//   out_begin_arr writes the type byte and a 4-byte placeholder, and
//   returns WHERE the placeholder is; once the items have been written,
//   out_end_arr goes back and fills in the real count.
static size_t out_begin_arr(string& out) {
    out.push_back(SER_ARR);
    out.append(4, '\0');            // placeholder for the count
    return out.size() - 4;          // position of the placeholder
}

static void out_end_arr(string& out, size_t pos, uint32_t n) {
    assert(out[pos - 1] == SER_ARR);
    memcpy(&out[pos], &n, 4);
}

// ---------------------------------------------------------------
// Parsing numbers from command arguments
// ---------------------------------------------------------------

// Whole string must be a valid decimal number (and not NaN).
static bool str2dbl(const string& s, double& out) {
    char* endp = nullptr;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !isnan(out);
}

static bool str2int(const string& s, int64_t& out) {
    char* endp = nullptr;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}

// ---------------------------------------------------------------
// The database
// ---------------------------------------------------------------

static bool cmd_is(const string& word, const char* cmd) {
    return word == cmd;
}

enum {
    T_STR = 0,    // the key holds a string
    T_ZSET = 1,   // the key holds a sorted set
};

// What we store per key. Only ONE of val / zset is used, depending on type.
struct Entry {
    HNode node;               // intrusive hashtable link
    string key;
    string val;               // used when type == T_STR
    uint32_t type = T_STR;
    ZSet* zset = nullptr;     // used when type == T_ZSET
};

static struct {
    HMap db;
} g_data;

static bool entry_eq(HNode* lhs, HNode* rhs) {
    Entry* le = container_of(lhs, Entry, node);
    Entry* re = container_of(rhs, Entry, node);
    return lhs->hcode == rhs->hcode && le->key == re->key;
}

// Free an Entry, including the sorted set it may own.
static void entry_del(Entry* ent) {
    if (ent->type == T_ZSET) {
        zset_dispose(ent->zset);
        delete ent->zset;
    }
    delete ent;
}

// A "probe" is a temporary Entry on the stack used only to search the
// hashtable. The key string is MOVED into it with swap (O(1)), not copied.
static void make_probe(Entry& probe, string& key) {
    probe.key.swap(key);
    probe.node.hcode = str_hash((uint8_t*)probe.key.data(), probe.key.size());
}

// Returns the stored Entry matching the probe, or nullptr.
static Entry* find_entry(Entry& probe) {
    HNode* node = hm_lookup(&g_data.db, &probe.node, &entry_eq);
    return node ? container_of(node, Entry, node) : nullptr;
}

// ---- string commands (chapter 9, now aware of key types) ----

static void do_get(vector<string>& cmd, string& out) {
    Entry probe;
    make_probe(probe, cmd[1]);
    Entry* ent = find_entry(probe);
    if (!ent) {
        out_nil(out);
        return;
    }
    if (ent->type != T_STR) {
        out_err(out, ERR_TYPE, "expect string type");
        return;
    }
    out_str(out, ent->val);
}

static void do_set(vector<string>& cmd, string& out) {
    Entry probe;
    make_probe(probe, cmd[1]);
    Entry* ent = find_entry(probe);
    if (ent) {
        if (ent->type != T_STR) {
            out_err(out, ERR_TYPE, "a non-string value exists");
            return;
        }
        ent->val.swap(cmd[2]);
    } else {
        ent = new Entry();
        ent->key.swap(probe.key);
        ent->node.hcode = probe.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    out_nil(out);
}

static void do_del(vector<string>& cmd, string& out) {
    Entry probe;
    make_probe(probe, cmd[1]);
    HNode* node = hm_pop(&g_data.db, &probe.node, &entry_eq);
    if (node) {
        entry_del(container_of(node, Entry, node));
    }
    out_int(out, node ? 1 : 0);
}

// ---- keys ----

static void h_scan(HTab* tab, void (*f)(HNode*, void*), void* arg) {
    if (tab->size == 0) {
        return;
    }
    for (size_t i = 0; i < tab->mask + 1; ++i) {
        HNode* node = tab->tab[i];
        while (node) {
            f(node, arg);
            node = node->next;
        }
    }
}

static void cb_scan(HNode* node, void* arg) {
    string& out = *(string*)arg;
    out_str(out, container_of(node, Entry, node)->key);
}

static void do_keys(vector<string>& cmd, string& out) {
    (void)cmd;
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    h_scan(&g_data.db.ht1, &cb_scan, &out);
    h_scan(&g_data.db.ht2, &cb_scan, &out);
}

// ---- sorted-set commands (NEW) ----

// zadd key score name
static void do_zadd(vector<string>& cmd, string& out) {
    // Check the argument FIRST, so a bad score doesn't leave behind an
    // empty sorted set that was created for nothing.
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        out_err(out, ERR_ARG, "expect fp number");
        return;
    }

    Entry probe;
    make_probe(probe, cmd[1]);
    Entry* ent = find_entry(probe);
    if (!ent) {
        // the key doesn't exist yet: create it as an empty sorted set
        ent = new Entry();
        ent->key.swap(probe.key);
        ent->node.hcode = probe.node.hcode;
        ent->type = T_ZSET;
        ent->zset = new ZSet();
        hm_insert(&g_data.db, &ent->node);
    } else if (ent->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expect zset");
        return;
    }

    const string& name = cmd[3];
    bool added = zset_add(ent->zset, name.data(), name.size(), score);
    out_int(out, (int64_t)added);
}

// zrem key name
// (Like the book, a sorted set that becomes empty stays as an empty key.)
static void do_zrem(vector<string>& cmd, string& out) {
    Entry probe;
    make_probe(probe, cmd[1]);
    Entry* ent = find_entry(probe);
    if (!ent) {
        out_int(out, 0);
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expect zset");
        return;
    }

    const string& name = cmd[2];
    ZNode* znode = zset_pop(ent->zset, name.data(), name.size());
    bool removed = (znode != nullptr);   // remember BEFORE freeing the node
    if (znode) {
        znode_del(znode);
    }
    out_int(out, removed ? 1 : 0);
}

// zscore key name
static void do_zscore(vector<string>& cmd, string& out) {
    Entry probe;
    make_probe(probe, cmd[1]);
    Entry* ent = find_entry(probe);
    if (!ent) {
        out_nil(out);
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expect zset");
        return;
    }

    const string& name = cmd[2];
    ZNode* znode = zset_lookup(ent->zset, name.data(), name.size());
    if (!znode) {
        out_nil(out);
        return;
    }
    out_dbl(out, znode->score);
}

// zquery key score name offset limit
//   1. find the first item >= (score, name)
//   2. move `offset` places from it (can be negative)
//   3. return up to `limit` ARRAY ELEMENTS, as name, score, name, score...
//      (each pair uses 2 elements, so limit 10 means at most 5 pairs)
static void do_zquery(vector<string>& cmd, string& out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        out_err(out, ERR_ARG, "expect fp number");
        return;
    }
    const string& name = cmd[3];
    int64_t offset = 0;
    int64_t limit = 0;
    if (!str2int(cmd[4], offset)) {
        out_err(out, ERR_ARG, "expect int");
        return;
    }
    if (!str2int(cmd[5], limit)) {
        out_err(out, ERR_ARG, "expect int");
        return;
    }

    Entry probe;
    make_probe(probe, cmd[1]);
    Entry* ent = find_entry(probe);
    if (!ent) {
        out_arr(out, 0);          // no such key: an empty result, not an error
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_TYPE, "expect zset");
        return;
    }
    if (limit <= 0) {
        out_arr(out, 0);
        return;
    }

    ZNode* znode = zset_query(ent->zset, score, name.data(), name.size(), offset);

    // We don't know the item count until we've walked the nodes, so use
    // the begin/end pair that fills the count in afterwards.
    size_t arr = out_begin_arr(out);
    uint32_t n = 0;
    while (znode && (int64_t)n < limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        AVLNode* next = avl_offset(&znode->tree, +1);   // step to the next item in order
        znode = next ? container_of(next, ZNode, tree) : nullptr;
        n += 2;                                         // a name and a score
    }
    out_end_arr(out, arr, n);
}

// ---------------------------------------------------------------
// Request parsing and dispatch
// ---------------------------------------------------------------

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

static void do_request(vector<string>& cmd, string& out) {
    if (cmd.size() == 1 && cmd_is(cmd[0], "keys")) {
        do_keys(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        do_del(cmd, out);
    } else if (cmd.size() == 4 && cmd_is(cmd[0], "zadd")) {
        do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zrem")) {
        do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd_is(cmd[0], "zscore")) {
        do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd_is(cmd[0], "zquery")) {
        do_zquery(cmd, out);
    } else {
        out_err(out, ERR_UNKNOWN, "Unknown cmd");
    }
}

// ---------------------------------------------------------------
// Event loop (unchanged from chapters 6-9)
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

    vector<string> cmd;
    if (parse_req(&conn->rbuf[4], len, cmd) != 0) {
        msg("bad req");
        conn->state = STATE_END;
        return false;
    }

    string out;
    do_request(cmd, out);

    if (4 + out.size() > k_max_msg) {
        out.clear();
        out_err(out, ERR_2BIG, "response is too big");
    }
    uint32_t wlen = (uint32_t)out.size();
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], out.data(), out.size());
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
