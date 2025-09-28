#include "executor.hpp"
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

struct Reply {
    std::string data;
    std::atomic<bool> ready{false};
};

struct Wakeup {
    Socket reader;
    Socket writer;

    static std::shared_ptr<Wakeup> create() {
        int pair[2];
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "socketpair");
        Socket reader(pair[0]), writer(pair[1]);
        check(fcntl(reader.get(), F_SETFL, O_NONBLOCK), "fcntl wake reader");
        check(fcntl(writer.get(), F_SETFL, O_NONBLOCK), "fcntl wake writer");
        return std::make_shared<Wakeup>(Wakeup{std::move(reader), std::move(writer)});
    }

    void notify() const {
        while (send(writer.get(), ".", 1, 0) < 0 && errno == EINTR) {}
    }

    void clear() const {
        char buffer[256];
        while (recv(reader.get(), buffer, sizeof buffer, 0) > 0) {}
    }
};

struct Client {
    explicit Client(Socket fd) : socket(std::move(fd)) {}

    Socket socket;
    std::string input;
    std::string output;
    std::size_t sent = 0;
    bool closing = false;
    std::shared_ptr<Reply> pending;
    short watched = POLLIN;
    std::chrono::steady_clock::time_point last_progress = std::chrono::steady_clock::now();

    bool receive() {
        char buffer[8192];
        const auto count = recv(socket.get(), buffer, sizeof buffer, 0);
        if (count > 0) {
            input.append(buffer, count);
            last_progress = std::chrono::steady_clock::now();
            return true;
        }
        return count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
    }

    void dispatch(Store& store, Executor& executor, const std::shared_ptr<Wakeup>& wake) {
        if (input.empty()) return;
        std::vector<std::string_view> views;
        std::size_t consumed = 0;
        const auto status = resp::parse(input, views, consumed);
        if (status == resp::Status::incomplete && input.size() <= resp::max_request) return;
        if (status != resp::Status::complete) {
            output = "-ERR protocol error\r\n";
            closing = true;
            return;
        }
        std::vector<std::string> args(views.begin(), views.end());
        input.erase(0, consumed);
        auto reply = std::make_shared<Reply>();
        if (!executor.submit([&store, wake, reply, args = std::move(args)] {
            try {
                const std::vector<std::string_view> views(args.begin(), args.end());
                reply->data = store.execute(views);
            } catch (const std::exception& error) {
                std::cerr << "Command failed: " << error.what() << '\n';
                reply->data = "-ERR command execution failed\r\n";
            }
            reply->ready.store(true, std::memory_order_release);
            wake->notify();
        })) {
            output = "-BUSY command queue is full\r\n";
            closing = true;
        } else {
            pending = std::move(reply);
        }
    }

    bool flush() {
        if (output.empty()) return true;
        const auto count = send(socket.get(), output.data() + sent, output.size() - sent, 0);
        if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        if (count > 0) last_progress = std::chrono::steady_clock::now();
        sent += count;
        if (sent == output.size()) {
            output.clear();
            sent = 0;
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

void serve(int listener, Store& store, Executor& executor, bool maintenance, std::chrono::seconds idle_timeout) {
    Events events;
    const auto wake = Wakeup::create();
    events.watch(listener, POLLIN, true, true);
    events.watch(wake->reader.get(), POLLIN, true);
    std::unordered_map<int, Client> clients;
    std::size_t shard = 0;
    auto sweep_at = std::chrono::steady_clock::now();
    auto advance = [&](Client& client) {
        if (client.output.empty() && !client.pending)
            client.dispatch(store, executor, wake);
        const short interest = client.pending ? 0 : client.output.empty() ? POLLIN : POLLOUT;
        if (interest != client.watched) {
            if (!interest) events.remove(client.socket.get());
            else events.watch(client.socket.get(), interest, !client.watched);
            client.watched = interest;
        }
    };
    while (!stopping.load(std::memory_order_relaxed)) {
        for (const auto& event : events.wait()) {
            if (event.fd == wake->reader.get()) {
                wake->clear();
                continue;
            }
            if (event.fd == listener) {
                Socket client(accept(listener, nullptr, nullptr));
                const int fd = client.get();
                if (fd < 0 || clients.size() == max_clients) continue;
                if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) continue;
                events.watch(fd, POLLIN, true);
                clients.try_emplace(fd, std::move(client));
                continue;
            }

            auto entry = clients.find(event.fd);
            if (entry == clients.end()) continue;
            auto& client = entry->second;
            if ((event.revents & (POLLERR | POLLNVAL)) ||
                (client.watched == POLLIN && !client.receive()) ||
                (client.watched == POLLOUT && !client.flush())) {
                events.remove(event.fd);
                clients.erase(entry);
            } else {
                advance(client);
            }
        }
        const auto time = std::chrono::steady_clock::now();
        for (auto it = clients.begin(); it != clients.end();) {
            auto& client = it->second;
            if (!client.pending && idle_timeout.count() && time - client.last_progress >= idle_timeout) {
                events.remove(it->first);
                it = clients.erase(it);
                continue;
            }
            if (client.pending && client.pending->ready.load(std::memory_order_acquire)) {
                client.output = std::move(client.pending->data);
                client.pending.reset();
                client.last_progress = time;
                advance(client);
            }
            ++it;
        }
        if (maintenance && time >= sweep_at) {
            if (executor.submit([&store, shard] { store.sweep(shard); })) ++shard;
            sweep_at = time + std::chrono::milliseconds(100);
        }
    }
}

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
                    serve(listener.get(), store, executor, i == 0, idle_timeout);
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
