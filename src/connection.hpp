#pragma once

#include "socket.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <poll.h>
#include <string>
#include <utility>

class Executor;
class Store;

struct Reply {
    std::string data;
    std::atomic<bool> ready{false};
};

struct Wakeup {
    Socket reader;
    Socket writer;

    static std::shared_ptr<Wakeup> create();

    void notify() const;

    void clear() const;
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

    bool receive();

    void dispatch(Store& store, Executor& executor, const std::shared_ptr<Wakeup>& wake);

    bool flush();
};
