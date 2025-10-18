CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
HEADERS := $(wildcard src/*/*.hpp)
STORE_SOURCES := src/storage/store.cpp src/storage/commands.cpp src/storage/snapshot.cpp
SERVER_SOURCES := src/app/server.cpp src/net/reactor.cpp src/net/connection.cpp src/net/events.cpp $(STORE_SOURCES)

redis-server: $(SERVER_SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -pthread -Isrc $(SERVER_SOURCES) $(LDFLAGS) -o $@

redis-tests: tests/regression.cpp $(STORE_SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -std=c++17 -pthread -Isrc tests/regression.cpp $(STORE_SOURCES) $(LDFLAGS) -o $@

.PHONY: clean test
test: redis-tests
	./redis-tests

clean:
	rm -f redis-server redis-tests
