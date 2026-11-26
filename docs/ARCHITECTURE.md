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
   |  HTTPS, JSON
   v
 Scenario REST API (api.cloud.scenario.com)
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

The plugin calls Scenario's REST API (`https://api.cloud.scenario.com/v1`) with JSON requests and replies, the same API as the other Scenario plugins. `FScenarioRestClient` sends each request behind the `IScenarioBackend` interface, and `FScenarioApi` turns the plugin's calls into typed results.

- A signed-in person's requests name the team and project in the query (`teamId`, `projectId`); an API key is already scoped to its project. Listings name less: teams nothing, projects the team only. An API key cannot list teams, so it reads its own project.
- Errors are sorted into kinds (`Auth`, `Forbidden`, `Quota`, `Moderated`, `RateLimit`, `NotFound`, `Invalid`, `Transport`, `Timeout`, `ServerError`, `Protocol`, ...) with one message each, built from the API's `reason`. Messages are cleaned of signed URL parameters and authorization headers before they reach a log, a toast or an agent.
- Each request says whether it reads, prices, can spend or cancels. Reads and dry runs are retried with backoff; anything else is sent once. Credentials the API's gateway refuses (401, or 403 with `x-amzn-errortype: AccessDeniedException`) never reached a handler, so the token is renewed and the request resent once, paid or not.
- The API has no long-poll: jobs are read one by one at the poll interval.
- Creative Unit amounts stay exact decimal strings from parsing to display; no floating-point rounding on the money path.

## Trained models

A trained model (a LoRA or a composition) is never the model called: it runs on a base model. Its record lists no inputs of its own.

- **Bases.** `Resources/Trained.json` lists the base models to read. The catalog reads their schemas once and caches them with its rows (24 hours): a base takes a trained model when its `loras` or `modelId` input lists the trained model's `type` in `modelTypes`. The list says which models to read, never what they take.
- **Kind.** A LoRA when a `loras` input lists the type; it runs with `loras: [id]` and `lorasScale: [weight]`. A composition when only a `modelId` input does; it runs with `modelId: id`, which replaces any LoRAs. `compliantModelIds` (base names such as `flux.1-dev`) only orders the candidates, through the file's name map. A type no base lists cannot run, and the plugin says so; nothing is guessed.
- **Weight and trigger words.** The weight is the record's own (`concepts[].scale` for its id), else 0.75, within the base's range. Trigger words are `parameters.conceptPrompt`, else `promptEmbedding`, else a word in capitals that every sample prompt holds (two or more); they go once before the prompt unless it already holds them.
- **Form.** The catalog's `GetSchema` gives a trained model the base's form without its trained inputs, plus `lorasScale` (a number, LoRAs only) and `addTriggerWords`, and a `trained` object (kind, base, weight range, trigger words).
- **Runs.** `FScenarioSession::Resolve` turns a request for the trained model into the base's request (`ScenarioTrained::BaseParameters`) before the fingerprint, the price check and the paid call, so the money contract below applies unchanged and a quote covers the trained model and its weight. The job list and provenance keep the base as the model and the trained model beside it (`TrainedModelId`).
- One trained model per run in version 0.2.

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

`FScenarioLedger` keeps every request in `<Project>/Saved/Scenario/` (newest first, atomic writes, revision counter). Rows carry the local id (`local_<ms>_<n>`), Scenario's job id once known, model (for a trained model's run, its base, with the trained model beside it), prompt, quoted and charged CU, state and files.

- On startup, active rows resume polling, and downloads resume from their `.part` files with HTTP range requests.
- An expired download link is replaced by a fresh one from Scenario; partial data is kept.
- Unknown rows are checked against Scenario's recent jobs, and linked only when exactly one job matches.

## Threading

All network and file IO is asynchronous. HTTP runs on Unreal's HTTP module; callbacks return to the game thread. Large files are written on the thread pool. Slate views read cached state and never block: for example, the Agents section reads the agents' config files on the thread pool, not per frame. Imports and placements run on the game thread inside one `FScopedTransaction` each, so one Ctrl+Z removes a placement.

## Credentials

- OAuth: the plugin signs in as "Scenario for Unreal Engine", its registered public OAuth application on Scenario's identity provider (`clerk.scenario.com`), with a fixed client id and the scopes `openid profile email offline_access`; the ID token's email shows who is signed in. Browser sign-in with PKCE and a loopback redirect on `127.0.0.1` (the first free port of 38917 to 38919; the provider accepts a loopback redirect on any port), with a device-code fallback. The refresh token is stored in one record with the client id, issuer and account it belongs to, and a token saved for another application is deleted, never sent; the access token stays in memory. Editors on one machine signed in to the same account share the saved session: each refresh replaces the refresh token, so an editor refreshes with the newest saved one, and when the provider refuses its token because another editor refreshed first, it retries once with the token that editor saved. `SCENARIO_OAUTH_ISSUER` and `SCENARIO_OAUTH_CLIENT_ID`, set together, select a development instance.
- API key and secret: checked with a free call, then stored.
- Stores: macOS Keychain and Windows Credential Manager. On Linux, version 0.2 uses a file readable only by the user, and the Settings page says it is not encrypted.
- Nothing secret is written to project config, `Saved/`, logs, provenance or diagnostics.

## Imports

- Meshes go through Interchange (GLB) with explicit pipelines: copies of the engine's default glTF, FBX and OBJ, or texture pipelines with the plugin's policy set by reflection (scale 1, so glTF meters become centimeters; simple collision; Nanite from Project Settings; no generated lightmap UVs; materials and textures imported). Properties an engine version lacks are logged and skipped. After the build, Nanite stays on only when every material slot is opaque or masked. Static or skeletal is decided from the file; morph targets and animations come with it.
- Textures get settings per role from Scenario's asset metadata and the file name: sRGB off for normal, roughness, metallic, smoothness and height maps; normal maps use the normal compression. A smoothness map is used as roughness; with Invert smoothness on it gets an inverted copy, made from its source pixels as linear data, used as roughness instead.
- Material results are wired into a material instance. Its parent materials (PBR, unlit, sky) are built by code on first use in `<Output root>/_Shared/`, so the plugin ships no content. The PBR parent reads `r.Substrate`: a Substrate slab graph when it is on, a legacy graph otherwise.
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

- `ScenarioToolsets` holds `UScenarioToolset` (18 tools) and `UScenarioAgentSkill`. Each tool adapts one action of the registry above; no logic is duplicated. Results are `UToolCallAsyncResult` objects completed when the action answers, so long actions (`WaitForJob`) do not block the game thread.
- Optional: `Scenario.uplugin` lists `ToolsetRegistry` as an optional plugin. `ScenarioToolsets.Build.cs` adds the dependency and `WITH_SCENARIO_TOOLSETS=1` only when the engine has the registry folder; the reflected headers are guarded by an engine version check that the header tool understands. On 5.6 and 5.7 the module compiles to nothing.
- Spending: the registry has no caller identity and Epic's server no token, so the toolset calls with `FScenarioCaller::bSpendWaitsForApproval`. `FScenarioActions::Call` then checks the quote and `approved_cost`, keeps an approval keyed by quote, answers `pending_approval` with an `approval_id` at once and asks the person with a toast. Approve runs the action once; Decline, a timeout or the quote's expiry sends nothing. `job_status` and `wait_for_job` accept the `approval_id`. The session's money rules still apply underneath.
- Optional string parameters default to a word (`all`, `auto`, `default`, `*`, `selection`, `none`): the registry fills a left-out parameter only from its default, and treats an empty default as none.

## Tool list size

Measured on Unreal 5.8.3 with the Scenario plugin and Epic's Model Context Protocol and AllToolsets plugins (53 toolsets). Tokens are bytes divided by four.

| What | Bytes | About tokens |
|---|---|---|
| Scenario MCP server, `tools/list` (47 tools) | 29,175 | 7,290 |
| Scenario MCP server, `tools/list` with **Agent tool search** (4 tools) | 1,456 | 360 |
| Epic's server, `tools/list` (3 meta tools) | 1,719 | 430 |
| Epic's server, `list_toolsets` | 13,366 | 3,340 |
| Epic's server, `describe_toolset` for the Scenario toolset (18 tools) | about 11,600 | 2,900 |

The full list is above 6,000 tokens, so the server has an opt-in tool search mode (`bAgentToolSearch`, off by default): `tools/list` returns `scenario_status`, `find_tools`, `describe_tools` and `call_tool`, and every tool stays callable by name. It is off by default because clients that already defer tool definitions gain nothing from it and lose the per-tool annotations in the list.

## Simplifications in version 0.2

These are deliberate, listed so reviewers can judge them:

- **API-key workspace.** An API key is scoped to one Scenario project. The plugin takes the first project the key lists as that project. This is an assumption: Scenario does not document it.
- **Action registry in C++.** Actions are C++ structs registered at startup, not `UObject` classes. Python and Blueprints reach them through `UScenarioScriptLibrary` by name with JSON arguments, the same contract as MCP tools.
- **Session-only state.** Prompt Spark proposals, uploaded references and pending agent proposals are kept for the editor session only.
- **Restyle lane.** Captures the active level viewport only; cameras and Movie Render Queue are not wired yet.
- **Audio.** Placed as an Ambient Sound; Level Sequence placement is not supported.
- **Recent jobs on Scenario.** `list_generations` returns at most the 50 most recent; the Library's **On Scenario** tab loads assets 50 at a time.
- **Linux credentials** use the owner-only file store; libsecret is not linked.
- **Telemetry** is opt-in and asked once; the client sends nothing while no collection endpoint exists.

## Tests

- `Scenario.Core.*` automation tests: JSON-RPC and SSE parsing, error kinds, retry policy, quotes and fingerprints, the job list state machine, schema forms, naming, OAuth helpers, the local HTTP listener, the session against a fake Scenario, trained models (bases, trigger words, the base's request, fingerprints), recorded schema fixtures in `Plugins/Scenario/Tests/Fixtures/`.
- `Scenario.Agent.*` automation tests: server checks (Host, Origin, token, sessions, JSON-RPC errors), tool list budget, tool search mode, permissions, client config writers.
- `Scenario.Toolsets.*` automation tests (5.8): registration, each tool's schema against its action, the spend gate (a paid call through the registry answers `pending_approval`, consumes no quote and writes no job), reads without sign-in, the agent skill's size.
- `tools/contract_check.py`: a free check against the live service (reads and dry runs only).
- Live paid runs are manual, with cheap models and low settings, recorded in `tests/live/LEDGER.md`.
