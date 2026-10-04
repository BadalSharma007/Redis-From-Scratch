#include <arpa/inet.h>      // IP address functions
#include <cstdio>           // printf(), perror()
#include <cstdlib>          // exit()
#include <cstring>          // C string functions
#include <netinet/in.h>     // sockaddr_in, IPv4 address structure
#include <sys/socket.h>     // socket(), connect()
#include <unistd.h>         // read(), write(), close()
#include <iostream>         // cout

using namespace std;

// Helper: print the error + stop the program
static void die(const char* what){
    perror(what);           // Print the system error message
    exit(1);                // 1 = program ended because of an error
}

int main(){
    // fd = file descriptor: integer used to identify our socket
    // AF_INET = IPv4, SOCK_STREAM = TCP, 0 = default TCP protocol
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    // socket() returns -1 if creation fails
    if(fd < 0) die("socket()");

    // Store the server's IPv4 address + port
    sockaddr_in server_addr = {};

    // Use IPv4
    server_addr.sin_family = AF_INET;

    // Server port = 5555; htons() converts port to network byte order
    server_addr.sin_port = htons(5555);

    // INADDR_LOOPBACK = 127.0.0.1 = this same computer
    // htonl() converts the address to network byte order
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // connect() asks the server to establish a TCP connection
    // reinterpret_cast converts server_addr to the generic socket address type
    if(connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0){
        die("connect()");     // Connection failed
    }

    // Message that the client will send to the server
    const char message[] = "Hello from the from-scratch client";

    // write() sends bytes through the socket
    // sizeof(message)-1 excludes the ending '\0' of the C string
    write(fd, message, sizeof(message) - 1);

    // Buffer = temporary memory used to store the server's reply
    char reply[128] = {};

    // read() waits for data from the server and stores it in reply
    // n = number of bytes actually received
    ssize_t n = read(fd, reply, sizeof(reply) - 1);

    // read() returns a negative value when an error occurs
    if(n < 0) die("read()");

    // Print the server's reply
    cout << "[Client] server replied: " << reply << '\n';

    // Close the socket and release its resource
    close(fd);

    // 0 = program finished successfully
    return 0;

    // CLIENT SIDE: connects to a TCP server, sends a message,
    // receives the server's reply, prints it, then closes the connection.
}


/*
 // CLIENT SIDE: connects to a TCP server, sends a message,
    // receives the server's reply, prints it, then closes the connection.
IP:   127.0.0.1
Port: 5555

Client
  │
  │ 1. Create TCP socket
  ↓
  │
  │ 2. Connect to 127.0.0.1:5555
  ↓
Server
  │
  │ 3. Send "Hello from the from-scratch client"
  ↓
Server
  │
  │ 4. Server sends a reply
  ↓
Client
  │
  │ 5. Read and print reply
  ↓
  │ 6. Close connection





                   TCP CONNECTION
        ┌─────────────────────────────┐
        │                             │
        ▼                             │
┌──────────────┐                 ┌──────────────┐
│    CLIENT    │                 │    SERVER    │
│ HelloClient  │                 │  Port 5555  │
└──────┬───────┘                 └──────▲───────┘
       │                                │
       │ 1. socket()                    │
       │    Create TCP socket           │
       │                                │
       │ 2. connect()                   │
       │ ─────────────────────────────> │
       │    127.0.0.1:5555              │
       │                                │
       │ 3. write()                     │
       │ ─── "Hello from client" ─────> │
       │                                │
       │ 4. read()                      │
       │ <────── Server's reply ─────── │
       │                                │
       │ 5. cout                        │
       │    Print reply                 │
       │                                │
       │ 6. close()                     │
       │ ───────── Close connection ──> │
       │                                │


*/