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