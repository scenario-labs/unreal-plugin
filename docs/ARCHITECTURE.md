# Architecture

How Scenario for Unreal Engine is built, for contributors and reviewers. The code map is in [AGENTS.md](../AGENTS.md); this page explains the design and the choices behind it.

## Shape

```
 People                          Agents (Claude Code, Codex, Cursor, ...)
   |  Studio tab, composer,        |  HTTP, JSON-RPC (Streamable HTTP)
   |  context menus                v                                v
   |                       ScenarioAgent: local Scenario      Epic's Unreal MCP (5.8) -> Toolset Registry
   |                       MCP server (127.0.0.1:9877)          -> ScenarioToolsets: Scenario toolset, agent skill
   v                               v                                v
 ScenarioEditor: action registry  <-  Python, Blueprints, -run=ScenarioEditor.Scenario commandlet
   |  import (Interchange), placement (one transaction), provenance, Slate UI
   v
 ScenarioCore: session, quotes, job list, transfers, OAuth, credential store
   |  HTTPS, JSON-RPC 2.0, SSE or JSON replies
   v
 Hosted Scenario MCP (mcp.scenario.com)
```

Four editor modules and one small runtime module. Only `ScenarioGameplay` is compiled into a packaged game.

| Module | Loading phase | Depends on the editor | Role |
|---|---|---|---|
| `ScenarioCore` | Default | No | Talks to Scenario. Owns every money rule. Unit-tested against a fake transport and a fake Scenario. |
| `ScenarioEditor` | PostEngineInit | Yes | Studio and composer, imports, placement, provenance, settings, the action registry, scripting entry points and the commandlet. |
| `ScenarioAgent` | PostEngineInit | Yes | The local Scenario MCP server, client setup, permissions UI and activity log. Can be removed without touching the others. |
| `ScenarioToolsets` | PostEngineInit | Yes | Registers a Scenario toolset and agent skill with Epic's Toolset Registry (Unreal 5.8). Builds empty on 5.6 and 5.7; no other module depends on it. |
| `ScenarioGameplay` | Default (runtime) | No | Pickup, goal and game rules actors (timer, music, Slate HUD) used by games the kit builds. No network code. |

## Kit

The kit turns Scenario results that are already imported into a walkable level, a game or a film. It is Python (`Content/Python/scenario_unreal`) because it drives editor APIs (levels, Sequencer, Movie Render Queue, materials) that Python reaches without a C++ wrapper per call, and it can change without a rebuild.

- `Resources/Kit.json` declares each tool: name, description, JSON schema, permission kind, whether it runs inside one undo step. `ScenarioKitActions` registers them in the action registry, so agents, scripts and the commandlet see them like any other action.
- A call runs `scenario_unreal.recipes.run(name, arguments)` through the Python plugin; the result comes back as one marked JSON line. Errors the caller can fix come back as `Kit: <message>`.
- The kit never talks to Scenario and never spends: assets come from the money-checked tools first.
- Tools that open a level or start a render run outside an undo step. The kit saves the levels it created (under `/Game/Scenario`) and never saves other levels.
- Films render with Movie Render Queue in the editor (image sequence), then ffmpeg, when installed, encodes the MP4 with the film's sounds.

## Backend

The plugin calls the hosted Scenario MCP (`https://mcp.scenario.com/mcp?toolsets=full`) over JSON-RPC 2.0, the same backend as the other Scenario plugins. `IScenarioBackend` keeps the transport behind an interface so a REST client could replace it.

- Replies come as SSE or plain JSON; the parser keeps the message matching the request id.
- Errors are sorted into kinds (`Auth`, `Forbidden`, `Quota`, `Moderated`, `RateLimit`, `NotFound`, `Invalid`, `Transport`, `Timeout`, `ServerError`, `Protocol`, ...) with one message each. Messages are cleaned of signed URL parameters and authorization headers before they reach a log, a toast or an agent.
- Reads and dry runs are retried with backoff. Any tool not on the read allow-list is treated as able to spend and is never retried.
- Creative Unit amounts stay exact decimal strings from parsing to display; no floating-point rounding on the money path.

## Money contract

Enforced in `FScenarioSession`, below every caller: the Studio, the composer, agents, Python, Blueprints and the commandlet.

1. **Price first.** A price check is a dry run: free, returns the exact cost.
2. **Single-use quotes.** A quote is bound to a fingerprint, SHA-256 of the target (model or workflow), the workspace and the canonical JSON of the parameters. It expires after 120 seconds and is consumed by the paid call. Agents must pass `quote_id` and `approved_cost` equal to the quote's exact string.
3. **Intent before spend.** The job row is written to the job list on disk before the paid request. If it cannot be written, nothing is sent.
4. **Sent once.** A paid call is never retried. A transport failure while sending marks the job **Unknown** (shown as **Not confirmed**); an identical request is then blocked until the person checks Scenario or marks it as not sent.
5. **Kill switch.** `SCENARIO_NO_SPEND=1` refuses paid calls before any request.
6. **Agent spend visibility.** Every agent spend raises a notification and an activity log row. An optional setting asks the person to approve each one.

No Creative Unit cap is set by the plugin.

## Job list and recovery

`FScenarioLedger` keeps every request in `<Project>/Saved/Scenario/` (newest first, atomic writes, revision counter). Rows carry the local id (`local_<ms>_<n>`), Scenario's job id once known, model, prompt, quoted and charged CU, state and files.

- On startup, active rows resume polling, and downloads resume from their `.part` files with HTTP range requests.
- An expired download link is replaced by a fresh one from Scenario; partial data is kept.
- Unknown rows are checked against Scenario's recent jobs, and linked only when exactly one job matches.

## Threading

All network and file IO is asynchronous. HTTP runs on Unreal's HTTP module; callbacks return to the game thread. Large files are written on the thread pool. Slate views read cached state and never block: for example, the Agents section reads the agents' config files on the thread pool, not per frame. Imports and placements run on the game thread inside one `FScopedTransaction` each, so one Ctrl+Z removes a placement.

## Credentials

- OAuth: browser sign-in with PKCE and a loopback redirect on a random local port, with a device-code fallback. The refresh token is stored; the access token stays in memory.
- API key and secret: checked with a free call, then stored.
- Stores: macOS Keychain and Windows Credential Manager. On Linux, version 0.1 uses a file readable only by the user, and the Settings page says it is not encrypted.
- Nothing secret is written to project config, `Saved/`, logs, provenance or diagnostics.

## Imports

- Meshes go through Interchange (GLB). Static or skeletal is decided from the file; morph targets and animations come with it. Collision and Nanite follow project settings.
- Textures get settings per role from Scenario's asset metadata and the file name: sRGB off for normal, roughness, metallic and height maps; normal maps use the normal compression.
- Material results are wired into a material instance. Its parent materials (PBR, unlit, sky) are built by code on first use in `<Output root>/_Shared/`, so the plugin ships no content.
- Video becomes a File Media Source on a Media Plate; skies become a sky sphere, or a cubemap on an HDRI Backdrop for `.hdr` files. Optional engine plugins (Media Plate, HDRI Backdrop, Python) are reached at runtime by class path, so the plugin does not force them on every project.
- Provenance is editor-only package metadata (`Scenario.*` keys) plus an index for the Content Browser filter. It is stripped at cook.

## Action layer

People, agents and scripts share one registry of 39 actions (`FScenarioActions`). Each action declares a JSON input schema, a kind (`Read`, `Editor`, `Spend`, `Change`, `Python`), whether it is destructive or costs CU, and a time limit. `Call()` runs it on the game thread, checks the caller's permissions, guarantees exactly one completion and broadcasts the result for the activity log.

| Caller | Change the project | Run Python |
|---|---|---|
| Agent (over MCP) | Only with **Change this project**; otherwise `apply_result_application` becomes a proposal the person approves | Only with **Run Unreal Python** |
| Epic's Toolset Registry (no token) | Same switch as agents; every paid call answers `pending_approval` and waits for the person | No |
| Script (Python, Blueprint, commandlet) | Yes | No |

The MCP server exposes each action as a tool with annotations derived from its kind. An automation test keeps the whole tool list under 40,000 characters and each description under 2,500, so connecting the server does not flood an agent's context.

## Local Scenario MCP server

Streamable HTTP on `127.0.0.1`, port 9877 or the next free one of ten. Requests are handed to the game thread.

- `Host` must be a single loopback name; any `Origin` header is refused (no browser access); `OPTIONS` is refused.
- Bearer token (two random GUIDs, in the credential store), compared in constant time. `GET /health` answers without it.
- `initialize` returns an `Mcp-Session-Id`; unknown sessions get 404; `DELETE` ends a session. Notifications get 202. Batches are refused.
- Protocol versions 2025-06-18, 2025-03-26 and 2024-11-05.
- Results are JSON text content, plus PNG image content for screenshots; failures set `isError`.

## Epic's Toolset Registry

Unreal 5.8 adds Epic's Toolset Registry (Experimental): classes derived from `UToolsetDefinition` whose static `UFUNCTION(meta=(AICallable))` functions become tools, with schemas and descriptions taken from the reflection data and doc comments. Epic's Model Context Protocol plugin serves the registry through three tools: `list_toolsets`, `describe_toolset` and `call_tool`.

- `ScenarioToolsets` holds `UScenarioToolset` (19 tools) and `UScenarioAgentSkill`. Each tool adapts one action of the registry above; no logic is duplicated. Results are `UToolCallAsyncResult` objects completed when the action answers, so long actions (`WaitForJob`) do not block the game thread.
- Optional: `Scenario.uplugin` lists `ToolsetRegistry` as an optional plugin. `ScenarioToolsets.Build.cs` adds the dependency and `WITH_SCENARIO_TOOLSETS=1` only when the engine has the registry folder; the reflected headers are guarded by an engine version check that the header tool understands. On 5.6 and 5.7 the module compiles to nothing.
- Spending: the registry has no caller identity and Epic's server no token, so the toolset calls with `FScenarioCaller::bSpendWaitsForApproval`. `FScenarioActions::Call` then checks the quote and `approved_cost`, keeps an approval keyed by quote, answers `pending_approval` with an `approval_id` at once and asks the person with a toast. Approve runs the action once; Decline, a timeout or the quote's expiry sends nothing. `job_status` and `wait_for_job` accept the `approval_id`. The session's money rules still apply underneath.
- Optional string parameters default to a word (`all`, `auto`, `default`, `*`, `selection`, `none`): the registry fills a left-out parameter only from its default, and treats an empty default as none.

## Tool list size

Measured on Unreal 5.8.3 with the Scenario plugin and Epic's Model Context Protocol and AllToolsets plugins (53 toolsets). Tokens are bytes divided by four.

| What | Bytes | About tokens |
|---|---|---|
| Scenario MCP server, `tools/list` (48 tools) | 29,425 | 7,350 |
| Scenario MCP server, `tools/list` with **Agent tool search** (4 tools) | 1,456 | 360 |
| Epic's server, `tools/list` (3 meta tools) | 1,719 | 430 |
| Epic's server, `list_toolsets` | 13,366 | 3,340 |
| Epic's server, `describe_toolset` for the Scenario toolset (19 tools) | about 11,600 | 2,900 |

The full list is above 6,000 tokens, so the server has an opt-in tool search mode (`bAgentToolSearch`, off by default): `tools/list` returns `scenario_status`, `find_tools`, `describe_tools` and `call_tool`, and every tool stays callable by name. It is off by default because clients that already defer tool definitions gain nothing from it and lose the per-tool annotations in the list.

## Simplifications in version 0.1

These are deliberate, listed so reviewers can judge them:

- **API-key workspace.** An API key is scoped to one Scenario project. The plugin takes the first project the key lists as that project. This is an assumption: Scenario does not document it.
- **Action registry in C++.** Actions are C++ structs registered at startup, not `UObject` classes. Python and Blueprints reach them through `UScenarioScriptLibrary` by name with JSON arguments, the same contract as MCP tools.
- **Session-only state.** Prompt Spark proposals, uploaded references and pending agent proposals are kept for the editor session only.
- **Restyle lane.** Captures the active level viewport only; cameras and Movie Render Queue are not wired yet.
- **Audio.** Placed as an Ambient Sound; Level Sequence placement is not supported.
- **Smoothness maps** are imported as they are, not inverted into roughness.
- **Recent jobs on Scenario.** `list_generations` returns at most the 50 most recent; the Library's **On Scenario** tab loads assets 50 at a time.
- **Linux credentials** use the owner-only file store; libsecret is not linked.
- **Telemetry** is opt-in and asked once; the client sends nothing while no collection endpoint exists.

## Tests

- `Scenario.Core.*` automation tests: JSON-RPC and SSE parsing, error kinds, retry policy, quotes and fingerprints, the job list state machine, schema forms, naming, OAuth helpers, the local HTTP listener, the session against a fake Scenario, recorded schema fixtures in `Plugins/Scenario/Tests/Fixtures/`.
- `Scenario.Agent.*` automation tests: server checks (Host, Origin, token, sessions, JSON-RPC errors), tool list budget, tool search mode, permissions, client config writers.
- `Scenario.Toolsets.*` automation tests (5.8): registration, each tool's schema against its action, the spend gate (a paid call through the registry answers `pending_approval`, consumes no quote and writes no job), reads without sign-in, the agent skill's size.
- `tools/contract_check.py`: a free check against the live service (reads and dry runs only).
- Live paid runs are manual, with cheap models and low settings, recorded in `tests/live/LEDGER.md`.
