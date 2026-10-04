// learning/ch05/event_server.cpp
//
// The big shift from chapter 4: instead of one blocking connection at
// a time, this server holds ALL connections open simultaneously and
// uses poll() to ask the kernel "which of these fds actually have
// something ready?" before touching any of them.
//
// This forces a real change in how state is managed: you can no longer
// call read_full() and just block until a whole message arrives,
// because blocking on one client would freeze every other client too.
// Instead, each connection gets its own persistent buffer, and we only
// ever try to read "whatever's available right now" and see if that's
// enough to complete a full framed message yet.
//
// Uses the SAME 4-byte length-prefix framing from chapter 4 (and the
// same port, 5556) — so chapter 4's proto_client.cpp works against
// this server completely unmodified. That's the point: same protocol,
// fundamentally different concurrency model underneath.
//
// Build:   g++ -std=c++17 -Wall -Wextra -o event_server event_server.cpp
// Run:     ./event_server

#include <arpa/inet.h>
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
#include <unordered_map>
#include <vector>

static const size_t k_max_msg = 4096;

// Print error message and exit program.
static void die(const char* what) {
    perror(what);
    exit(1);
}

// Make the file descriptor non-blocking.
static void set_nonblocking(int fd) {
    // Get current flags of the fd.
    int flags = fcntl(fd, F_GETFL, 0);
    // Set fd flags to non-blocking mode.
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Per-connection state. Unlike chapters 3-4, this has to survive
// across multiple event loop iterations, since a full message might
// not arrive in a single read().
struct Conn {
    int fd = -1;
    bool want_close = false;
    std::vector<uint8_t> incoming;  // bytes read but not yet parsed
    std::vector<uint8_t> outgoing;  // bytes queued to send, not yet written
};

// Reads whatever is currently available (non-blocking, so this never
// waits), appends it to the connection's buffer, then tries to peel
// off as many COMPLETE framed messages as the buffer now contains.
static void handle_readable(Conn& conn) {
    // Temporary buffer for reading data.
    uint8_t buf[4096];
    while (true) {
        // Read data from connection fd.
        ssize_t n = read(conn.fd, buf, sizeof(buf));
        if (n > 0) {
            // Append read bytes to incoming buffer.
            conn.incoming.insert(conn.incoming.end(), buf, buf + n);
        } else if (n == 0) {
            // Connection closed by peer.
            conn.want_close = true;
            return;
        } else {
            // If no data available now, break loop.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // On other errors, mark connection to close.
            conn.want_close = true;
            return;
        }
    }

    while (true) {
        // Check if we have at least 4 bytes for length prefix.
        if (conn.incoming.size() < 4) break;

        // Read message body length from first 4 bytes.
        uint32_t body_len = 0;
        memcpy(&body_len, conn.incoming.data(), 4);
        // Check for oversized messages.
        if (body_len > k_max_msg) {
            fprintf(stderr, "[server] fd %d sent an oversized message\n", conn.fd);
            conn.want_close = true;
            return;
        }
        // Wait for full message if not all bytes arrived yet.
        if (conn.incoming.size() < 4 + body_len) {
            break;
        }

        // Extract message body as string.
        std::string body(reinterpret_cast<char*>(conn.incoming.data()) + 4, body_len);
        // Print received message.
        printf("[server] fd %d says: %s\n", conn.fd, body.c_str());

        // Remove processed message from incoming buffer.
        conn.incoming.erase(conn.incoming.begin(), conn.incoming.begin() + 4 + body_len);

        // Prepare reply message.
        const char reply_text[] = "message received";
        uint32_t reply_len = (uint32_t)strlen(reply_text);
        // Add length prefix to outgoing buffer.
        conn.outgoing.insert(conn.outgoing.end(),
                              reinterpret_cast<uint8_t*>(&reply_len),
                              reinterpret_cast<uint8_t*>(&reply_len) + 4);
        // Add reply text to outgoing buffer.
        conn.outgoing.insert(conn.outgoing.end(), reply_text, reply_text + reply_len);
    }
}

// Write as much as possible from outgoing buffer to the connection.
static void handle_writable(Conn& conn) {
    // Return if no data to send.
    if (conn.outgoing.empty()) return;

    // Write data to fd.
    ssize_t n = write(conn.fd, conn.outgoing.data(), conn.outgoing.size());
    if (n > 0) {
        // Remove sent bytes from outgoing buffer.
        conn.outgoing.erase(conn.outgoing.begin(), conn.outgoing.begin() + n);
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // On write error, mark connection to close.
        conn.want_close = true;
    }
}

int main() {
    // Create TCP socket.
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    // Check if socket creation failed.
    if (listen_fd < 0) die("socket()");

    // Enable address reuse option.
    int opt_val = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

    // Prepare server address structure.
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5556);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind socket to the address and port.
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        die("bind()");
    }
    // Put socket into listening mode.
    if (listen(listen_fd, SOMAXCONN) < 0) die("listen()");
    // Make listening socket non-blocking.
    set_nonblocking(listen_fd);

    // Print server start message.
    printf("[server] event loop listening on port 5556\n");

    // Map of active connections by fd.
    std::unordered_map<int, Conn> conns;

    while (true) {
        // Vector to hold pollfd structs for poll().
        std::vector<pollfd> poll_args;
        // Add listening socket to poll list.
        poll_args.push_back({listen_fd, POLLIN, 0});

        // Add all active connections to poll list.
        for (auto& [fd, conn] : conns) {
            pollfd pfd{};
            pfd.fd = fd;
            // Watch for read and write readiness.
            pfd.events = POLLIN | (conn.outgoing.empty() ? 0 : POLLOUT);
            poll_args.push_back(pfd);
        }

        // Wait for events on all fds.
        int ready = poll(poll_args.data(), poll_args.size(), -1);
        // If poll was interrupted, retry.
        if (ready < 0) {
            if (errno == EINTR) continue;
            die("poll()");
        }

        // List of fds to close after handling events.
        std::vector<int> to_close;

        // Iterate over all poll results.
        for (auto& pfd : poll_args) {
            // Check if event is on listening socket.
            if (pfd.fd == listen_fd) {
                // If ready to read, accept new connections.
                if (pfd.revents & POLLIN) {
                    while (true) {
                        // Accept new client connection.
                        int client_fd = accept(listen_fd, nullptr, nullptr);
                        // No more connections to accept now.
                        if (client_fd < 0) break;
                        // Make client socket non-blocking.
                        set_nonblocking(client_fd);
                        // Create new connection state.
                        Conn new_conn;
                        new_conn.fd = client_fd;
                        // Add new connection to map.
                        conns[client_fd] = new_conn;
                        // Print new connection message.
                        printf("[server] new connection: fd %d\n", client_fd);
                    }
                }
                continue;
            }

            // Find connection state for this fd.
            auto it = conns.find(pfd.fd);
            if (it == conns.end()) continue;
            Conn& conn = it->second;

            // If ready to read, handle incoming data.
            if (pfd.revents & POLLIN) handle_readable(conn);
            // If ready to write and not closing, send outgoing data.
            if (!conn.want_close && (pfd.revents & POLLOUT)) handle_writable(conn);
            // If connection marked to close, add to close list.
            if (conn.want_close) to_close.push_back(conn.fd);
        }

        // Close and remove all connections marked for closing.
        for (int fd : to_close) {
            // Print closing message.
            printf("[server] closing fd %d\n", fd);
            // Close socket.
            close(fd);
            // Remove from connection map.
            conns.erase(fd);
        }
    }

    // Return success code.
    return 0;
}
// fcntl -> function used to get/change settings (flags) of a file descriptor
// F_GETFL -> get the current flags of an fd
// F_SETFL -> set/change the flags of an fd
// O_NONBLOCK -> makes an fd nonblocking, so it does not wait when data/work is not ready
// nonblocking -> operation returns immediately instead of waiting
// EAGAIN -> not ready right now; try again later
// EWOULDBLOCK -> same idea as EAGAIN here; operation would block, so try later
// errno -> stores the error code from a failed system call
// Conn -> struct storing the state/data of one client connection
// persistent state -> information kept between event-loop iterations
// incoming -> buffer holding received bytes that are not fully processed yet
// outgoing -> buffer holding bytes waiting to be sent
// uint8_t -> unsigned 8-bit integer; commonly used to store one raw byte
// ssize_t -> signed integer type used by read/write to return byte count or -1 on error
// pollfd -> structure used by poll() to describe one fd and the events we want to watch
// poll_args -> list of fds given to poll() in the current event-loop iteration
// poll() -> waits and tells us which fds are ready for I/O
// POLLIN -> ask/check whether an fd is ready for reading
// POLLOUT -> ask/check whether an fd is ready for writing
// events -> events we ask poll() to watch for
// revents -> events that actually happened and were reported by poll()
// ready -> number of fds for which poll() reported an event
// event loop -> loop that repeatedly waits for ready fds and processes them
// EINTR -> poll/system call was interrupted by a signal; retry it
// unordered_map -> key-value container; here maps fd to its Conn state
// conns -> stores all currently active client connections by fd
// to_close -> temporary list of client fds that should be closed
// iterator (it) -> object used to point to/find an element inside a container
// reinterpret_cast -> C++ cast used here to treat the same memory as another pointer type
// framing -> message format with a length header followed by the message body
// partial read -> read() received only part of the expected message
// partial write -> write() sent only part of the queued data
// SO_REUSEADDR -> socket option allowing reuse of local addresses (helps restart server quickly)
// SOMAXCONN -> maximum number of pending connections allowed in listen queue
// INADDR_ANY -> special IP address meaning "all interfaces" on the local machine
// nullptr -> C++ keyword representing a null pointer (no address)
// insert -> vector method to add elements at a specific position
// erase -> vector method to remove elements from a specific range
// data() -> returns pointer to underlying array of vector or string
// empty() -> returns true if container has no elements
// ternary operator -> shorthand if-else expression, e.g. (cond ? val1 : val2)