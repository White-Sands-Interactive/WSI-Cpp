#include "wsi/agent_player.hpp"
#include "wsi/testing.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const std::vector<std::string> kPassengers{"wolf", "goat", "cabbage"};
const std::vector<std::string> kSolution{"goat", "none", "cabbage", "goat", "wolf", "none", "goat"};
const char* kGoal =
    "Get the wolf, the goat, and the cabbage onto the far bank. "
    "Each turn, cross with a passenger from the take list. "
    "none means cross alone.";

std::vector<std::string> listed(const std::vector<std::string>& items) {
    std::vector<std::string> ordered;
    for (const auto& passenger : kPassengers) {
        for (const auto& item : items) {
            if (item == passenger) ordered.push_back(passenger);
        }
    }
    return ordered;
}

std::optional<std::string> unsupervised_danger(const std::vector<std::string>& left_behind) {
    const bool wolf = std::find(left_behind.begin(), left_behind.end(), "wolf") != left_behind.end();
    const bool goat = std::find(left_behind.begin(), left_behind.end(), "goat") != left_behind.end();
    const bool cabbage = std::find(left_behind.begin(), left_behind.end(), "cabbage") != left_behind.end();
    if (wolf && goat) return "The wolf would eat the goat on the bank you are leaving";
    if (goat && cabbage) return "The goat would eat the cabbage on the bank you are leaving";
    return std::nullopt;
}

class Ferryman : public wsi::Player {
 public:
    Ferryman() : near_{"wolf", "goat", "cabbage"} {
        set_description("A ferryman moving a wolf, a goat, and a cabbage across a river");
        bind_see(&Ferryman::see);
        bind(&Ferryman::cross, "cross",
             "Cross to the other bank. passenger must be one value from take in the view. none crosses alone.",
             wsi::args(wsi::enumeration("passenger", {"wolf", "goat", "cabbage", "none"})));
    }

    WSI_TYPE_NAME(Ferryman)

    wsi::Json see() {
        const bool solved = boat_ == "far" && far_.size() == kPassengers.size();
        return wsi::Json{{"boat", boat_},
                         {"near", listed(near_)},
                         {"far", listed(far_)},
                         {"take", take()},
                         {"solved", solved},
                         {"rule", "Call cross with passenger set to a value in take. none crosses alone."}};
    }

    wsi::Json cross(const std::string& passenger) {
        const auto choices = take();
        if (std::find(choices.begin(), choices.end(), passenger) == choices.end()) {
            std::string message = "passenger must be one of: ";
            for (std::size_t i = 0; i < choices.size(); ++i) {
                if (i != 0) message += ", ";
                message += choices[i];
            }
            throw std::runtime_error(message);
        }
        auto& origin = boat_ == "near" ? near_ : far_;
        auto& destination = boat_ == "near" ? far_ : near_;
        if (passenger != "none") {
            origin.erase(std::find(origin.begin(), origin.end(), passenger));
            destination.push_back(passenger);
        }
        boat_ = boat_ == "near" ? "far" : "near";
        last_passenger_ = passenger;
        return wsi::Json{{"boat", boat_}, {"near", listed(near_)}, {"far", listed(far_)}, {"take", take()}};
    }

 private:
    std::vector<std::string> take() const {
        const auto& origin = boat_ == "near" ? near_ : far_;
        const bool alone = !unsupervised_danger(origin) && last_passenger_ != "none";
        const bool wolf = std::find(origin.begin(), origin.end(), "wolf") != origin.end();
        const bool cabbage = std::find(origin.begin(), origin.end(), "cabbage") != origin.end();
        const bool goat = std::find(origin.begin(), origin.end(), "goat") != origin.end();
        if (wolf && cabbage && !goat && alone) return {"none"};
        std::vector<std::string> choices;
        for (const auto& passenger : listed(origin)) {
            if (passenger == last_passenger_) continue;
            std::vector<std::string> left;
            for (const auto& item : origin) {
                if (item != passenger) left.push_back(item);
            }
            if (!unsupervised_danger(left)) choices.push_back(passenger);
        }
        if (choices.empty() && alone) return {"none"};
        return choices;
    }

    std::string boat_ = "near";
    std::vector<std::string> near_;
    std::vector<std::string> far_;
    std::string last_passenger_;
};

std::string render(Ferryman& ferry) {
    const auto view = ferry.see();
    const std::string boat = view.at("boat") == "near" ? "boat ~ ~ ~ ~" : "~ ~ ~ ~ boat";
    std::string near;
    std::string far;
    for (const auto& item : view.at("near")) {
        if (!near.empty()) near.push_back(' ');
        near += item.get<std::string>();
    }
    for (const auto& item : view.at("far")) {
        if (!far.empty()) far.push_back(' ');
        far += item.get<std::string>();
    }
    return "near [" + near + "]  " + boat + "  far [" + far + "]";
}

std::string required_key() {
    const char* value = std::getenv("PLATFORM_KEY");
    if (value == nullptr || value[0] == '\0') value = std::getenv("WSI_PLATFORM_KEY");
    if (value == nullptr || value[0] == '\0') throw std::runtime_error("PLATFORM_KEY is required for --remote");
    return value;
}

std::string required_player_id() {
    const char* value = std::getenv("WSI_PLAYER_ID");
    if (value == nullptr || value[0] == '\0') throw std::runtime_error("WSI_PLAYER_ID is required for --remote");
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool remote = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--remote") remote = true;
        }
        Ferryman ferry;
        std::cout << render(ferry) << "\n";
        std::unique_ptr<wsi::AgentServer> agents;
        if (remote) {
            agents = std::make_unique<wsi::AgentServer>(wsi::AgentServerOptions{.platform_key = required_key()});
        } else {
            std::vector<wsi::testing::ToolCall> script;
            for (const auto& passenger : kSolution) {
                script.push_back(wsi::testing::call("cross", wsi::Json{{"passenger", passenger}}));
            }
            agents = std::make_unique<wsi::AgentServer>(
                wsi::AgentServerOptions{.inline_platform = wsi::testing::create_test_platform(std::move(script))});
        }
        wsi::StartOptions options;
        options.name = "Ferryman";
        options.goal = kGoal;
        if (remote) options.player_id = required_player_id();
        auto agent = agents->start(ferry, options);
        const int limit = remote ? 12 : static_cast<int>(kSolution.size());
        for (int turn = 1; turn <= limit && !ferry.see().at("solved").get<bool>(); ++turn) {
            try {
                const auto result = agent.turn();
                std::cout << "\n" << turn << ". " << result.action << "(" << result.args.dump()
                          << ") attempts=" << result.attempts << "\n";
                std::cout << render(ferry) << "\n";
            } catch (const std::exception& error) {
                const auto* failed = dynamic_cast<const wsi::TurnFailedError*>(&error);
                std::string extra;
                if (failed != nullptr) extra = " after " + std::to_string(failed->attempts().size()) + " attempt(s)";
                std::cout << "\n" << turn << ". turn failed" << extra << ": " << error.what() << "\n";
                std::cout << render(ferry) << "\n";
                break;
            }
        }
        agent.stop();
        agents->close();
        if (!ferry.see().at("solved").get<bool>()) {
            throw std::runtime_error("The wolf, goat, and cabbage did not all reach the far bank");
        }
        std::cout << "\nOK — everyone is on the far bank.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
