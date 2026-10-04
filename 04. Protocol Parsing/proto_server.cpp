#include <arpa/inet.h> // for inet_pton and htons functions
#include <cstdint> // for fixed width integer types like uint32_t
#include <cstdio> // for standard input/output functions like printf and fprintf
#include <cstdlib> // for general utilities like exit
#include <cstring> // for memory functions like memcpy and strlen
#include <netinet/in.h> // for sockaddr_in structure and constants
#include <sys/socket.h> // for socket functions and definitions
#include <unistd.h> // for POSIX API like read, write, and close

static const size_t k_max_msg=4096; // maximum allowed message size

static void die(const char* what){
    perror(what); // print error message related to last system call failure
    exit(1); // exit program with error status
};

static int read_full(int fd, char* buf, size_t n) {
    while (n > 0) { // loop until all requested bytes are read
        ssize_t got = read(fd, buf, n); // read from fd into buf up to n bytes
        if (got <= 0) {
            return -1;  // error or peer closed connection mid-message
        }
        buf += got; // move buffer pointer forward by number of bytes read
        n -= (size_t)got; // reduce number of bytes left to read
    }
    return 0; // success, all bytes read
}

static int write_all(int fd,const char* buf , size_t n){
    while(n>0){ // loop until all bytes are written
        ssize_t sent =write(fd,buf,n); // write to fd from buf up to n bytes
        if(sent<=0){
            return -1; // error writing
        }
        buf +=sent; // move buffer pointer forward by bytes written
        n-=(size_t)sent; // reduce bytes left to write
    }
    return 0; // success, all bytes written
}

static int handle_one_message(int conn_fd){
    char header[4]; // buffer to read message length header
    if(read_full(conn_fd,header,4)<0){
        return -1; // error reading header
    }

    uint32_t body_len=0; // variable to store message length (typo in name)
    memcpy(&body_len,header,4); // copy 4 bytes from header into body_len
    if(body_len>k_max_msg){
        fprintf(stderr,"[server] message too large: %u bytes\n", body_len);
        return -1;
    }

    char body[k_max_msg+1]; // buffer for message body plus null terminator
    if(read_full(conn_fd,body,body_len)<0){
        fprintf(stderr,"[server] connection dropped mid-msg\n");
        return -1;
    }

    body[body_len]='\0'; // add null terminator to message body string
    printf("server client said: %s\n", body); // print received message

    const char reply_text[]="msg received"; // reply message text
    uint32_t reply_len=(uint32_t)strlen(reply_text); // length of reply message

    char out_buf[4+sizeof(reply_text)]; // buffer for reply length + reply message
    memcpy(out_buf, &reply_len,4); // copy reply length into first 4 bytes of buffer
    memcpy(out_buf+4,reply_text,reply_len); // copy reply text after length header

    return write_all(conn_fd,out_buf,4+reply_len); // send reply message back to client

}


int main(){
    int listen_fd=socket(AF_INET,SOCK_STREAM,0); // create TCP socket
    if (listen_fd < 0) die("socket()"); // error if socket creation failed

    int opt_val=1; // value to enable socket option
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val)); // allow reuse of local addresses

    sockaddr_in server_addr={};

    server_addr.sin_family=AF_INET;
    server_addr.sin_port=htons(5556);

    server_addr.sin_addr.s_addr=htonl(INADDR_ANY);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&server_addr),sizeof(server_addr))<0){
        die("bind()");
    }

    if (listen(listen_fd,SOMAXCONN)<0)die("listen()");

    printf("[server] listening on port 5556\n");

    while(true){
        sockaddr_in client_addr={};
        socklen_t client_len=sizeof(client_addr);

        int conn_fd =accept(listen_fd,reinterpret_cast<sockaddr*>(&client_addr),&client_len);

        if(conn_fd<0){
            perror("accept()");
            continue;
        }

        while(handle_one_message(conn_fd)==0){

        }
        close(conn_fd);
    }
    return 0;
}