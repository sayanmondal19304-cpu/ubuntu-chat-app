CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread -Iinclude -Idriver

# user-space programs (no root, no kernel headers needed)
all: bin/server bin/client bin/chatlog_tool

bin/server: src/server.cpp include/common.hpp driver/chatlog_ioctl.h | bin
	$(CXX) $(CXXFLAGS) $< -o $@

bin/client: src/client.cpp include/common.hpp | bin
	$(CXX) $(CXXFLAGS) $< -o $@

bin/chatlog_tool: tools/chatlog_tool.cpp driver/chatlog_ioctl.h | bin
	$(CXX) $(CXXFLAGS) $< -o $@

bin:
	mkdir -p bin

# kernel module (needs: sudo apt install linux-headers-$(uname -r))
driver:
	$(MAKE) -C driver

clean:
	rm -rf bin
	-$(MAKE) -C driver clean

.PHONY: all driver clean
