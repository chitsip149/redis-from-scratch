#pragma once

#include "socket.hpp"

#include <poll.h>
#include <vector>

class Events {
#ifdef __linux__
    Socket epoll_;
#else
    std::vector<pollfd> watched_;
#endif

public:
    Events();

    void watch(int fd, short interest, bool add, bool listener = false);

    void remove(int fd);

    std::vector<pollfd> wait();
};
