#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <vector>

namespace twinvq::detail {
// Synchronous batches keep all captured encoder data alive until workers join
// the batch. Copying an Encoder may share this pool; serialize those callers.
class VqWorkers {
public:
    explicit VqWorkers(int background_count) {
        if (background_count < 0) throw std::invalid_argument("negative VQ worker count");
        try {
            workers_.reserve(background_count);
            for (int id = 1; id <= background_count; ++id)
                workers_.emplace_back([this, id] { work(id); });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~VqWorkers() { stop(); }
    VqWorkers(const VqWorkers&) = delete;
    VqWorkers& operator=(const VqWorkers&) = delete;

    int capacity() const { return static_cast<int>(workers_.size()) + 1; }

    void run(int count, int vectors, std::function<void(int, int)> task) {
        if (count < 1 || count > static_cast<int>(workers_.size()) + 1 || vectors < 0)
            throw std::invalid_argument("invalid VQ batch size");
        std::lock_guard<std::mutex> batch(batch_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::move(task);
            count_ = count;
            vectors_ = vectors;
            pending_ = count - 1;
            failure_ = nullptr;
            ++generation_;
        }
        ready_.notify_all();
        try { task_(0, vectors / count); }
        catch (...) { record_failure(); }
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&] { return pending_ == 0; });
        task_ = {};
        const auto failure = failure_;
        lock.unlock();
        if (failure) std::rethrow_exception(failure);
    }

private:
    void record_failure() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failure_) failure_ = std::current_exception();
    }
    void work(int id) {
        size_t seen = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            ready_.wait(lock, [&] { return stopping_ || generation_ != seen; });
            if (stopping_) return;
            seen = generation_;
            if (id >= count_) continue;
            const int begin = vectors_ * id / count_;
            const int end = vectors_ * (id + 1) / count_;
            lock.unlock();
            try { task_(begin, end); }
            catch (...) { record_failure(); }
            lock.lock();
            if (--pending_ == 0) finished_.notify_one();
        }
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_all();
        for (auto& worker : workers_) worker.join();
    }
    std::mutex batch_mutex_, mutex_;
    std::condition_variable ready_, finished_;
    std::vector<std::thread> workers_;
    std::function<void(int, int)> task_;
    std::exception_ptr failure_;
    size_t generation_ = 0;
    int count_ = 0, vectors_ = 0, pending_ = 0;
    bool stopping_ = false;
};
} // namespace twinvq::detail
