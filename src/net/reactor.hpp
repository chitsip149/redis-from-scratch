#pragma once

#include <atomic>
#include <chrono>

class Executor;
class Store;

void serve(int listener, Store& store, Executor& executor, bool maintenance,
           std::chrono::seconds idle_timeout, const std::atomic<bool>& stopping);
