#include "store.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif

namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};
constexpr std::size_t max_output = 2 * resp::max_request;
constexpr std::size_t max_clients = 256;

void stop(int) { stopping.store(true, std::memory_order_relaxed); }

class Socket {
    int fd_;

public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { if (fd_ >= 0) close(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    int get() const { return fd_; }
};

void check(int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}

struct Client {
    Socket socket;
    std::string input;
    std::string output;
    std::size_t sent = 0;
    bool closing = false;

    bool update(Store& store, short ready) {
        if (ready & (POLLERR | POLLNVAL)) return false;
        if (!closing && output.empty() && (ready & (POLLIN | POLLHUP))) {
            char buffer[8192];
            const auto count = recv(socket.get(), buffer, sizeof buffer, 0);
            if (count == 0) return false;
            if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
            input.append(buffer, count);
            std::size_t cursor = 0;
            while (cursor < input.size()) {
                std::vector<std::string_view> args;
                std::size_t consumed = 0;
                const auto status = resp::parse(std::string_view(input).substr(cursor), args, consumed);
                if (status == resp::Status::incomplete && input.size() - cursor <= resp::max_request)
                    break;
                if (status != resp::Status::complete) {
                    output += "-ERR protocol error\r\n";
                    closing = true;
                    break;
                }
                auto response = store.execute(args);
                if (response.size() > max_output - output.size()) return false;
                output += response;
                cursor += consumed;
            }
            input.erase(0, cursor);
        }
        if (!output.empty()) {
            const auto count = send(socket.get(), output.data() + sent, output.size() - sent, 0);
            if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
            sent += count;
            if (sent == output.size()) {
                output.clear();
                sent = 0;
            }
        }
        return !closing || !output.empty();
    }
};

class Events {
#ifdef __linux__
    Socket epoll_{epoll_create1(EPOLL_CLOEXEC)};
#else
    std::vector<pollfd> watched_;
#endif

public:
    Events() {
#ifdef __linux__
        check(epoll_.get(), "epoll_create1");
#endif
    }

    void watch(int fd, short interest, bool add, bool listener = false) {
#ifdef __linux__
        epoll_event event{};
        event.data.fd = fd;
        event.events = interest == POLLIN ? EPOLLIN : EPOLLOUT;
        if (listener) event.events |= EPOLLEXCLUSIVE;
        check(epoll_ctl(epoll_.get(), add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &event), "epoll_ctl");
#else
        (void)listener;
        if (add) watched_.push_back({fd, interest, 0});
        else for (auto& entry : watched_) if (entry.fd == fd) entry.events = interest;
#endif
    }

    void remove(int fd) {
#ifdef __linux__
        check(epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr), "epoll_ctl delete");
#else
        for (auto it = watched_.begin(); it != watched_.end(); ++it) {
            if (it->fd == fd) {
                watched_.erase(it);
                break;
            }
        }
#endif
    }

    std::vector<pollfd> wait() {
        std::vector<pollfd> ready;
#ifdef __linux__
        epoll_event events[64];
        const int count = epoll_wait(epoll_.get(), events, 64, 100);
        if (count < 0 && errno != EINTR) check(count, "epoll_wait");
        for (int i = 0; i < count; ++i) {
            short flags = 0;
            if (events[i].events & EPOLLIN) flags |= POLLIN;
            if (events[i].events & EPOLLOUT) flags |= POLLOUT;
            if (events[i].events & EPOLLHUP) flags |= POLLHUP;
            if (events[i].events & EPOLLERR) flags |= POLLERR;
            ready.push_back({events[i].data.fd, 0, flags});
        }
#else
        const int count = poll(watched_.data(), watched_.size(), 100);
        if (count < 0 && errno != EINTR) check(count, "poll");
        if (count > 0) for (const auto& entry : watched_) if (entry.revents) ready.push_back(entry);
#endif
        return ready;
    }
};

void serve(int listener, Store& store) {
    Events events;
    events.watch(listener, POLLIN, true, true);
    std::unordered_map<int, Client> clients;
    while (!stopping.load(std::memory_order_relaxed)) {
        for (const auto& event : events.wait()) {
            if (event.fd == listener) {
                Socket client(accept(listener, nullptr, nullptr));
                const int fd = client.get();
                if (fd < 0 || clients.size() == max_clients) continue;
                if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) continue;
                events.watch(fd, POLLIN, true);
                clients.emplace(fd, Client{std::move(client), {}, {}});
                continue;
            }

            auto entry = clients.find(event.fd);
            if (entry == clients.end()) continue;
            auto& client = entry->second;
            const bool was_writing = !client.output.empty();
            if (!client.update(store, event.revents)) {
                events.remove(event.fd);
                clients.erase(entry);
            } else if (was_writing != !client.output.empty()) {
                events.watch(event.fd, client.output.empty() ? POLLIN : POLLOUT, false);
            }
        }
    }
}

int argument(const char* text, int max) {
    long long value = 0;
    if (!resp::integer(text, value) || value < 1 || value > max)
        throw std::invalid_argument("invalid port or worker count");
    return static_cast<int>(value);
}
}

int main(int argc, char** argv) try {
    if (argc > 3) throw std::invalid_argument("usage: redis-server [port] [workers]");
    const int port = argc > 1 ? argument(argv[1], 65535) : 6379;
    const int workers = argc > 2 ? argument(argv[2], 64) : 4;
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

    Store store;
    std::vector<std::thread> pool;
    std::mutex error_mutex;
    std::exception_ptr failure;
    try {
        for (int i = 0; i < workers; ++i) {
            pool.emplace_back([&] {
                try {
                    serve(listener.get(), store);
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
    if (failure) std::rethrow_exception(failure);
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
}
