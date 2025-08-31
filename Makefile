CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic

redis-server: src/server.cpp src/resp.hpp src/store.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -pthread src/server.cpp $(LDFLAGS) -o $@

.PHONY: clean
clean:
	rm -f redis-server
