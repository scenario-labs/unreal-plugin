# Scenario for Unreal Engine

Every Scenario model inside the Unreal Editor: images, video, 3D, PBR materials, audio and skies. Each request is priced before you send it, and each result lands as a native Unreal asset, placed in your level in one undo step. For games, virtual production, film, architecture and previs.

People use the Studio tab and a quick composer in the viewport. Agents (Claude Code, Codex, Cursor, Claude Desktop) use the same features through the plugin's local Scenario MCP server.

- **Every model and workflow** in your Scenario account, including your team's trained models, with lanes and recommended picks for Unreal.
- **Priced first.** The Generate button shows the exact Creative Unit cost of your settings; a paid request is sent once and survives editor restarts.
- **Native results.** Meshes through Interchange with PBR materials, skeletons and morph targets; textures with the right settings per map; material instances; Sound Waves; Media Plates; sky spheres and HDRI backdrops.
- **Provenance.** Every imported asset keeps its model, prompt, price and job id, stripped from cooked builds.
- **Agents.** A local Scenario MCP server with one-click setup, price quotes, permissions and an activity log.

## Requirements

- Unreal Engine 5.6, 5.7 or 5.8 (Editor). macOS is the tested platform in version 0.1; Windows and Linux are compiled but not tested yet.
- A Scenario account: [app.scenario.com](https://app.scenario.com).

## Install

Unzip the release into your project so the folder reads `<Project>/Plugins/Scenario/Scenario.uplugin`, open the project, and click the **Scenario** button in the level editor toolbar. Release zips are not signed yet: on macOS, run `xattr -dr com.apple.quarantine "<Project>/Plugins/Scenario"` once if the editor cannot load the plugin.

From source: clone this repository and copy `Plugins/Scenario/` into your project's `Plugins/` folder; Unreal builds it when the project opens (needs a C++ toolchain).

## Documentation

- [Quick start](docs/QUICK_START.md): a first asset in your level in two minutes.
- [User guide](docs/USER_GUIDE.md): sign-in, lanes, jobs and recovery, imports, settings, scripting.
- [Scenario MCP guide](docs/MCP_GUIDE.md): connect an agent, permissions, tools.
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Privacy](docs/PRIVACY.md): what is sent, stored and kept local.
- [Architecture](docs/ARCHITECTURE.md): for contributors.

## Development

The repository root is a host project (`ScenarioHost.uproject`) for building and testing the plugin. With Unreal installed (`UE_ROOT` points to it):

```sh
tools/build.sh --host   # package the plugin and build the host editor target
tools/test.sh           # automation tests, headless
python3 tools/lint.py   # repository checks that need no engine
```

Rules for contributors and coding agents are in [AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md). Report security issues as described in [SECURITY.md](SECURITY.md); for help, see [SUPPORT.md](SUPPORT.md).

## License

[MIT](LICENSE). Scenario is a trademark of Scenario Inc. Unreal and Unreal Engine are trademarks of Epic Games, Inc.
