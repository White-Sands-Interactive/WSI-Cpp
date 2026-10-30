#include "session_loop.hpp"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace wsi::testing {
namespace {

class ScriptedModel : public Model {
 public:
    explicit ScriptedModel(std::vector<ToolCall> script) : script_(std::move(script)) {}

    ModelDecision decide(const std::string&, const Json&, const Json&) override {
        std::lock_guard lock(mu_);
        ToolCall next{"wait", Json::object()};
        if (index_ < script_.size()) next = script_[index_];
        ++index_;
        if (!next.arguments.is_object()) next.arguments = Json::object();
        return ModelDecision{std::move(next), TokenUsage{1, 1}};
    }

 private:
    std::mutex mu_;
    std::vector<ToolCall> script_;
    std::size_t index_ = 0;
};

class TestPlatform : public InlinePlatform {
 public:
    explicit TestPlatform(std::shared_ptr<Model> model) : model_(std::move(model)) {}

    void start_session(const StartSession& session) override {
        auto loop = std::make_shared<SessionLoop>(session.session_id, session.transport, session.instructions, session.goal,
                                                  model_, session.logger, session.on_turn_result, session.on_event,
                                                  session.on_usage);
        {
            std::lock_guard lock(mu_);
            loops_[session.session_id] = loop;
        }
        try {
            loop->start();
        } catch (...) {
            std::lock_guard lock(mu_);
            loops_.erase(session.session_id);
            throw;
        }
    }

    void prepare_stop(const std::string& session_id) override {
        std::lock_guard lock(mu_);
        auto found = loops_.find(session_id);
        if (found != loops_.end()) found->second->request_stop();
    }

    void stop_session(const std::string& session_id) override {
        std::shared_ptr<SessionLoop> loop;
        {
            std::lock_guard lock(mu_);
            auto found = loops_.find(session_id);
            if (found == loops_.end()) return;
            loop = found->second;
            loops_.erase(found);
        }
        loop->stop();
    }

    void close() override {
        std::vector<std::shared_ptr<SessionLoop>> running;
        {
            std::lock_guard lock(mu_);
            for (auto& [id, loop] : loops_) {
                loop->request_stop();
                running.push_back(loop);
            }
            loops_.clear();
        }
        for (const auto& loop : running) loop->stop();
    }

 private:
    std::shared_ptr<Model> model_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<SessionLoop>> loops_;
};

}  // namespace

std::shared_ptr<InlinePlatform> create_test_platform(std::vector<ToolCall> script) {
    return create_test_platform(std::make_shared<ScriptedModel>(std::move(script)));
}

std::shared_ptr<InlinePlatform> create_test_platform(std::shared_ptr<Model> model) {
    return std::make_shared<TestPlatform>(std::move(model));
}

}  // namespace wsi::testing
