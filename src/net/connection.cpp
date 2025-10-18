#include "connection.hpp"

#include "app/executor.hpp"
#include "storage/store.hpp"

#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>

std::shared_ptr<Wakeup> Wakeup::create() {
    int pair[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), "socketpair");
    Socket reader(pair[0]), writer(pair[1]);
    check(fcntl(reader.get(), F_SETFL, O_NONBLOCK), "fcntl wake reader");
    check(fcntl(writer.get(), F_SETFL, O_NONBLOCK), "fcntl wake writer");
    return std::make_shared<Wakeup>(Wakeup{std::move(reader), std::move(writer)});
}

void Wakeup::notify() const {
    while (send(writer.get(), ".", 1, 0) < 0 && errno == EINTR) {}
}

void Wakeup::clear() const {
    char buffer[256];
    while (recv(reader.get(), buffer, sizeof buffer, 0) > 0) {}
}

bool Client::receive() {
    char buffer[8192];
    const auto count = recv(socket.get(), buffer, sizeof buffer, 0);
    if (count > 0) {
        input.append(buffer, count);
        last_progress = std::chrono::steady_clock::now();
        return true;
    }
    return count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK);
}

void Client::dispatch(Store& store, Executor& executor, const std::shared_ptr<Wakeup>& wake) {
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

bool Client::flush() {
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
