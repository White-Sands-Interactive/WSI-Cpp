#include "wsi/agent_player.hpp"
#include "wsi/testing.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Forest {
 public:
    Forest(int width = 5, int height = 5) : width_(width), height_(height), tiles_(static_cast<std::size_t>(width * height), '.') {
        set(2, 2, 'T');
    }

    bool in_bounds(int x, int y) const { return x >= 0 && x < width_ && y >= 0 && y < height_; }

    char get(int x, int y) const {
        if (!in_bounds(x, y)) throw std::runtime_error("Out of bounds");
        return tiles_[static_cast<std::size_t>(y * width_ + x)];
    }

    void set(int x, int y, char tile) {
        if (!in_bounds(x, y)) throw std::runtime_error("Out of bounds");
        tiles_[static_cast<std::size_t>(y * width_ + x)] = tile;
    }

    int trees_left() const {
        int count = 0;
        for (const char tile : tiles_) {
            if (tile == 'T') ++count;
        }
        return count;
    }

    std::string render() const {
        std::string out;
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) out.push_back(get(x, y));
            if (y + 1 < height_) out.push_back('\n');
        }
        return out;
    }

    int width() const { return width_; }
    int height() const { return height_; }

 private:
    int width_;
    int height_;
    std::vector<char> tiles_;
};

class Villager : public wsi::Player {
 public:
    Villager(Forest& world, std::string name, int x, int y) : world_(world), name_(std::move(name)), x_(x), y_(y) {
        bind_see(&Villager::see);
        bind(&Villager::move, "move", "Step by dx and dy. Stay inside the map.", wsi::names("dx", "dy"));
        bind(&Villager::chop, "chop", "Chop the tree on your tile.");
    }

    WSI_TYPE_NAME(Villager)

    int x() const { return x_; }
    int y() const { return y_; }
    int wood() const { return wood_; }

    wsi::Json see() {
        return wsi::Json{{"name", name_},
                         {"x", x_},
                         {"y", y_},
                         {"wood", wood_},
                         {"tile", std::string(1, world_.get(x_, y_))},
                         {"treesLeft", world_.trees_left()},
                         {"bounds", wsi::Json{{"minX", 0}, {"minY", 0}, {"maxX", world_.width() - 1}, {"maxY", world_.height() - 1}}},
                         {"hint", "move(dx,dy) changes position by integer steps; stay in bounds. chop() only works on tile T."},
                         {"map", world_.render()}};
    }

    wsi::Json move(int dx, int dy) {
        const int next_x = x_ + dx;
        const int next_y = y_ + dy;
        if (!world_.in_bounds(next_x, next_y)) {
            throw std::runtime_error("Cannot move to " + std::to_string(next_x) + "," + std::to_string(next_y));
        }
        x_ = next_x;
        y_ = next_y;
        return wsi::Json{{"x", x_}, {"y", y_}, {"tile", std::string(1, world_.get(x_, y_))}};
    }

    wsi::Json chop() {
        if (world_.get(x_, y_) != 'T') {
            throw std::runtime_error("No tree at " + std::to_string(x_) + "," + std::to_string(y_));
        }
        world_.set(x_, y_, '.');
        wood_ += 1;
        return wsi::Json{{"wood", wood_}, {"treesLeft", world_.trees_left()}};
    }

 private:
    Forest& world_;
    std::string name_;
    int x_ = 0;
    int y_ = 0;
    int wood_ = 0;
};

std::string required_key() {
    const char* value = std::getenv("PLATFORM_KEY");
    if (value == nullptr || value[0] == '\0') value = std::getenv("WSI_PLATFORM_KEY");
    if (value == nullptr || value[0] == '\0') throw std::runtime_error("PLATFORM_KEY is required for --remote");
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        bool remote = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--remote") remote = true;
        }
        Forest world;
        Villager ada(world, "Ada", 0, 2);
        Villager bea(world, "Bea", 4, 4);
        std::cout << "Before:\n" << world.render() << "\n";
        std::cout << "Ada at (" << ada.x() << "," << ada.y() << ") wood=" << ada.wood() << "\n";

        std::unique_ptr<wsi::AgentServer> agents;
        if (remote) {
            agents = std::make_unique<wsi::AgentServer>(wsi::AgentServerOptions{.platform_key = required_key()});
        } else {
            agents = std::make_unique<wsi::AgentServer>(wsi::AgentServerOptions{
                .inline_platform = wsi::testing::create_test_platform(
                    {wsi::testing::call("move", wsi::Json{{"dx", 2}, {"dy", 0}}), wsi::testing::call("chop"),
                     wsi::testing::call("move", wsi::Json{{"dx", -1}, {"dy", 0}})}),
            });
        }

        auto ada_agent = agents->start(ada, wsi::StartOptions{.goal = "Stand on the tree and chop it"});
        auto bea_agent = agents->start(bea);
        const auto moved = ada_agent.turn();
        std::cout << "\nAda " << moved.action << "(" << moved.args.dump() << ")\n";
        std::cout << "Ada at (" << ada.x() << "," << ada.y() << ") tile=" << world.get(ada.x(), ada.y())
                  << " wood=" << ada.wood() << "\n";
        const auto chopped = ada_agent.turn();
        std::cout << "\nAda " << chopped.action << "(" << chopped.args.dump() << ")\n";
        std::cout << world.render() << "\n";
        std::cout << "Ada at (" << ada.x() << "," << ada.y() << ") wood=" << ada.wood()
                  << " treesLeft=" << world.trees_left() << "\n";
        try {
            const auto bea_turn = bea_agent.turn();
            std::cout << "\nBea " << bea_turn.action << "(" << bea_turn.args.dump() << ")\n";
            std::cout << "Bea at (" << bea.x() << "," << bea.y() << ")\n";
        } catch (const std::exception& error) {
            std::cout << "\nBea turn failed (model chose an illegal action): " << error.what() << "\n";
            std::cout << "Bea still at (" << bea.x() << "," << bea.y() << ")\n";
        }
        ada_agent.stop();
        bea_agent.stop();
        agents->close();

        if (!remote && (moved.action != "move" || chopped.action != "chop")) {
            throw std::runtime_error("Scripted forest did not move then chop");
        }
        if (ada.x() != 2 || ada.y() != 2 || ada.wood() != 1 || world.trees_left() != 0) {
            throw std::runtime_error("Forest demo did not produce expected Ada state");
        }
        if (!remote && (bea.x() != 3 || bea.y() != 4)) {
            throw std::runtime_error("Forest demo did not produce expected Bea state");
        }
        std::cout << "\nOK — Ada moved and chopped; tile and wood changed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
