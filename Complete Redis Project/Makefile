CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
THREAD_FLAGS = -pthread

SERVER_SOURCES = kv_server.cpp zset.cpp avl.cpp hashtable.cpp heap.cpp thread_pool.cpp

.PHONY: all clean test unit-tests

all: kv_server kv_client

kv_server: $(SERVER_SOURCES) common.h hashtable.h heap.h list.h thread_pool.h zset.h avl.h
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -o $@ $(SERVER_SOURCES)

kv_client: kv_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: all unit-tests
	@echo "Start ./kv_server in another terminal, then run: python3 test_cmds.py"

unit-tests: test_thread_pool test_async_free
	./test_thread_pool
	./test_async_free

test_thread_pool: test_thread_pool.cpp thread_pool.cpp thread_pool.h
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -o $@ test_thread_pool.cpp thread_pool.cpp

test_async_free: test_async_free.cpp zset.cpp avl.cpp hashtable.cpp thread_pool.cpp
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -o $@ test_async_free.cpp zset.cpp avl.cpp hashtable.cpp thread_pool.cpp

clean:
	rm -f kv_server kv_client test_thread_pool test_async_free
