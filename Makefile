CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic

redis-server: src/server.cpp src/store.cpp $(wildcard src/*.hpp)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -pthread src/server.cpp src/store.cpp $(LDFLAGS) -o $@

.PHONY: clean
clean:
	rm -f redis-server
