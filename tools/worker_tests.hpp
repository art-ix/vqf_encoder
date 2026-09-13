#pragma once
#include "twinvq_workers.hpp"
#include "twinvq/twinvq_encoder.hpp"
#include <future>

int test_workers() {
    // Vary active worker count and exercise uneven and empty partitions.
    twinvq::detail::VqWorkers pool(3);
    std::vector<int> visits(37, 0);
    auto task = [&](int begin, int end) { for (int i = begin; i < end; ++i) ++visits[i]; };
    for (int count : {4, 2, 3, 1, 4}) pool.run(count, 37, task);
    // Shared pools (e.g. copied Encoders) serialize concurrent batch callers.
    auto other = std::async(std::launch::async, [&] { pool.run(4, 37, task); });
    pool.run(2, 37, task);
    other.get();
    for (int visits_at_bin : visits)
        if (visits_at_bin != 7) throw std::runtime_error("worker range skipped or overlapped");
    pool.run(4, 0, task);
    for (bool caller : {true, false}) {
        bool caught = false;
        try {
            pool.run(4, 37, [&](int begin, int) {
                if ((begin == 0) == caller) throw std::runtime_error("injected worker failure");
            });
        } catch (const std::runtime_error&) { caught = true; }
        if (!caught) throw std::runtime_error("lost worker exception");
        pool.run(4, 37, task); // A failed batch must not poison later work.
    }
    for (int visits_at_bin : visits)
        if (visits_at_bin != 9) throw std::runtime_error("pool failed after an exception");
    bool rejected = false;
    try { pool.run(5, 37, task); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("invalid worker batch accepted");
    // Dynamic chunks must cover each vector exactly once, including tails,
    // more workers than chunks, a single worker, and concurrent batch callers.
    std::vector<std::atomic<int>> dynamic_visits(37);
    for (auto& value : dynamic_visits) value.store(0);
    auto dynamic_task = [&](int begin, int end) {
        if (begin < 0 || end > 37 || begin >= end)
            throw std::runtime_error("invalid dynamic range");
        for (int i = begin; i < end; ++i) dynamic_visits[i].fetch_add(1);
    };
    int batches = 0;
    for (int grain : {1, 8, 64}) for (int count : {4, 2, 1}) {
        pool.run(count, 37, dynamic_task, grain);
        ++batches;
    }
    auto dynamic_other = std::async(std::launch::async, [&] { pool.run(4, 37, dynamic_task, 8); });
    pool.run(2, 37, dynamic_task, 1);
    dynamic_other.get();
    batches += 2;
    pool.run(4, 0, [](int, int) { throw std::runtime_error("empty dynamic task ran"); }, 8);
    bool dynamic_caught = false;
    try {
        pool.run(4, 37, [](int, int) { throw std::runtime_error("dynamic failure"); }, 8);
    } catch (const std::runtime_error&) { dynamic_caught = true; }
    if (!dynamic_caught) throw std::runtime_error("lost dynamic worker exception");
    pool.run(4, 37, dynamic_task, 8);
    ++batches;
    for (const auto& value : dynamic_visits)
        if (value.load() != batches) throw std::runtime_error("dynamic range skipped or overlapped");
    rejected = false;
    try { pool.run(4, 37, dynamic_task, -1); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("negative dynamic grain accepted");
    // Copy after a parallel frame; flushing one owner must not invalidate the
    // shared workers needed to finish the other copy of the stream.
    twinvq::Encoder::Config config;
    config.threads = 4;
    config.ppc_search = true;
    twinvq::Encoder original(config);
    const int n = original.frame_samples();
    std::vector<float> pcm(2 * n, 0.125f);
    original.feed(pcm.data(), n);
    auto copied = original;
    original.feed(pcm.data(), n); original.flush();
    copied.feed(pcm.data(), n); copied.flush();
    if (original.data() != copied.data()) throw std::runtime_error("copied Encoder lost worker state");
    std::cout << "VQ worker lifecycle, ranges and exceptions passed\n";
    return 0;
}
