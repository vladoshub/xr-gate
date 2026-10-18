#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace xr_basalt {
// Feed IMU independently of the image producer. A large IMU window must not
// block the producer before it can deliver the image the estimator awaits.
// Image queue backpressure bounds the number of outstanding batches.
template <typename Queue, typename Value>
class InputFeeder {
 public:
  InputFeeder(Queue& queue, std::function<bool()> consumer_finished)
      : queue_(queue), consumer_finished_(std::move(consumer_finished)),
        thread_([this] { run(); }) {}
  ~InputFeeder() { finish(); join(); }
  InputFeeder(const InputFeeder&) = delete;
  InputFeeder& operator=(const InputFeeder&) = delete;

  void submit(std::vector<Value> batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    batches_.push_back(std::move(batch));
    ready_.notify_one();
  }
  void finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    ready_.notify_one();
  }
  void join() { if (thread_.joinable()) thread_.join(); }

 private:
  bool send(const Value& value) {
    while (!queue_.try_push(value)) {
      if (consumer_finished_()) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  void run() {
    for (;;) {
      std::vector<Value> batch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closing_ || !batches_.empty(); });
        if (batches_.empty()) break;
        batch = std::move(batches_.front());
        batches_.pop_front();
      }
      for (const auto& sample : batch) if (!send(sample)) return;
    }
    send(Value{});  // End of IMU input also unblocks an estimator waiting for IMU.
  }
  Queue& queue_;
  std::function<bool()> consumer_finished_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::vector<Value>> batches_;
  bool closing_ = false;
  std::thread thread_;
};
}  // namespace xr_basalt
