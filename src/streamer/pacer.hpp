// Host-wide send pacer for the raw-RTP transport.
//
// Every monitor pipeline blasts UDP bursts at the same WiFi client; a 4K
// keyframe alone is 100+ back-to-back packets, and N pipelines burst
// independently. The client radio (and any AP queue in between) drops the
// excess, which the NACK/PLI machinery then amplifies into more bursts.
//
// One shared token bucket smooths the *aggregate* video send rate across all
// pipelines. Each RtpServer calls consume() per packet; the call sleeps just
// long enough to keep the total under the configured rate plus headroom.
// Audio and RTCP are tiny and stay unpaced.
//
// The bucket rate follows the sum of the pipelines' (adaptive) encoder
// bitrates, so pacing tightens automatically when the bitrate controller
// backs off.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <thread>

namespace metashare {

class Pacer {
  public:
    // Update one pipeline's contribution to the aggregate rate (kbps of
    // encoder target). Call whenever the bitrate controller adapts.
    void set_stream_rate(int index, int kbps) {
        std::lock_guard<std::mutex> lk(mu_);
        stream_kbps_[index] = kbps;
        int total = 0;
        for (const auto& [i, k] : stream_kbps_) total += k;
        // 25% headroom over the nominal encoder rate: RTP/UDP/IP overhead,
        // encoder VBV overshoot, and retransmissions.
        rate_bytes_per_sec_ = static_cast<double>(total) * 1000.0 / 8.0 * 1.25;
        // Burst budget: ~10 ms worth, floor 32 KB so small rates still allow
        // one MTU-sized batch through without sleeping.
        burst_bytes_ = std::max(32.0 * 1024.0, rate_bytes_per_sec_ / 100.0);
        tokens_ = std::min(tokens_, burst_bytes_);
    }

    // Block (sleeping in small slices) until len bytes fit under the rate.
    // With no configured rate this is a no-op.
    void consume(std::size_t len) {
        using clock = std::chrono::steady_clock;
        std::unique_lock<std::mutex> lk(mu_);
        if (rate_bytes_per_sec_ <= 0) return;
        for (;;) {
            const auto now = clock::now();
            const double dt =
                std::chrono::duration<double>(now - last_refill_).count();
            last_refill_ = now;
            tokens_ =
                std::min(burst_bytes_, tokens_ + dt * rate_bytes_per_sec_);
            if (tokens_ >= static_cast<double>(len)) {
                tokens_ -= static_cast<double>(len);
                return;
            }
            const double need =
                (static_cast<double>(len) - tokens_) / rate_bytes_per_sec_;
            lk.unlock();
            // Sleep in <=2 ms slices so a rate increase takes effect quickly.
            std::this_thread::sleep_for(
                std::chrono::duration<double>(std::min(need, 0.002)));
            lk.lock();
            if (rate_bytes_per_sec_ <= 0) return;
        }
    }

  private:
    std::mutex mu_;
    std::map<int, int> stream_kbps_;
    double rate_bytes_per_sec_ = 0;
    double burst_bytes_ = 32.0 * 1024.0;
    double tokens_ = 32.0 * 1024.0;
    std::chrono::steady_clock::time_point last_refill_ =
        std::chrono::steady_clock::now();
};

}  // namespace metashare
