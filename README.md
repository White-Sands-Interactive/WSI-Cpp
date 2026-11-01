# WSI Agent Player for C++

The library starts a remote game agent from a player class. The game calls `start` and `turn`. The SDK publishes that class as tools. The model runs on the platform, so the game process never holds the model key.

This is the C++ port of [`@whitesands/agent-player`](https://github.com/White-Sands-Interactive/WSI-Node), `whitesands.agent_player`, and `WhiteSands.AgentPlayer`. All four speak protocol version 1.

The library also provides:

- A player class turned into tools, with argument schemas taken from the method signature.
- One action per turn, and one retry when that action throws.
- An offline loop for tests, with no socket and no platform key.
- A local SQLite log of what each character has already called.

## Build

C++20, CMake 3.16 or newer, OpenSSL, and SQLite. macOS and Linux. The platform connection is a POSIX WebSocket client.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`FetchContent` downloads nlohmann/json 3.11.3. Headers install from `include/wsi`. Link the `wsi_agent_player` target, or the installed archive, and also link Threads, SQLite, and OpenSSL.

## Usage

Set the platform key, then start one agent per character. `turn()` grants that character one chance to act and returns what they did. It blocks the calling thread.

```sh
export WSI_PLATFORM_KEY=pk_...
```

The SDK connects to `wss://api.whitesandsint.com`. That address is not read from the environment or from the command line.

```cpp
#include "wsi/agent_player.hpp"

wsi::AgentServer agents;
auto ada = agents.start(villager, wsi::StartOptions{.goal = "Stand on the tree and chop it"});

wsi::TurnResult result = ada.turn();
// result.action, result.args, result.result, result.attempts, result.usage
```

`AgentServerOptions{.platform_key = "pk_..."}` overrides `WSI_PLATFORM_KEY`.

### The player class

C++ has no runtime method list, so each action is registered in the constructor with `bind` and `bind_see`. That list is what the other SDKs build by reflecting public methods. `see` is required. It returns the JSON the model reads at the start of the turn. Names that start with `_`, and `wait` (the SDK provides that tool), cannot be published.

Throw to refuse an action. The platform asks the model once more. A second failure throws `TurnFailedError` from `turn()`.

```cpp
class Villager : public wsi::Player {
 public:
    Villager(Forest& world, std::string name) : world_(world), name_(std::move(name)) {
        bind_see(&Villager::see);
        bind(&Villager::move, "move", "Step by dx and dy. Stay inside the map.", wsi::names("dx", "dy"));
        bind(&Villager::chop, "chop", "Chop the tree on your tile.");
    }

    wsi::Json see() {
        return wsi::Json{{"x", x_}, {"y", y_}, {"wood", wood_}};
    }

    wsi::Json move(int dx, int dy) {
        const int next_x = x_ + dx;
        const int next_y = y_ + dy;
        if (!world_.in_bounds(next_x, next_y)) {
            throw std::runtime_error("Cannot move to " + std::to_string(next_x) + "," + std::to_string(next_y));
        }
        x_ = next_x;
        y_ = next_y;
        return wsi::Json{{"x", x_}, {"y", y_}};
    }

    wsi::Json chop() {
        if (world_.get(x_, y_) != 'T') {
            throw std::runtime_error("No tree at " + std::to_string(x_) + "," + std::to_string(y_));
        }
        wood_ += 1;
        return wsi::Json{{"wood", wood_}};
    }

 private:
    Forest& world_;
    std::string name_;
    int x_ = 0;
    int y_ = 0;
    int wood_ = 0;
};
```

`wsi::names(...)` names the arguments. Their JSON Schema comes from the C++ parameter types. A parameter of type `wsi::Json` accepts any JSON value, which is what an undeclared Python or Node argument does. A method that returns `void`, or JSON `null`, is reported as `{ "ok": true }`.

### Declaring arguments

`bind` takes the description the model sees. Pass `wsi::args(...)` when the schema should not be inferred, for example a string enum. A schema failure counts as a failed action and uses the one retry.

```cpp
bind(&Ferryman::cross, "cross",
     "Cross to the other bank.",
     wsi::args(wsi::enumeration("passenger", {"wolf", "goat", "cabbage", "none"})));
```

| C++ parameter | JSON Schema | Accepted values |
| --- | --- | --- |
| `int` and other integers | integer | integers. `true` and `false` are rejected |
| `float`, `double` | number | integers and floats |
| `std::string` | string | strings |
| `bool` | boolean | `true` and `false` |
| `std::vector<T>` | array | arrays, and each item when the item type is set |
| `wsi::Json` | unconstrained | any JSON value |
| `wsi::enumeration` | string enum | one of those values |
| `std::optional<T>` | same as `T`, not required | omitted, `null`, or a value of `T` |

Argument names must match the method parameters in order. A missing required argument fails before the method runs. Extra keys are ignored. `declare("fly")` records a name that must be bound before `start`; a missing method throws.

### Turns

`turn()` does not choose the action. It unblocks that session's `wait`. The platform reads `see()`, calls one action, and reports back.

```cpp
wsi::AbortSignal signal;
result = ada.turn(wsi::TurnOptions{.timeout_ms = 30000, .signal = &signal});
```

| Field | Meaning |
| --- | --- |
| `action` | Tool the model called |
| `args` | Arguments it passed |
| `result` | What your method returned, after a JSON round trip |
| `attempts` | `1`, or `2` when the first call failed and the retry succeeded |
| `usage` | `prompt_tokens` and `completion_tokens` for that turn |

The default timeout is 60 seconds. Pass `signal` to abort the wait. Call `signal.abort()` and `turn()` throws that reason.

`handle.stop()` ends that session. `agents.close()`, also run by the destructor, stops every session on the server. The platform process keeps running.

### Errors

| Error | When |
| --- | --- |
| `PlatformAuthError` | The platform rejected the key |
| `ProtocolVersionError` | This SDK and the platform disagree on the protocol |
| `TurnFailedError` | The action and the retry both failed. `error.attempts()` lists both calls |
| `TurnTimeoutError` | The platform did not finish within `timeout_ms` |
| `ConnectionLostError` | The socket dropped and the session could not be resumed |

The SDK reconnects with backoff. A disconnected session can be resumed for 30 seconds.

### Several characters

One `AgentServer` hosts every character. Each `start()` is its own session.

```cpp
auto ada = agents.start(ada_villager, wsi::StartOptions{.name = "Ada", .goal = "Chop the tree"});
auto bea = agents.start(bea_villager, wsi::StartOptions{.name = "Bea", .goal = "Carry wood home"});

ada.turn();
bea.turn();
```

The game decides whose turn it is by calling `turn()` on that handle.

### Events

`agents.on` hears every session. `handle.on` hears one session. The returned `Subscription` unsubscribes when it is destroyed.

| `event.type` | Fields |
| --- | --- |
| `action` | `session_id`, `action`, `args` |
| `subgoal` | `session_id`, `action`, `args`, and `error` when the call was rejected. `args` includes `id` when the platform assigned or received one |
| `retry` | `session_id`, `action`, `args`, `error` |
| `turnFailed` | `session_id`, `error`, and `action` / `args` when the retry was a real tool |
| `disconnected` | none. No `session_id` |
| `reconnected` | none. No `session_id` |

`disconnected` and `reconnected` are connection events. They are delivered to `agents.on` only.

### Action history

Every `start()` records that character's calls in one SQLite file, `.wsi/agent-memory.sqlite` in the process's current directory. The directory is created on the first `start()`. Deleting it is safe. The next start creates it again.

The next `see()` view includes `done`, oldest first, up to the last 50 calls for that session:

```json
{
  "x": 1,
  "y": 0,
  "done": [
    { "action": "move", "args": { "dx": 1, "dy": 0 }, "ok": true }
  ]
}
```

A refused call is `{ "ok": false, "error": "..." }`. A new `start()` has an empty `done` list. Past 50 calls, older ones become a single summary at the front of `done`: a count for each action, and the last error when one of them failed. Stopping the session deletes its rows. Two characters on one server share the file and see only their own rows. The name on each row is the `name` passed to `start()`, or the player type name.

```cpp
wsi::AgentServer agents(wsi::AgentServerOptions{.memory_path = "saves/agents.sqlite"});
```

`AgentServerOptions{.memory = false}` turns the log off. The view then has no `done` field.

### Goal and instructions

`goal` is added to the model prompt for that character. `instructions` on `start()` is appended to the default prompt. `instructions` on `AgentServer` replaces the default prompt for every character on that server.

### Testing without a platform

`wsi::testing::create_test_platform` runs the same wait, act, and retry loop in memory. The script is the list of tool calls, in order. No key and no socket. Every session on that server consumes the script from one shared queue.

```cpp
#include "wsi/testing.hpp"

wsi::AgentServer agents(wsi::AgentServerOptions{
    .inline_platform = wsi::testing::create_test_platform({
        wsi::testing::call("move", wsi::Json{{"dx", 1}, {"dy", 0}}),
        wsi::testing::call("chop"),
    }),
});
```

When the script runs out, the stand-in model calls `wait`, which is not an action, so the turn fails.

### Where each piece runs

| | Game process | Platform |
| --- | --- | --- |
| World and player methods | yes | |
| Tools (`see`, actions, `wait`) | yes | calls them |
| Whose turn it is | `turn()` | |
| Which action to take | | the model |
| Model API key | | yes |

### Logging

Pass a `Logger` to print SDK warnings. Leave the functions empty and nothing is written.

```cpp
wsi::Logger logger;
logger.warn = [](const std::string& message) { std::cerr << message << "\n"; };
wsi::AgentServer agents(wsi::AgentServerOptions{.logger = logger});
```

### Protocol

`wsi/protocol.hpp` exports the wire constants and `kProtocolVersion`. The Node SDK's copy is `src/protocol.ts`. The Python SDK's copy is `whitesands.agent_player.protocol`. The C# SDK's copy is `WhiteSands.AgentPlayer.Protocol`. A change to the wire format has to land in every place. Game code uses `AgentServer` and does not include the protocol header.

Messages on the socket use the same camelCase fields as the other SDKs (`sessionId`, `promptTokens`, `inputSchema`). The C++ API uses snake_case (`session_id`, `prompt_tokens`).

## This repository

| Path | Role |
| --- | --- |
| `include/wsi` | Public headers |
| `src` | The SDK |
| `examples/forest.cpp` | Two villagers, bound methods |
| `examples/courier.cpp` | An integer argument and a goal |
| `examples/ferry.cpp` | One passenger chosen each turn |
| `tests/agent_player_test.cpp` | The same cases as the other SDKs |

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/wsi_forest
./build/wsi_courier
./build/wsi_ferry
```

Forest, courier, and ferry replay a script. Add `--remote` to forest or ferry to connect to `wss://api.whitesandsint.com`:

```sh
PLATFORM_KEY=pk_... ./build/wsi_forest --remote
PLATFORM_KEY=pk_... ./build/wsi_ferry --remote
```

Releases are recorded in [CHANGELOG.md](CHANGELOG.md).
