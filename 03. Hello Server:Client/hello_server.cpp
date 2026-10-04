#include <arpa/inet.h>      // IP
#include <cstdio>           // printf, perror
#include <cstdlib>          // exit
#include <cstring>          // C strings
#include <netinet/in.h>     // IPv4
#include <sys/socket.h>     // sockets
#include <unistd.h>         // read, write, close
#include <iostream>         // cout

using namespace std;

// Print error and stop
static void die(const char* what){
    perror(what);
    exit(1);
}

// Handle one client
static void handle_one_client(int conn_fd){
    char request[128] = {};     // Input buffer

    // n = bytes received
    ssize_t n = read(conn_fd, request, sizeof(request) - 1);

    if(n < 0){                  // Read error
        perror("read()");
        return;
    }

    cout << "[server] received: " << request << endl;

    const char reply[] = "greetings from the from-scratch server";

    // Send reply
    write(conn_fd, reply, sizeof(reply) - 1);
}

int main(){
    // IPv4 + TCP
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0) die("socket()");

    // Reuse port quickly
    int opt_val = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

    // Server address
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5555);          // Port 5555
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Any local IP

    // Attach socket to IP + port
    if(::bind(listen_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0){
        die("bind()");
    }

    // Start waiting for clients
    if(listen(listen_fd, SOMAXCONN) < 0) die("listen()");

    cout << "[server] listening on port 5555\n";

    // Keep accepting clients
    while(true){
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);

        // Get a connected socket for one client
        int conn_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if(conn_fd < 0){
            perror("accept()");
            continue;
        }

        handle_one_client(conn_fd);
        close(conn_fd);            // Close client connection
    }

    return 0;
}

/*
SERVER EXPLANATION

1. socket()
   Creates a TCP socket.
   listen_fd = socket used to accept clients.

2. setsockopt()
   SO_REUSEADDR lets the server reuse port 5555 quickly.

3. sockaddr_in
   Stores server IP + port.
   AF_INET  = IPv4
   5555     = port
   INADDR_ANY = accept on any local IP

4. bind()
   Connects listen_fd to IP + port.
   Server is now attached to port 5555.

5. listen()
   Makes listen_fd a listening socket.
   Server now waits for clients.

6. accept()
   Waits for a client.
   When a client connects, it creates conn_fd.

   listen_fd -> accepts clients
   conn_fd   -> talks to one client

7. handle_one_client()
   read()  -> receives client message
   cout    -> prints message
   write() -> sends reply

8. close(conn_fd)
   Closes that client's connection.
   The loop then calls accept() again.

FULL FLOW

Client                         Server
  |                              |
  |                         socket()
  |                            bind()
  |                           listen()
  |                              |
  | -------- connect() --------> |
  |                              | accept()
  |                              |    |
  | -------- message ----------> | read()
  |                              |
  | <--------- reply ----------- | write()
  |                              |
  |                           close()
  |                              |
  |                         next client

IMPORTANT

listen_fd = waiting/accepting socket
conn_fd   = communication socket

Server order:
socket -> setsockopt -> bind -> listen -> accept -> read -> write -> close

Client order:
socket -> connect -> write -> read -> close
*/

