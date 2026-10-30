#include "wsi/agent_player.hpp"
#include "wsi/testing.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

constexpr int kDoor = 3;

class Courier : public wsi::Player {
 public:
    explicit Courier(int x = 0) : x_(x) {
        set_description("A courier carrying one parcel along a street");
        bind_see(&Courier::see);
        bind(&Courier::walk, "walk", "Move dx steps. Positive is toward the door.", wsi::args(wsi::integer("dx")));
        bind(&Courier::deliver, "deliver", "Hand over the parcel. Only works at x = 3.");
    }

    WSI_TYPE_NAME(Courier)

    int x() const { return x_; }
    bool delivered() const { return delivered_; }

    wsi::Json see() { return wsi::Json{{"x", x_}, {"door", kDoor}, {"carrying", !delivered_}}; }

    wsi::Json walk(int dx) {
        const int next = x_ + dx;
        if (next < 0 || next > kDoor) {
            throw std::runtime_error("Cannot walk to " + std::to_string(next) + "; the street is 0.." + std::to_string(kDoor));
        }
        x_ = next;
        return wsi::Json{{"x", x_}};
    }

    wsi::Json deliver() {
        if (x_ != kDoor) throw std::runtime_error("The door is at 3, you are at " + std::to_string(x_));
        if (delivered_) throw std::runtime_error("Already delivered");
        delivered_ = true;
        return wsi::Json{{"delivered", true}};
    }

 private:
    int x_ = 0;
    bool delivered_ = false;
};

}  // namespace

int main() {
    try {
        Courier nia(0);
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("walk", wsi::Json{{"dx", 3}}), wsi::testing::call("deliver")}),
        });
        auto agent = agents.start(nia, wsi::StartOptions{.name = "Nia", .goal = "Deliver the parcel to the door"});
        const auto walked = agent.turn();
        const auto delivered = agent.turn();
        std::cout << walked.action << " " << walked.args.dump() << " -> " << walked.result.dump() << "\n";
        std::cout << delivered.action << " -> " << delivered.result.dump() << "\n";
        agents.close();
        if (nia.x() != 3 || !nia.delivered()) throw std::runtime_error("Courier did not deliver the parcel");
        if (walked.attempts != 1 || delivered.action != "deliver") throw std::runtime_error("Unexpected turn results");
        std::cout << "OK — parcel delivered.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
