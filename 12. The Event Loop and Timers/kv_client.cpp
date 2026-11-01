// learning/ch09/kv_client.cpp
//
// Chapter 9's client with ONE addition: it can now decode the new
// SER_DBL (decimal number) reply type, used for sorted-set scores.
//
// (Originally: chapter 7's client, upgraded to READ the typed reply format
// -- "deserialization".) Sending a command is unchanged; what's new is
// on_response(), which looks at the 1-byte type tag and decodes
// whatever follows. For arrays it calls ITSELF once per item, which
// is how nested values are handled.
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o kv_client kv_client.cpp
// Run:   ./kv_client zadd z 1.5 alice -> (int) 1
//        ./kv_client zscore z alice   -> (dbl) 1.5
//        ./kv_client set foo bar      -> (nil)
//        ./kv_client get foo          -> (str) bar
//        ./kv_client keys             -> (arr) len=1 ... (arr) end
//        ./kv_client del foo          -> (int) 1

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

// Same type tags as the server (SER_DBL is new, so SER_ARR moved to 5).
enum {
    SER_NIL = 0,
    SER_ERR = 1,
    SER_STR = 2,
    SER_INT = 3,
    SER_DBL = 4,
    SER_ARR = 5,
};

static void msg(const char* text) {
    fprintf(stderr, "%s\n", text);
}

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
// (unchanged from chapter 7)
static int32_t send_req(int fd, const vector<string>& cmd) {
    uint32_t len = 4;
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

// Decodes ONE value starting at data[0], prints it, and returns how
// many bytes that value occupied (so the caller knows where the next
// value starts). Returns -1 if the bytes are malformed.
static int32_t on_response(const uint8_t* data, size_t size) {
    if (size < 1) {
        msg("bad response");
        return -1;
    }

    switch (data[0]) {
    case SER_NIL:
        printf("(nil)\n");
        return 1;   // just the 1 type byte

    case SER_ERR: {
        if (size < 1 + 8) {
            msg("bad response");
            return -1;
        }
        int32_t code = 0;
        uint32_t len = 0;
        memcpy(&code, &data[1], 4);
        memcpy(&len, &data[1 + 4], 4);
        if (size < 1 + 8 + len) {
            msg("bad response");
            return -1;
        }
        printf("(err) %d %.*s\n", code, (int)len, &data[1 + 8]);
        return 1 + 8 + len;
    }

    case SER_STR: {
        if (size < 1 + 4) {
            msg("bad response");
            return -1;
        }
        uint32_t len = 0;
        memcpy(&len, &data[1], 4);
        if (size < 1 + 4 + len) {
            msg("bad response");
            return -1;
        }
        printf("(str) %.*s\n", (int)len, &data[1 + 4]);
        return 1 + 4 + len;
    }

    case SER_INT: {
        if (size < 1 + 8) {
            msg("bad response");
            return -1;
        }
        int64_t val = 0;
        memcpy(&val, &data[1], 8);
        printf("(int) %lld\n", (long long)val);
        return 1 + 8;
    }

    case SER_DBL: {
        if (size < 1 + 8) {
            msg("bad response");
            return -1;
        }
        double val = 0;
        memcpy(&val, &data[1], 8);
        printf("(dbl) %g\n", val);   // %g prints 1.1 as "1.1" and 2.0 as "2"
        return 1 + 8;
    }

    case SER_ARR: {
        if (size < 1 + 4) {
            msg("bad response");
            return -1;
        }
        uint32_t len = 0;
        memcpy(&len, &data[1], 4);
        printf("(arr) len=%u\n", len);

        size_t arr_bytes = 1 + 4;   // type byte + count
        for (uint32_t i = 0; i < len; ++i) {
            // decode the next item; it may itself be an array
            int32_t rv = on_response(&data[arr_bytes], size - arr_bytes);
            if (rv < 0) {
                return rv;
            }
            arr_bytes += (size_t)rv;
        }
        printf("(arr) end\n");
        return (int32_t)arr_bytes;
    }

    default:
        msg("bad response");
        return -1;
    }
}

static int32_t read_res(int fd) {
    char rbuf[4 + k_max_msg + 1];
    if (read_full(fd, rbuf, 4) < 0) {
        msg("read() error on header");
        return -1;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }
    if (read_full(fd, &rbuf[4], len) < 0) {
        msg("read() error on body");
        return -1;
    }

    int32_t rv = on_response((uint8_t*)&rbuf[4], len);
    if (rv < 0) {
        return -1;
    }
    if ((uint32_t)rv != len) {
        msg("bad response");   // the value didn't use up exactly `len` bytes
        return -1;
    }
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
