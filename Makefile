CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic

redis-server: src/server.cpp src/store.cpp $(wildcard src/*.hpp)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -pthread src/server.cpp src/store.cpp $(LDFLAGS) -o $@

redis-tests: tests/regression.cpp src/store.cpp $(wildcard src/*.hpp)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG -std=c++17 -pthread -Isrc tests/regression.cpp src/store.cpp $(LDFLAGS) -o $@

.PHONY: clean test
test: redis-tests
	./redis-tests

clean:
	rm -f redis-server redis-tests
