// learning/ch04/proto_client.cpp
// Client program: sends several messages over ONE TCP connection.
// Framing = adding a fixed-size length header before each message so the receiver knows message boundaries.
// Build: g++ -std=c++17 -Wall -Wextra -o proto_client proto_client.cpp
// Run: ./proto_client (with proto_server already running)

#include <arpa/inet.h>      // Network address functions; converts IP/address values between formats.
#include <cstdint>          // Fixed-width integer types such as uint32_t.
#include <cstdio>           // C-style input/output functions such as printf() and fprintf().
#include <cstdlib>          // General utilities such as exit().
#include <cstring>          // C-string/memory functions such as strlen() and memcpy().
#include <netinet/in.h>     // IPv4 socket structures such as sockaddr_in.
#include <sys/socket.h>     // Socket API functions such as socket() and connect().
#include <unistd.h>         // POSIX functions such as read(), write(), and close().

static const size_t k_max_msg = 4096; // Maximum message size allowed by this client: 4096 bytes.
                                      // size_t = unsigned integer type commonly used for sizes/counts.
                                      // static here means this variable is limited to this source file.

static void die(const char* what) {   // Helper function: print the OS error and stop the program.
                                      // const char* = pointer to a C-style string that this function will not modify.
    perror(what);                     // perror = prints a human-readable error message from the last OS error.
    exit(1);                           // exit(1) = terminate the program; non-zero usually means failure.
}                                    // End of die().

static int read_full(int fd, char* buf, size_t n) { // Read exactly n bytes from socket fd.
                                                    // fd = file descriptor, an integer handle used by the OS for the socket.
                                                    // buf = memory buffer where received bytes are stored.
                                                    // n = number of bytes still needed.
    while (n > 0) {                                // Keep reading until all requested bytes have arrived.
        ssize_t got  = read(fd, buf, n);            // read() asks the OS for up to n bytes from the socket.
                                                    // ssize_t = signed type used for byte counts returned by read/write.
        if (got <= 0) return -1;                    // 0 = connection closed; negative = read error; -1 signals failure.
        buf += got;                                 // Move the buffer pointer forward by the bytes already received.
        n -= (size_t)got;                           // Reduce the number of bytes still needed.
    }                                             // End of read loop.
    return 0;                                      // 0 signals success.
}                                                 // End of read_full().

static int write_all(int fd, const char* buf, size_t n) { // Write exactly n bytes to socket fd.
                                                          // const char* means this function reads from buf but does not modify it.
    while (n > 0) {                                       // Keep writing until every byte has been sent.
        ssize_t sent = write(fd, buf, n);                 // write() asks the OS to send up to n bytes through the socket.
        if (sent <= 0) return -1;                         // 0 or negative means the send did not succeed.
        buf += sent;                                      // Move the buffer pointer past the bytes already sent.
        n -= (size_t)sent;                                // Reduce the number of bytes still waiting to be sent.
    }                                                     // End of write loop.
    return 0;                                             // 0 signals success.
}                                                         // End of write_all().

// Sends one framed message, then waits for the server's framed reply.
// Frame = [4-byte length][message body]. This creates clear message boundaries on a TCP byte stream.
static int send_and_receive(int fd, const char* text) { // Send one request and receive one response on the same connection.
    uint32_t body_len = (uint32_t)strlen(text);          // strlen() counts characters/bytes before '\0'; store that length in 32 bits.
                                                         // uint32_t = unsigned integer guaranteed to be exactly 32 bits.

    char out_buf[4 + k_max_msg];                          // Buffer containing 4-byte length header + message body.
                                                         // char = 1-byte storage; here it is used as a raw byte buffer.
    memcpy(out_buf, &body_len, 4);                        // Copy the 4-byte message length into the beginning of the buffer.
                                                         // memcpy() copies raw bytes; it does not care about string termination.
    memcpy(out_buf + 4, text, body_len);                  // Copy the message immediately after the 4-byte length header.
                                                         // out_buf + 4 = pointer to the fifth byte, where the body starts.
    if (write_all(fd, out_buf, 4 + body_len) < 0) {       // Send the complete frame: header + body.
        fprintf(stderr, "[client] failed to send message\n"); // fprintf(stderr, ...) prints an error to the standard error stream.
        return -1;                                       // Tell the caller that sending failed.
    }                                                     // End of send-error check.

    char header[4];                                       // Four-byte buffer used to receive the reply length header.
    if (read_full(fd, header, 4) < 0) {                   // Read exactly four bytes from the server.
        fprintf(stderr, "[client] failed to read reply header\n"); // Print a reply-header error.
        return -1;                                       // Tell the caller that receiving failed.
    }                                                     // End of header-read check.
    uint32_t reply_len = 0;                               // Variable that will store the server reply's body length.
    memcpy(&reply_len, header, 4);                        // Copy the 4 received header bytes into reply_len.
    if (reply_len > k_max_msg) {                          // Reject a reply larger than our allowed maximum.
        fprintf(stderr, "[client] reply too large\n");   // Print a size-limit error.
        return -1;                                       // Stop processing this request.
    }                                                     // End of size check.

    char reply_body[k_max_msg + 1];                       // Buffer for reply body plus one extra byte for '\0'.
    if (read_full(fd, reply_body, reply_len) < 0) {       // Read exactly reply_len bytes from the server.
        fprintf(stderr, "[client] failed to read reply body\n"); // Print a body-read error.
        return -1;                                       // Tell the caller that receiving failed.
    }                                                     // End of body-read check.
    reply_body[reply_len] = '\0';                         // Add C-string terminator so reply_body can safely be printed as text.
    printf("[client] sent \"%s\" -> server replied \"%s\"\n", text, reply_body); // Display request and response.
    return 0;                                             // 0 signals that send + receive succeeded.
}                                                         // End of send_and_receive().

int main() {                                               // Program execution starts here.
    int fd = socket(AF_INET, SOCK_STREAM, 0);              // Create an IPv4 TCP socket.
                                                           // AF_INET = IPv4; SOCK_STREAM = TCP; 0 = choose the default protocol for this type.
    if (fd < 0) die("socket()");                          // Negative fd means socket creation failed; print error and exit.

    sockaddr_in server_addr = {};                          // Create and zero-initialize an IPv4 server address structure.
                                                           // sockaddr_in = structure containing IPv4 address + port information.
    server_addr.sin_family = AF_INET;                      // Tell the OS this address uses IPv4.
    server_addr.sin_port = htons(5556);                    // Set server port to 5556; htons() converts host byte order to network byte order.
                                                           // Network byte order = standard byte ordering used when sending multi-byte values over networks.
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // Set server IP to 127.0.0.1 (this same computer).
                                                           // INADDR_LOOPBACK = loopback address; htonl() converts a 32-bit value to network byte order.

    if (connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) { // Establish TCP connection to the server.
                                                                                           // connect() performs the client side of the TCP connection setup.
                                                                                           // reinterpret_cast converts the pointer to the type expected by connect().
                                                                                           // sizeof(server_addr) gives the size of the address structure in bytes.
        die("connect()");                                // If connection fails, print the OS error and terminate.
    }                                                       // End of connection-error check.

    // All three requests below reuse the SAME TCP connection represented by fd.
    send_and_receive(fd, "first message");                 // Send first framed message and wait for its reply.
    send_and_receive(fd, "second message");                // Send second framed message without reconnecting.
    send_and_receive(fd, "third message");                 // Send third framed message without reconnecting.

    close(fd);                                               // Close the socket and release the OS resource.
    return 0;                                                // Return 0 to the operating system: program finished successfully.
}                                                            // End of main().

// KEY TERMS:
// Socket = an OS-managed endpoint used for network communication.
// File descriptor (fd) = integer handle representing an open resource such as a socket.
// TCP = reliable, connection-oriented transport protocol that delivers an ordered byte stream.
// IPv4 = Internet Protocol version 4; addresses are 32 bits, commonly written like 127.0.0.1.
// Port = number used to identify a network service/process on a machine; this program uses 5556.
// Loopback = communication with the same computer; 127.0.0.1 is the common IPv4 loopback address.
// Byte = 8 bits; networking functions such as read/write operate on bytes.
// Buffer = region of memory temporarily holding data being sent or received.
// Header = metadata placed before a message; here the header stores the message length.
// Body = the actual message data after the header.
// Frame = one complete protocol unit; here it is [4-byte length][body].
// Framing = technique for telling the receiver where one message ends and the next begins.
// Byte stream = TCP provides bytes in order, but TCP itself does NOT preserve application message boundaries.
// Blocking = a call waits until it can make progress or fails; read_full() waits for the required bytes.
// Network byte order = standard byte order used in network protocols, normally big-endian.
// Host byte order = byte order used  internally by the current computer's CPU.
// Request = data sent from client to server.
// Response/reply = data sent from server back to client.
