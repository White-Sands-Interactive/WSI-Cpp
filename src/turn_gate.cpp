#include "turn_gate.hpp"

#include <chrono>
#include <utility>

namespace wsi {

struct TurnGate::Impl {
    std::mutex mu;
    std::condition_variable cv;
    bool released = false;
    bool stopped = false;
    bool turn_active = false;
    bool turn_done = false;
    std::exception_ptr turn_error;
    TurnReport turn_report;
};

TurnGate::TurnGate() : impl_(std::make_shared<Impl>()) {}

void TurnGate::wait_for_turn() {
    auto impl = impl_;
    std::unique_lock lock(impl->mu);
    if (impl->stopped) throw std::runtime_error("session stopped");
    if (impl->released) {
        impl->released = false;
        return;
    }
    impl->cv.wait(lock, [&] { return impl->released || impl->stopped; });
    if (impl->stopped) throw std::runtime_error("session stopped");
    impl->released = false;
}

TurnReport TurnGate::run_turn(std::optional<int> timeout_ms, AbortSignal* signal) {
    auto impl = impl_;
    std::unique_lock lock(impl->mu);
    if (impl->turn_active) throw std::runtime_error("turn() already in progress for this session");
    if (signal != nullptr && signal->aborted()) throw std::runtime_error(signal->reason());

    impl->turn_active = true;
    impl->turn_done = false;
    impl->turn_error = nullptr;
    impl->released = true;
    impl->cv.notify_all();

    auto aborted = std::make_shared<std::atomic<bool>>(false);
    Subscription subscription;
    if (signal != nullptr) {
        std::weak_ptr<Impl> weak = impl;
        subscription = signal->on_abort([weak, aborted] {
            aborted->store(true);
            if (auto locked = weak.lock()) locked->cv.notify_all();
        });
        if (signal->aborted()) aborted->store(true);
    }

    const auto ready = [&] { return impl->turn_done || aborted->load(); };
    if (timeout_ms.has_value()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*timeout_ms);
        if (!impl->cv.wait_until(lock, deadline, ready)) {
            impl->turn_active = false;
            throw TurnTimeoutError("turn() timed out after " + std::to_string(*timeout_ms) + "ms");
        }
    } else {
        impl->cv.wait(lock, ready);
    }

    impl->turn_active = false;
    if (!impl->turn_done && aborted->load()) {
        const std::string message = signal != nullptr && !signal->reason().empty() ? signal->reason() : "turn aborted";
        throw std::runtime_error(message);
    }
    if (impl->turn_error) std::rethrow_exception(impl->turn_error);
    return impl->turn_report;
}

void TurnGate::complete_turn(const TurnReport& report) {
    auto impl = impl_;
    std::lock_guard lock(impl->mu);
    if (!impl->turn_active || impl->turn_done) return;
    impl->turn_report = report;
    impl->turn_done = true;
    impl->cv.notify_all();
}

void TurnGate::fail_turn(std::exception_ptr error) {
    auto impl = impl_;
    std::lock_guard lock(impl->mu);
    if (!impl->turn_active || impl->turn_done) return;
    impl->turn_error = std::move(error);
    impl->turn_done = true;
    impl->cv.notify_all();
}

void TurnGate::reset() {
    auto impl = impl_;
    std::lock_guard lock(impl->mu);
    impl->stopped = true;
    impl->released = true;
    if (impl->turn_active && !impl->turn_done) {
        impl->turn_report = TurnReport{};
        impl->turn_report.ok = false;
        impl->turn_report.error = "session stopped";
        impl->turn_done = true;
    }
    impl->cv.notify_all();
}

bool TurnGate::stopped() const {
    auto impl = impl_;
    std::lock_guard lock(impl->mu);
    return impl->stopped;
}

}  // namespace wsi
