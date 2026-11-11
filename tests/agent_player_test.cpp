#include "memory.hpp"
#include "session_loop.hpp"
#include "turn_gate.hpp"
#include "wsi/agent_player.hpp"
#include "wsi/testing.hpp"
#include "websocket.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;

#define CHECK(...)                                                                                        \
    do {                                                                                                  \
        if (!(__VA_ARGS__)) {                                                                             \
            throw std::runtime_error(std::string(#__VA_ARGS__) + " failed at " + __FILE__ + ":" +         \
                                     std::to_string(__LINE__));                                           \
        }                                                                                                 \
    } while (0)

template <typename Fn>
void test(const char* name, Fn fn) {
    try {
        fn();
        std::cout << "ok " << name << "\n";
    } catch (const std::exception& ex) {
        std::cout << "FAIL " << name << "\n  " << ex.what() << "\n";
        ++g_failed;
    }
}

class TinyWorld : public wsi::Player {
 public:
    TinyWorld() {
        bind_see(&TinyWorld::see);
        bind(&TinyWorld::move, "move", "Move", wsi::names("dx"));
        bind(&TinyWorld::chop, "chop", "Chop");
        bind(&TinyWorld::bad, "bad", "Bad");
        bind(&TinyWorld::wave, "wave", "Wave");
    }

    WSI_TYPE_NAME(TinyWorld)

    int x = 0;
    int wood = 0;
    bool fail_once = true;

    wsi::Json see() { return wsi::Json{{"x", x}, {"wood", wood}}; }

    wsi::Json move(int dx) {
        x += dx;
        return wsi::Json{{"x", x}};
    }

    wsi::Json chop() {
        if (fail_once) {
            fail_once = false;
            throw std::runtime_error("axe slipped");
        }
        wood += 1;
        return wsi::Json{{"wood", wood}};
    }

    wsi::Json bad() { throw std::runtime_error("always bad"); }

    void wave() {}
};

class Gated : public wsi::Player {
 public:
    Gated() {
        bind_see(&Gated::see);
        bind(&Gated::move, "move", "Move", wsi::args(wsi::number("dx")));
    }

    int x = 0;
    wsi::Json see() { return wsi::Json{{"x", x}}; }
    wsi::Json move(double dx) {
        x += static_cast<int>(dx);
        return wsi::Json{{"x", x}};
    }
};

class Idle : public wsi::InlinePlatform {
 public:
    void start_session(const wsi::StartSession&) override {}
    void stop_session(const std::string&) override {}
};

class Recording : public wsi::InlinePlatform {
 public:
    std::optional<std::string> goal;
    std::string instructions;

    void start_session(const wsi::StartSession& session) override {
        goal = session.goal;
        instructions = session.instructions;
    }
    void stop_session(const std::string&) override {}
};

class ViewModel : public wsi::testing::Model {
 public:
    explicit ViewModel(std::vector<std::string>& views, std::vector<wsi::testing::ToolCall> script)
        : views_(views), script_(std::move(script)) {}

    wsi::testing::ModelDecision decide(const std::string&, const wsi::Json&, const wsi::Json& messages) override {
        for (const auto& message : messages) {
            if (message.is_object() && message.value("role", "") == "user") {
                views_.push_back(message.value("content", ""));
                break;
            }
        }
        wsi::testing::ToolCall call{"wait", wsi::Json::object()};
        if (index_ < script_.size()) call = script_[index_];
        ++index_;
        return wsi::testing::ModelDecision{std::move(call), wsi::TokenUsage{1, 1}};
    }

 private:
    std::vector<std::string>& views_;
    std::vector<wsi::testing::ToolCall> script_;
    std::size_t index_ = 0;
};

wsi::Json view_from_prompt(const std::string& prompt) {
    const auto start = prompt.find('{');
    const auto end = prompt.rfind('}');
    CHECK(start != std::string::npos && end != std::string::npos && end >= start);
    return wsi::Json::parse(prompt.substr(start, end - start + 1));
}

class Sample : public wsi::Player {
 public:
    Sample() {
        bind_see(&Sample::see);
        bind(&Sample::move, "move", "", wsi::names("dx", "dy"));
        bind(&Sample::chop, "chop", "");
    }
    wsi::Json see() { return wsi::Json{{"ok", true}}; }
    wsi::Json move(wsi::Json dx, wsi::Json dy) { return dx.get<int>() + dy.get<int>(); }
    wsi::Json chop() { return 1; }
};

class Typed : public wsi::Player {
 public:
    Typed() {
        set_description("A typed player");
        bind_see(&Typed::see);
        bind(&Typed::move, "move", "Step by dx", wsi::args(wsi::integer("dx")));
        bind(&Typed::wave, "wave", "");
    }
    WSI_TYPE_NAME(Typed)
    wsi::Json see() { return wsi::Json{{"ok", true}}; }
    wsi::Json move(int dx) { return dx; }
    wsi::Json wave() { return "hi"; }
};

class NoSee : public wsi::Player {
 public:
    NoSee() { bind(&NoSee::move, "move", ""); }
    wsi::Json move() { return 1; }
};

class Broken : public wsi::Player {
 public:
    Broken() {
        declare("fly", "nope");
        bind_see(&Broken::see);
    }
    WSI_TYPE_NAME(Broken)
    wsi::Json see() { return wsi::Json::object(); }
};

class Pawn : public wsi::Player {
 public:
    Pawn() {
        bind_see(&Pawn::see);
        bind(&Pawn::move, "move", "Move", wsi::names("dx"));
    }
    wsi::Json see() { return wsi::Json::object(); }
    wsi::Json move(int dx) { return wsi::Json{{"dx", dx}}; }
};

class FakePlatform {
 public:
    explicit FakePlatform(wsi::Json hello_err = nullptr) : hello_err_(std::move(hello_err)) {
        server_ = std::make_unique<wsi::net::WsServer>([this](const std::shared_ptr<wsi::net::WsConnection>& conn) {
            {
                std::lock_guard lock(mu_);
                sockets_.push_back(conn);
            }
            conn->on_text([this, conn](const std::string& text) {
                const wsi::Json envelope = wsi::Json::parse(text);
                {
                    std::lock_guard lock(mu_);
                    received_.push_back(envelope);
                }
                const std::string kind = envelope.value("type", "");
                if (kind == "hello") {
                    if (!hello_err_.is_null()) {
                        wsi::Json reply = hello_err_;
                        reply["type"] = "hello_err";
                        conn->send_text(reply.dump());
                    } else {
                        conn->send_text(wsi::Json{{"type", "hello_ok"},
                                                  {"accountName", "fake"},
                                                  {"protocolVersion", wsi::kProtocolVersion}}
                                            .dump());
                    }
                } else if (kind == "start_session") {
                    conn->send_text(wsi::Json{{"type", "session_started"}, {"sessionId", envelope.at("sessionId")}}.dump());
                } else if (kind == "resume_session") {
                    conn->send_text(wsi::Json{{"type", "session_resumed"}, {"sessionId", envelope.at("sessionId")}}.dump());
                } else if (kind == "stop_session") {
                    conn->send_text(wsi::Json{{"type", "session_stopped"}, {"sessionId", envelope.at("sessionId")}}.dump());
                } else if (kind == "ping") {
                    conn->send_text(wsi::Json{{"type", "pong"}, {"id", envelope.at("id")}}.dump());
                }
            });
        });
    }

    ~FakePlatform() {
        if (server_) server_->close();
    }

    std::string url() const { return "ws://127.0.0.1:" + std::to_string(server_->port()); }

    std::vector<wsi::Json> received() const {
        std::lock_guard lock(mu_);
        return received_;
    }

    void abort_first() {
        std::lock_guard lock(mu_);
        CHECK(!sockets_.empty());
        sockets_.front()->abort();
    }

    void send_latest(const std::string& text) {
        std::lock_guard lock(mu_);
        CHECK(!sockets_.empty());
        sockets_.back()->send_text(text);
    }

    std::size_t socket_count() const {
        std::lock_guard lock(mu_);
        return sockets_.size();
    }

 private:
    wsi::Json hello_err_;
    mutable std::mutex mu_;
    std::vector<wsi::Json> received_;
    std::vector<std::shared_ptr<wsi::net::WsConnection>> sockets_;
    std::unique_ptr<wsi::net::WsServer> server_;
};

void wait_until(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

int main() {
    test("turn gate pairs wait with turn", [] {
        wsi::TurnGate gate;
        std::mutex mu;
        std::vector<std::string> events;
        std::thread waiter([&] {
            gate.wait_for_turn();
            std::lock_guard lock(mu);
            events.push_back("wait-released");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        {
            std::lock_guard lock(mu);
            CHECK(events.empty());
        }
        std::thread turning([&] {
            const auto outcome = gate.run_turn(std::nullopt, nullptr);
            std::lock_guard lock(mu);
            events.push_back(outcome.ok ? "turn-ok" : "turn-fail");
        });
        waiter.join();
        {
            std::lock_guard lock(mu);
            CHECK(events.size() == 1);
            CHECK(events[0] == "wait-released");
        }
        wsi::TurnReport report;
        report.ok = true;
        gate.complete_turn(report);
        turning.join();
        CHECK(events.size() == 2);
        CHECK(events[1] == "turn-ok");
    });

    test("memory keeps one session oldest first and caps the view", [] {
        const auto dir = std::filesystem::temp_directory_path() / ("wsi-memory-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
        {
            wsi::SessionMemory memory((dir / "memory.sqlite").string());
            memory.record("ada", "Ada", wsi::Json{{"action", "cross"}, {"args", wsi::Json{{"passenger", "goat"}}}, {"ok", true}});
            memory.record("ada", "Ada",
                          wsi::Json{{"action", "cross"},
                                    {"args", wsi::Json{{"passenger", "wolf"}}},
                                    {"ok", false},
                                    {"error", "wrong bank"}});
            memory.record("bea", "Bea", wsi::Json{{"action", "move"}, {"args", wsi::Json{{"dx", 1}}}, {"ok", true}});
            CHECK(memory.done("ada") == wsi::Json::parse(
                                             R"([{"action":"cross","args":{"passenger":"goat"},"ok":true},{"action":"cross","args":{"passenger":"wolf"},"ok":false,"error":"wrong bank"}])"));
            CHECK(memory.done("bea") ==
                  wsi::Json::parse(R"([{"action":"move","args":{"dx":1},"ok":true}])"));
            for (int i = 0; i < 55; ++i) {
                memory.record("cap", "Ada", wsi::Json{{"action", "move"}, {"args", wsi::Json{{"dx", i}}}, {"ok", true}});
            }
            const auto done = memory.done("cap");
            CHECK(done.size() == 51);
            CHECK(done[0].at("summary").at("move") == 5);
            CHECK(!done[0].contains("lastError"));
            CHECK(done[1].at("args") == wsi::Json{{"dx", 5}});
            CHECK(done[50].at("args") == wsi::Json{{"dx", 54}});
            memory.close();
        }
        std::filesystem::remove_all(dir);
    });

    test("memory folds older rows and drops a finished session", [] {
        const auto dir = std::filesystem::temp_directory_path() / ("wsi-memory-fold-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
        {
            wsi::SessionMemory memory((dir / "memory.sqlite").string());
            memory.record("ada", "Ada",
                          wsi::Json{{"action", "cross"},
                                    {"args", wsi::Json{{"passenger", "goat"}}},
                                    {"ok", false},
                                    {"error", "slipped"}});
            memory.record("ada", "Ada",
                          wsi::Json{{"action", "cross"},
                                    {"args", wsi::Json{{"passenger", "wolf"}}},
                                    {"ok", false},
                                    {"error", "snapped"}});
            for (int i = 0; i < 50; ++i) {
                memory.record("ada", "Ada", wsi::Json{{"action", "move"}, {"args", wsi::Json{{"dx", i}}}, {"ok", true}});
            }
            const auto first = memory.done("ada");
            CHECK(first.size() == 51);
            CHECK(first[0].at("summary").at("cross") == 2);
            CHECK(first[0].at("lastError") == "snapped");
            CHECK(first[1].at("args") == wsi::Json{{"dx", 0}});
            CHECK(first[50].at("args") == wsi::Json{{"dx", 49}});

            for (int i = 50; i < 100; ++i) {
                memory.record("ada", "Ada", wsi::Json{{"action", "move"}, {"args", wsi::Json{{"dx", i}}}, {"ok", true}});
            }
            const auto second = memory.done("ada");
            CHECK(second.size() == 51);
            CHECK(second[0].at("summary").at("cross") == 2);
            CHECK(second[0].at("summary").at("move") == 50);
            CHECK(second[0].at("lastError") == "snapped");
            CHECK(second[1].at("args") == wsi::Json{{"dx", 50}});
            CHECK(second[50].at("args") == wsi::Json{{"dx", 99}});

            memory.record("bea", "Bea", wsi::Json{{"action", "move"}, {"args", wsi::Json{{"dx", 1}}}, {"ok", true}});
            memory.forget("ada");
            CHECK(memory.done("ada") == wsi::Json::array());
            CHECK(memory.done("bea") == wsi::Json::parse(R"([{"action":"move","args":{"dx":1},"ok":true}])"));
            memory.close();
        }
        std::filesystem::remove_all(dir);
    });

    test("view with done and the default path", [] {
        CHECK(wsi::view_with_done(wsi::Json{{"boat", "near"}}, wsi::Json::array()) ==
              wsi::Json{{"boat", "near"}, {"done", wsi::Json::array()}});
        CHECK(wsi::view_with_done("plain", wsi::Json::array()) ==
              wsi::Json{{"view", "plain"}, {"done", wsi::Json::array()}});
        const auto path = std::filesystem::path(wsi::default_memory_path());
        CHECK(path.filename() == "agent-memory.sqlite");
        CHECK(path.parent_path().filename() == ".wsi");
    });

    test("reflect publishes see actions and wait", [] {
        Sample sample;
        const auto reflected = sample.reflect();
        CHECK(reflected.see.name == "see");
        CHECK(reflected.actions.size() == 2);
        CHECK(reflected.actions[0].name == "chop");
        CHECK(reflected.actions[1].name == "move");
        CHECK(reflected.tools.size() == 4);
        const auto& move = reflected.tools[2];
        CHECK(move.at("name") == "move");
        CHECK(move.at("inputSchema").at("required") == wsi::Json::array({"dx", "dy"}));
        CHECK(!move.at("inputSchema").at("properties").at("dx").contains("type"));
        const auto action = reflected.actions[1];
        CHECK(action.invoke(wsi::Json{{"dx", 2}, {"dy", 3}}) == 5);
    });

    test("bind_dynamic publishes a callable and checks its arguments", [] {
        class Dynamic : public wsi::Player {
         public:
            Dynamic() {
                bind_dynamic("see", "", {}, [this](const wsi::Json&) { return wsi::Json{{"n", n}}; });
                bind_dynamic("add", "Add dx", {wsi::integer("dx")}, [this](const wsi::Json& args) {
                    n += args.at("dx").get<int>();
                    return wsi::Json{{"n", n}};
                });
            }
            int n = 1;
        } player;
        const auto reflected = player.reflect();
        CHECK(reflected.actions.size() == 1);
        CHECK(reflected.actions[0].name == "add");
        CHECK(reflected.actions[0].invoke(wsi::Json{{"dx", 4}}) == wsi::Json{{"n", 5}});
        bool rejected = false;
        try {
            reflected.actions[0].invoke(wsi::Json{{"dx", "no"}});
        } catch (const std::runtime_error& ex) {
            rejected = std::string(ex.what()).find("dx") != std::string::npos;
        }
        CHECK(rejected);
        bool hidden = false;
        try {
            player.bind_dynamic("wait", "", {}, [](const wsi::Json&) { return wsi::Json(nullptr); });
        } catch (const std::runtime_error&) {
            hidden = true;
        }
        CHECK(hidden);
    });

    test("reflect requires see and rejects a missing declared action", [] {
        try {
            NoSee player;
            player.reflect();
            throw std::runtime_error("expected see() to be required");
        } catch (const std::runtime_error& ex) {
            CHECK(std::string(ex.what()).find("see") != std::string::npos);
        }
        try {
            Broken player;
            player.reflect();
            throw std::runtime_error("expected a missing action to fail");
        } catch (const std::runtime_error& ex) {
            CHECK(std::string(ex.what()).find("fly") != std::string::npos);
        }
    });

    test("declared args are checked and undeclared methods stay tools", [] {
        Typed player;
        const auto reflected = player.reflect();
        CHECK(reflected.description == "A typed player");
        CHECK(reflected.actions.size() == 2);
        const wsi::Json* move_tool = nullptr;
        for (const auto& tool : reflected.tools) {
            if (tool.at("name") == "move") move_tool = &tool;
        }
        CHECK(move_tool != nullptr);
        CHECK(move_tool->at("description") == "Step by dx");
        CHECK(move_tool->at("inputSchema").at("properties").at("dx").at("type") == "integer");
        const wsi::ReflectedAction* move = nullptr;
        for (const auto& action : reflected.actions) {
            if (action.name == "move") move = &action;
        }
        CHECK(move != nullptr);
        bool threw = false;
        try {
            move->invoke(wsi::Json{{"dx", "no"}});
        } catch (const std::runtime_error& ex) {
            threw = true;
            CHECK(std::string(ex.what()).find("dx") != std::string::npos);
        }
        CHECK(threw);
        CHECK(move->invoke(wsi::Json{{"dx", 4}}) == 4);
    });

    test("scripted turn returns the action", [] {
        std::vector<wsi::AgentEvent> events;
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform({wsi::testing::call("move", wsi::Json{{"dx", 3}})}),
            .on_event = [&](const wsi::AgentEvent& event) { events.push_back(event); },
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        const auto result = agent.turn();
        CHECK(result.action == "move");
        CHECK(result.args == wsi::Json{{"dx", 3}});
        CHECK(result.attempts == 1);
        CHECK(result.usage.prompt_tokens == 1);
        CHECK(world.x == 3);
        bool saw = false;
        for (const auto& event : events) {
            if (event.type == "action" && event.action == "move") saw = true;
        }
        CHECK(saw);
        agents.close();
    });

    test("subgoal is its own event and the turn stays the game action", [] {
        std::vector<wsi::AgentEvent> events;
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("add_subgoal", wsi::Json{{"objective", "  reach the far side  "}}),
                 wsi::testing::call("update_subgoal", wsi::Json{{"id", 1}, {"progress", " stepped once "}}),
                 wsi::testing::call("move", wsi::Json{{"dx", 3}})}),
            .on_event = [&](const wsi::AgentEvent& event) { events.push_back(event); },
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        const auto result = agent.turn();
        CHECK(result.action == "move");
        CHECK(result.attempts == 1);
        CHECK(world.x == 3);
        CHECK(events.size() == 3);
        CHECK(events[0].type == "subgoal");
        CHECK(events[0].action == "add_subgoal");
        CHECK(events[0].args->at("id") == "1");
        CHECK(events[0].args->at("objective") == "  reach the far side  ");
        CHECK(!events[0].error);
        CHECK(events[1].type == "subgoal");
        CHECK(events[1].action == "update_subgoal");
        CHECK(events[1].args->at("id") == "1");
        CHECK(events[2].type == "action");
        agents.close();
    });

    test("a rejected subgoal is not a failed turn", [] {
        std::vector<wsi::AgentEvent> events;
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("add_subgoal", wsi::Json{{"objective", "   "}}),
                 wsi::testing::call("move", wsi::Json{{"dx", 1}})}),
            .on_event = [&](const wsi::AgentEvent& event) { events.push_back(event); },
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        const auto result = agent.turn();
        CHECK(result.action == "move");
        CHECK(result.attempts == 1);
        CHECK(events.size() == 2);
        CHECK(events[0].type == "subgoal");
        CHECK(events[0].error == "objective is required");
        CHECK(events[1].type == "action");
        agents.close();
    });

    test("completing a subgoal removes it", [] {
        std::vector<std::string> views;
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(std::make_shared<ViewModel>(
                views, std::vector<wsi::testing::ToolCall>{
                           wsi::testing::call("add_subgoal", wsi::Json{{"objective", "reach the far side"}}),
                           wsi::testing::call("move", wsi::Json{{"dx", 1}}),
                           wsi::testing::call("complete_subgoal", wsi::Json{{"id", "1"}}),
                           wsi::testing::call("move", wsi::Json{{"dx", 1}}),
                           wsi::testing::call("move", wsi::Json{{"dx", 1}})})),
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        agent.turn();
        agent.turn();
        agent.turn();
        CHECK(views.at(0).find("Active subgoals: none.") != std::string::npos);
        CHECK(views.at(2).find("1. reach the far side") != std::string::npos);
        CHECK(views.at(4).find("Active subgoals: none.") != std::string::npos);
        CHECK(world.x == 3);
        agents.close();
    });

    test("a platform subgoal event is not a failed turn", [] {
        const auto event = wsi::loop_event_to_agent_event(
            "s1", wsi::Json{{"event", "subgoal"},
                            {"action", "add_subgoal"},
                            {"args", wsi::Json{{"objective", "reach the far side"}, {"id", "1"}}}});
        CHECK(event.type == "subgoal");
        CHECK(event.action == "add_subgoal");
        CHECK(event.args->at("id") == "1");
        CHECK(!event.error);

        const auto rejected = wsi::loop_event_to_agent_event(
            "s1", wsi::Json{{"event", "subgoal"},
                            {"action", "add_subgoal"},
                            {"args", wsi::Json{{"objective", ""}}},
                            {"error", "objective is required"}});
        CHECK(rejected.type == "subgoal");
        CHECK(rejected.error == "objective is required");
    });

    test("retries once after a rejected action", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("chop"), wsi::testing::call("chop")}),
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        const auto result = agent.turn();
        CHECK(result.attempts == 2);
        CHECK(result.action == "chop");
        CHECK(world.wood == 1);
        agents.close();
    });

    test("turn failed lists both attempts", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("bad"), wsi::testing::call("bad")}),
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        try {
            agent.turn();
            throw std::runtime_error("expected TurnFailedError");
        } catch (const wsi::TurnFailedError& error) {
            CHECK(std::string(error.what()) == "always bad");
            CHECK(error.attempts().size() == 2);
            CHECK(error.attempts()[0].action == "bad");
            CHECK(error.attempts()[0].error == "always bad");
            CHECK(error.attempts()[1].action == "bad");
            CHECK(error.attempts()[1].error == "always bad");
        }
        agents.close();
    });

    test("two sessions share one script", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("move", wsi::Json{{"dx", 1}}), wsi::testing::call("move", wsi::Json{{"dx", 5}})}),
            .memory = false,
        });
        TinyWorld first;
        TinyWorld second;
        auto agent_a = agents.start(first);
        auto agent_b = agents.start(second);
        agent_a.turn();
        agent_b.turn();
        CHECK(first.x == 1);
        CHECK(second.x == 5);
        agents.close();
    });

    test("turn timeout", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = std::make_shared<Idle>(),
            .turn_timeout_ms = 20,
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        bool threw = false;
        try {
            agent.turn();
        } catch (const wsi::TurnTimeoutError&) {
            threw = true;
        }
        CHECK(threw);
        agents.close();
    });

    test("goal and extra instructions", [] {
        auto recording = std::make_shared<Recording>();
        wsi::AgentServer agents(wsi::AgentServerOptions{.inline_platform = recording, .memory = false});
        TinyWorld world;
        agents.start(world, wsi::StartOptions{.goal = "Collect wood", .instructions = "Stay calm."});
        CHECK(recording->goal == "Collect wood");
        CHECK(recording->instructions.find("Stay calm.") != std::string::npos);
        CHECK(recording->instructions.find("call wait first") != std::string::npos);
        CHECK(recording->instructions.find("done list") != std::string::npos);
        agents.close();
    });

    test("schema failure retries", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(
                {wsi::testing::call("move", wsi::Json{{"dx", "nope"}}), wsi::testing::call("move", wsi::Json{{"dx", 2}})}),
            .memory = false,
        });
        Gated world;
        auto agent = agents.start(world);
        const auto result = agent.turn();
        CHECK(result.attempts == 2);
        CHECK(world.x == 2);
        agents.close();
    });

    test("platform key comes from the environment", [] {
        const char* previous = std::getenv("WSI_PLATFORM_KEY");
        const std::string saved = previous ? previous : "";
        ::unsetenv("WSI_PLATFORM_KEY");
        bool threw = false;
        try {
            wsi::AgentServer agents;
            (void)agents;
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()).find("WSI_PLATFORM_KEY") != std::string::npos;
        }
        CHECK(threw);
        ::setenv("WSI_PLATFORM_KEY", "pk_test", 1);
        wsi::AgentServer agents(wsi::AgentServerOptions{.inline_platform = std::make_shared<Idle>()});
        if (previous) ::setenv("WSI_PLATFORM_KEY", saved.c_str(), 1);
        else ::unsetenv("WSI_PLATFORM_KEY");
    });

    test("earlier actions show up on the next view", [] {
        const auto dir = std::filesystem::temp_directory_path() / ("wsi-views-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
        std::vector<std::string> views;
        {
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .inline_platform = wsi::testing::create_test_platform(std::make_shared<ViewModel>(
                    views, std::vector<wsi::testing::ToolCall>{wsi::testing::call("move", wsi::Json{{"dx", 1}}),
                                                               wsi::testing::call("move", wsi::Json{{"dx", 4}})})),
                .memory = true,
                .memory_path = (dir / "memory.sqlite").string(),
            });
            TinyWorld world;
            auto agent = agents.start(world, wsi::StartOptions{.name = "Ada"});
            agent.turn();
            agent.turn();
            CHECK(view_from_prompt(views.at(0)).at("done") == wsi::Json::array());
            CHECK(view_from_prompt(views.at(1)).at("done") ==
                  wsi::Json::parse(R"([{"action":"move","args":{"dx":1},"ok":true}])"));
            CHECK(world.x == 5);
            agents.close();
        }
        std::filesystem::remove_all(dir);
    });

    test("memory off omits done", [] {
        std::vector<std::string> views;
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform(std::make_shared<ViewModel>(
                views, std::vector<wsi::testing::ToolCall>{wsi::testing::call("move", wsi::Json{{"dx", 1}})})),
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world, wsi::StartOptions{.name = "Ada"});
        agent.turn();
        CHECK(!view_from_prompt(views.at(0)).contains("done"));
        agents.close();
    });

    test("abort rejects the turn", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{.inline_platform = std::make_shared<Idle>(), .memory = false});
        TinyWorld world;
        auto agent = agents.start(world);
        wsi::AbortSignal signal;
        signal.abort("stop");
        bool threw = false;
        try {
            agent.turn(wsi::TurnOptions{.signal = &signal});
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "stop";
        }
        CHECK(threw);
        agents.close();
    });

    test("void action is reported as ok", [] {
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .inline_platform = wsi::testing::create_test_platform({wsi::testing::call("wave")}),
            .memory = false,
        });
        TinyWorld world;
        auto agent = agents.start(world);
        const auto result = agent.turn();
        CHECK(result.action == "wave");
        CHECK(result.result == wsi::Json{{"ok", true}});
        CHECK(result.usage.completion_tokens == 1);
        agents.close();
    });

    test("a platform session requires a character id", [] {
        const auto id = std::string("3f6c2d1e-8b7a-4c5d-9e0f-1a2b3c4d5e6f");
        {
            FakePlatform platform;
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .platform_key = "pk_test",
                .memory = false,
                .platform_url = platform.url(),
            });
            Pawn pawn;
            bool threw = false;
            try {
                agents.start(pawn);
            } catch (const std::runtime_error& error) {
                threw = std::string(error.what()) == "playerId is required. Copy the character ID from the web app";
            }
            CHECK(threw);
            for (const auto& item : platform.received()) {
                CHECK(item.value("type", "") != "start_session");
            }
            agents.close();
        }
        {
            FakePlatform platform;
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .platform_key = "pk_test",
                .memory = false,
                .platform_url = platform.url(),
            });
            Pawn pawn;
            bool threw = false;
            try {
                agents.start(pawn, wsi::StartOptions{.player_id = "  not-a-character  "});
            } catch (const std::runtime_error& error) {
                threw = std::string(error.what()) ==
                        "playerId \"not-a-character\" is not a character ID. Copy the ID from the character in the web app";
            }
            CHECK(threw);
            agents.close();
        }
        {
            FakePlatform platform;
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .platform_key = "pk_test",
                .memory = false,
                .platform_url = platform.url(),
            });
            Pawn pawn;
            auto agent = agents.start(pawn, wsi::StartOptions{.player_id = "  " + id + "  "});
            bool sent = false;
            for (const auto& item : platform.received()) {
                if (item.value("type", "") == "start_session") {
                    CHECK(item.at("playerId") == id);
                    sent = true;
                }
            }
            CHECK(sent);
            agent.stop();
            agents.close();
        }
        {
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .inline_platform = wsi::testing::create_test_platform({wsi::testing::call("move", wsi::Json{{"dx", 1}})}),
                .memory = false,
            });
            Pawn pawn;
            auto agent = agents.start(pawn);
            const auto result = agent.turn();
            CHECK(result.action == "move");
            agents.close();
        }
    });

    test("remote subgoal event is not a failed turn", [] {
        FakePlatform platform;
        std::vector<wsi::AgentEvent> events;
        std::mutex events_mu;
        wsi::AgentServer agents(wsi::AgentServerOptions{
            .platform_key = "pk_test",
            .on_event = [&](const wsi::AgentEvent& event) {
                std::lock_guard lock(events_mu);
                events.push_back(event);
            },
            .memory = false,
            .platform_url = platform.url(),
        });
        Pawn pawn;
        auto agent = agents.start(pawn, wsi::StartOptions{.goal = "Step", .player_id = "3f6c2d1e-8b7a-4c5d-9e0f-1a2b3c4d5e6f"});
        wsi::TurnResult result;
        std::exception_ptr error;
        std::thread turning([&] {
            try {
                result = agent.turn(wsi::TurnOptions{.timeout_ms = 5000});
            } catch (...) {
                error = std::current_exception();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        platform.send_latest(wsi::Json{{"type", "session_event"},
                                       {"sessionId", agent.session_id()},
                                       {"event", "subgoal"},
                                       {"action", "add_subgoal"},
                                       {"args", wsi::Json{{"objective", "reach the far side"}, {"id", "1"}}}}
                                 .dump());
        platform.send_latest(wsi::Json{{"type", "turn_result"},
                                       {"sessionId", agent.session_id()},
                                       {"ok", true},
                                       {"action", "move"},
                                       {"args", wsi::Json{{"dx", 1}}},
                                       {"result", wsi::Json{{"dx", 1}}},
                                       {"attempts", 1},
                                       {"usage", wsi::Json{{"promptTokens", 3}, {"completionTokens", 2}}}}
                                 .dump());
        turning.join();
        if (error) std::rethrow_exception(error);
        CHECK(result.action == "move");
        CHECK(result.attempts == 1);
        bool saw_subgoal = false;
        bool saw_failure = false;
        {
            std::lock_guard lock(events_mu);
            for (const auto& event : events) {
                if (event.type == "subgoal" && event.action == "add_subgoal" && event.args &&
                    event.args->at("id") == "1") {
                    saw_subgoal = true;
                }
                if (event.type == "turnFailed") saw_failure = true;
            }
        }
        CHECK(saw_subgoal);
        CHECK(!saw_failure);
        agents.close();
    });

    test("hello errors and reconnect", [] {
        {
            FakePlatform platform(wsi::Json{{"code", "PROTOCOL"}, {"message", "upgrade"}});
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .platform_key = "pk_test",
                .memory = false,
                .platform_url = platform.url(),
            });
            Pawn pawn;
            bool threw = false;
            try {
                agents.start(pawn);
            } catch (const wsi::ProtocolVersionError&) {
                threw = true;
            }
            CHECK(threw);
            const auto received = platform.received();
            CHECK(!received.empty());
            CHECK(received[0].at("type") == "hello");
            CHECK(received[0].at("protocolVersion") == wsi::kProtocolVersion);
            agents.close();
        }
        {
            FakePlatform platform(wsi::Json{{"code", "AUTH"}, {"message", "bad key"}});
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .platform_key = "pk_wrong",
                .memory = false,
                .platform_url = platform.url(),
            });
            Pawn pawn;
            bool threw = false;
            try {
                agents.start(pawn);
            } catch (const wsi::PlatformAuthError&) {
                threw = true;
            }
            CHECK(threw);
            agents.close();
        }
        {
            FakePlatform platform;
            std::vector<std::string> events;
            std::mutex events_mu;
            wsi::AgentServer agents(wsi::AgentServerOptions{
                .platform_key = "pk_test",
                .on_event = [&](const wsi::AgentEvent& event) {
                    std::lock_guard lock(events_mu);
                    events.push_back(event.type);
                },
                .memory = false,
                .platform_url = platform.url(),
            });
            Pawn pawn;
            auto agent = agents.start(pawn, wsi::StartOptions{.goal = "Step", .player_id = "3f6c2d1e-8b7a-4c5d-9e0f-1a2b3c4d5e6f"});
            wsi::Json started;
            for (const auto& item : platform.received()) {
                if (item.value("type", "") == "start_session") started = item;
            }
            CHECK(started.at("goal") == "Step");
            CHECK(started.at("playerId") == "3f6c2d1e-8b7a-4c5d-9e0f-1a2b3c4d5e6f");
            platform.abort_first();
            wait_until([&] {
                std::lock_guard lock(events_mu);
                for (const auto& event : events) {
                    if (event == "reconnected") return true;
                }
                return false;
            });
            {
                std::lock_guard lock(events_mu);
                bool disconnected = false;
                for (const auto& event : events) {
                    if (event == "disconnected") disconnected = true;
                }
                CHECK(disconnected);
            }
            CHECK(platform.socket_count() == 2);
            bool resumed = false;
            for (const auto& item : platform.received()) {
                if (item.value("type", "") == "resume_session" && item.value("sessionId", "") == agent.session_id() &&
                    item.value("sessionToken", "") == started.at("sessionToken").get<std::string>()) {
                    resumed = true;
                }
            }
            CHECK(resumed);
            for (const auto& item : platform.received()) {
                if (item.value("type", "") == "resume_session") {
                    CHECK(!item.contains("playerId"));
                }
            }
            wsi::TurnResult result;
            std::exception_ptr error;
            std::thread turning([&] {
                try {
                    result = agent.turn(wsi::TurnOptions{.timeout_ms = 5000});
                } catch (...) {
                    error = std::current_exception();
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            platform.send_latest(wsi::Json{{"type", "turn_result"},
                                           {"sessionId", agent.session_id()},
                                           {"ok", true},
                                           {"action", "move"},
                                           {"args", wsi::Json{{"dx", 1}}},
                                           {"result", wsi::Json{{"dx", 1}}},
                                           {"attempts", 1},
                                           {"usage", wsi::Json{{"promptTokens", 3}, {"completionTokens", 2}}}}
                                     .dump());
            turning.join();
            if (error) std::rethrow_exception(error);
            CHECK(result.action == "move");
            CHECK(result.args == wsi::Json{{"dx", 1}});
            CHECK(result.result == wsi::Json{{"dx", 1}});
            CHECK(result.attempts == 1);
            CHECK(result.usage.prompt_tokens == 3);
            CHECK(result.usage.completion_tokens == 2);
            agents.close();
        }
    });

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
