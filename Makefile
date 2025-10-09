CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
HEADERS := $(wildcard src/*.hpp)
STORE_SOURCES := src/store.cpp src/commands.cpp src/snapshot.cpp
SERVER_SOURCES := src/server.cpp src/reactor.cpp src/connection.cpp src/events.cpp $(STORE_SOURCES)

redis-server: $(SERVER_SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -pthread $(SERVER_SOURCES) $(LDFLAGS) -o $@

redis-tests: tests/regression.cpp $(STORE_SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -std=c++17 -pthread -Isrc tests/regression.cpp $(STORE_SOURCES) $(LDFLAGS) -o $@

.PHONY: clean test
test: redis-tests
	./redis-tests

clean:
	rm -f redis-server redis-tests
