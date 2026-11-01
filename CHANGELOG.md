# Changelog

## Unreleased

- `subgoal` is its own listener event. `add_subgoal`, `update_subgoal`, and `complete_subgoal` are no longer reported as a failed turn. `turn()` still returns the game action.
- The offline test loop accepts those three tools, up to four calls in a turn, and keeps the list only for that session.
- Action history past the latest 50 calls for a session is folded into one summary. Stopping the session deletes its rows.

## 1.0.0

First release of the C++ agent player SDK. It speaks the same protocol as `@whitesands/agent-player`, `whitesands.agent_player`, and `WhiteSands.AgentPlayer`.

- `AgentServer` reads `WSI_PLATFORM_KEY` and connects to `wss://api.whitesandsint.com`. The address is not configurable.
- Players become tools through `bind` and `bind_see`. Argument schemas come from the method signature. `wsi::Json` parameters stay unconstrained.
- `turn()` blocks until the platform reports the action, arguments, result, attempt count, and token usage.
- Typed errors, event listeners, turn timeouts, and abort signals.
- Reconnect with session resume, heartbeat, and a protocol version handshake.
- `create_test_platform` runs the production loop offline.
- Action history is written to `.wsi/agent-memory.sqlite`. Pass `memory = false` to turn it off.
