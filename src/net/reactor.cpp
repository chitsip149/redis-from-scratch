#include "reactor.hpp"

#include "connection.hpp"
#include "events.hpp"
#include "app/executor.hpp"
#include "storage/store.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unordered_map>

namespace {
constexpr std::size_t max_clients = 256;
}

void serve(int listener, Store& store, Executor& executor, bool maintenance,
           std::chrono::seconds idle_timeout, const std::atomic<bool>& stopping) {
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
