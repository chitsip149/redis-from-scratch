#include "executor.hpp"
#include "reactor.hpp"
#include "socket.hpp"
#include "store.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>

namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};

void stop(int) { stopping.store(true, std::memory_order_relaxed); }

int argument(const char* text, int max, int min = 1) {
    long long value = 0;
    if (!resp::integer(text, value) || value < min || value > max)
        throw std::invalid_argument("invalid numeric server argument");
    return static_cast<int>(value);
}
}

int main(int argc, char** argv) try {
    if (argc > 6)
        throw std::invalid_argument("usage: redis-server [port] [workers] [snapshot|-] [keys-per-shard] [idle-seconds]");
    const int port = argc > 1 ? argument(argv[1], 65535) : 6379;
    const int workers = argc > 2 ? argument(argv[2], 64) : 4;
    const auto idle_timeout = std::chrono::seconds(argc > 5 ? argument(argv[5], 86400, 0) : 0);
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    Socket listener(socket(AF_INET, SOCK_STREAM, 0));
    check(listener.get(), "socket");
    const int reuse = 1;
    check(setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse), "setsockopt");
    check(fcntl(listener.get(), F_SETFL, O_NONBLOCK), "fcntl");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    check(bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof address), "bind");
    check(listen(listener.get(), SOMAXCONN), "listen");

    Store store(argc > 3 && std::string_view(argv[3]) != "-" ? argv[3] : "",
                argc > 4 ? argument(argv[4], 10000000, 0) : 0);
    Executor executor(workers);
    std::vector<std::thread> pool;
    std::mutex error_mutex;
    std::exception_ptr failure;
    try {
        for (int i = 0; i < workers; ++i) {
            pool.emplace_back([&, i] {
                try {
                    serve(listener.get(), store, executor, i == 0, idle_timeout, stopping);
                } catch (...) {
                    std::lock_guard lock(error_mutex);
                    if (!failure) failure = std::current_exception();
                    stop(SIGTERM);
                }
            });
        }
    } catch (...) {
        stop(SIGTERM);
        for (auto& worker : pool) worker.join();
        throw;
    }
    for (auto& worker : pool) worker.join();
    executor.drain();
    if (failure) std::rethrow_exception(failure);
    if (argc > 3 && std::string_view(argv[3]) != "-") store.save();
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
