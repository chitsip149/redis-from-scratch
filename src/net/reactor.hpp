#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

class Executor;
class Store;

void serve(int listener, Store& store, Executor& executor, bool maintenance,
           std::chrono::seconds idle_timeout, std::size_t max_clients,
           const std::atomic<bool>& stopping);
