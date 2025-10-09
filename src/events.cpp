#include "events.hpp"

#ifdef __linux__
#include <sys/epoll.h>
#endif

    Events::Events() {
#ifdef __linux__
        check(epoll_.get(), "epoll_create1");
#endif
    }

    void Events::watch(int fd, short interest, bool add, bool listener) {
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

    void Events::remove(int fd) {
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

    std::vector<pollfd> Events::wait() {
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
