# Privacy

This page describes what Scenario for Unreal Engine sends, stores and keeps local, as of version 0.2.1. It was checked against the plugin's code.

## Where the plugin connects

All connections use HTTPS.

| Host | What for |
|---|---|
| `mcp.scenario.com` | Scenario's hosted service: every model, price, job, workflow, asset and account call; sign-in tokens and app registration |
| Scenario's sign-in service, at the addresses Scenario's sign-in metadata lists (`clerk.scenario.com` today) | Signing in on another device, and revoking your session when you sign out |
| `cdn.scenario.com`, `cdn.cloud.scenario.com` | Downloading results. Downloads go to these hosts only |
| `*.amazonaws.com` | Uploading your input files, to the presigned storage addresses Scenario returns |
| The image addresses Scenario returns for thumbnails | Showing thumbnails of models and assets in the Studio |

Sign-in also opens your browser on Scenario's sign-in page, and the sign-in panel links to `www.scenario.com`. While you sign in in the browser, the plugin listens on `127.0.0.1` (ports 38917 to 38919) for the answer. The plugin has no crash reporting, no update check and no analytics service.

## What goes to Scenario

- **Your account or key identity**: an OAuth access token after "Sign in with Scenario", or your API key and secret as a Basic authorization header. For unattended runs, `SCENARIO_ACCESS_TOKEN` or `SCENARIO_API_KEY` and `SCENARIO_API_SECRET` can be read from the environment instead.
- **What you ask for**: the model, your prompt, the settings in the form, and the workspace (team and project) you chose. Model searches and recommendations send their query, which can include your prompt.
- **Price checks**: while you edit the form, the plugin asks Scenario for the price of the current settings. These are dry runs: they carry the same prompt and settings as a generation and cost nothing.
- **Files you choose as inputs**: images, meshes, audio or video from your project or your disk, captures of the level viewport, and meshes exported for a 3D edit. They are uploaded only when you, or an agent connected to the editor, add them as an input. The upload's file name is built from the actor or asset name, and an exported mesh carries the names inside it.
- **Workflow inputs and Prompt Spark text**, when you run them.
- **Credit balance reads**, to show your balance and usage.
- **A User-Agent header** on every request the plugin makes, in the form `ScenarioUnreal/0.2.1 (Unreal 5.8.0; Mac)`: plugin version, engine version and operating system family. It contains no personal or project data.

Scenario processes these under your Scenario account terms and privacy policy. Generated results and uploaded inputs are stored in your Scenario project, as they are when you use the web app.

## What the plugin never sends

- Your level, actors, asset names, file listings or project settings, unless you or an allowed agent send one of them as an input (see the file names above).
- Your OAuth tokens or API secret to any host other than Scenario's (`*.scenario.com`).
- Credentials of any kind with downloads, thumbnails or uploads.

## What stays on your machine

| Data | Where | Committed to source control? |
|---|---|---|
| OAuth refresh token, API key id and secret, the plugin's OAuth client id, the agent server token | macOS Keychain or Windows Credential Manager; on Linux, `~/.config/scenario/unreal/credentials.json`, readable only by you | No |
| OAuth access token | Memory only | No |
| Chosen team and project (ids and names) | `<Project>/Saved/Scenario/workspace.json` | No, `Saved/` is excluded by Unreal's default ignore rules |
| Your editor preferences, including the agent settings | Your per-user editor settings | No |
| Project settings: output folder, prompt in provenance, import options | `Config/DefaultEditor.ini` | Yes, like other project settings |
| Job list (ids, model, prompt, prices, states), model catalog cache, provenance index | `<Project>/Saved/Scenario/` | No |
| Downloaded originals, viewport captures, staged uploads, kit films | `<Project>/Saved/Scenario/` (`Downloads/`, `Captures/`, `Staging/`, `Films/`) | No |
| Imported assets | `/Game/Scenario/...` by default (the folder is a project setting) | Yes, like any asset you import |
| Imported videos | The original file is also copied to `Content/Movies/Scenario/<date>/`, the folder Unreal stages with packaged projects | Yes, and it ships in packaged builds |
| Provenance on each imported asset: asset id, job id, input asset ids, model, lane, prompt and settings, price charged, date, workspace ids, plugin and engine version | Editor-only metadata inside the asset | Yes, with the asset. A project setting leaves the prompt out; other text settings, such as a negative prompt, stay. The metadata is removed when the game is cooked, so it never ships in a build |

Kit films are rendered with a local ffmpeg; nothing about them leaves the machine.

Only the Scenario Gameplay module (pickup, goal and game rules actors) is compiled into a packaged game. It has no network code and holds no Scenario data.

## Agents

### The plugin's local MCP server

The local agent server starts with the editor (a setting turns it off). It listens only on your machine (`127.0.0.1`) and refuses requests from web pages on other hosts. By default it requires a bearer token; Editor Preferences > Plugins > Scenario can turn that off for a client that cannot send headers. A health check answers without the token.

The Set up buttons on the Agent page write the server address and token, in plain text, into your agent's configuration file (`~/.claude.json`, `~/.codex/config.toml`, `~/.cursor/mcp.json` or Claude Desktop's configuration), after saving a timestamped backup of that file. The Claude Desktop entry runs `npx -y mcp-remote`, which downloads that package from npm. "Install the agent skill" writes `~/.claude/skills/scenario-unreal/SKILL.md`.

What an agent reads through the server (level summaries, screenshots, asset details, editor log lines, job results) goes to that agent's own model provider, under your agreement with that provider. The plugin does not control what the provider does with it.

An agent can spend Creative Units only with a price quote it asked for first: the plugin checks that the paid call carries that exact quote, and shows each agent spend in the editor and in the activity log on the Agent page. An optional setting, off by default, asks for your approval in the editor before each agent spend. By default, an agent cannot change your project and cannot run Python. An agent can capture the viewport or export a mesh and upload it as an input without those permissions. You can change these on the Agent page or in Editor Preferences > Plugins > Scenario.

### Through Epic's Unreal MCP (Unreal 5.8)

When Epic's Toolset Registry plugin is enabled, the plugin adds a Scenario toolset to it. Epic's server has no token, so any program on your computer can call it: it can read your models, jobs and assets and ask for prices, which sends a prompt to Scenario. Every paid call through this route waits for your approval in the editor, whatever the Agent page says. This toolset has no upload, capture or Python tools.

The in-engine agent skill puts the names of your chosen team and project into the agent's context, so they reach that agent's model provider.

### Turning spending off

With the environment variable `SCENARIO_NO_SPEND=1`, the plugin refuses every paid call before any request leaves the machine.

## Telemetry

Off by default. After your first successful generation, the plugin asks once whether you want to share anonymous usage statistics; ignoring the question leaves it off. You can change the answer in Editor Preferences > Plugins > Scenario.

Version 0.2.1 sends nothing even when you agree: no collection endpoint exists yet. The events the plugin is ready to send hold only these fields:

| Field | Example |
|---|---|
| Event name | `job_finished` |
| Lane and job kind | `3d`, and `model`, `workflow` or `prompt_spark` |
| Job state | `success`, `failed` |
| Who started it | `person`, `agent`, `script` or `recovered`, and the client's name: `Unreal Editor`, `Toolset Registry`, or the name an agent or script reports for itself, at most 64 characters |
| Plugin version, engine version, OS family | `0.2.1`, `5.8.0`, `Mac` |

Events never include prompts, file contents, asset, level or model names, team, project, job or asset ids, or credentials. This page will be updated before any endpoint is turned on.
