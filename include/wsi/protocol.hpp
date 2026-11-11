#pragma once

#include <string>

namespace wsi {

/// Wire protocol between AgentServer and the platform. Version 1.
/// The Node, Python, and C# SDKs speak this same version.
inline constexpr int kProtocolVersion = 1;

inline constexpr const char* kSdkVersion = "1.1.0";

/// How long a disconnected session can be resumed. Shared by both sides.
inline constexpr int kSessionResumeGraceMs = 30000;

/// MCP revision echoed during initialize. Matches the platform client.
inline constexpr const char* kMcpProtocolVersion = "2025-06-18";

inline constexpr const char* kPlatformUrl = "wss://api.whitesandsint.com";

inline constexpr const char* kSeeDescription =
    "Read the current game view for this player. Prefer wait, which returns this after the game grants a turn.";

inline constexpr const char* kWaitDescription =
    "Block until the game calls turn() for this session, then return the current see() view.";

inline constexpr const char* kDefaultAgentInstructions =
    "You are a remote game agent. Tools are provided by the game.\n"
    "On every turn: call wait first. It blocks until the game grants your turn and then returns the current view from see.\n"
    "After wait returns, call exactly one action tool (not see, not wait).\n"
    "The view's done list is what this character has already called in this session, oldest first. Use it when choosing the next action.\n"
    "If an action tool returns an error, retry once with a corrected action. A second failure ends the turn as failed.";

inline const std::string& default_agent_instructions() {
    static const std::string text(kDefaultAgentInstructions);
    return text;
}

}  // namespace wsi
