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
