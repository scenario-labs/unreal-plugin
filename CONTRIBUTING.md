# Contributing to Scenario for Unreal Engine

Thanks for helping. This guide covers what a pull request needs; the commands,
rules and code map live in [AGENTS.md](AGENTS.md), which humans and coding
agents both follow.

## Before you start

- For anything larger than a small fix, open a change request first
  ([issue chooser](https://github.com/scenario-labs/unreal-plugin/issues/new/choose))
  so the approach can be agreed before you write it.
- Read the spending rules in `AGENTS.md`: price check first, single-use quotes,
  paid calls sent once, `SCENARIO_NO_SPEND=1` honored. Every change keeps them.

## Set up

- Unreal Engine 5.6, 5.7 or 5.8 with its platform toolchain (Xcode on macOS,
  Visual Studio on Windows). Set `UE_ROOT` to the engine folder if it is not
  `/Users/Shared/Epic Games/UE_5.8`.
- Python 3 for the tools, and [uv](https://docs.astral.sh/uv/) to build the
  docs site.
- Open `ScenarioHost.uproject` at the repository root: it hosts the plugin
  from `Plugins/Scenario/` and enables the engine plugins it uses.

## Run the checks

`tools/build.sh` (package the plugin with `RunUAT BuildPlugin`),
`tools/build.sh --host` (also build the host editor target), `tools/test.sh`
(the automation tests headless, against fake Scenario replies, 0 CU) and
`python3 tools/lint.py`. AGENTS.md lists every check CI runs. Live checks
against Scenario need a test key; `tools/contract_check.py` is free. Any paid
run spends Creative Units and needs authorization for the project and budget
first.

## Commits and pull requests

- Branch from `main`. Pull requests are squash-merged: the PR title becomes the
  commit header and the changelog line, so it follows Conventional Commits with
  a scope listed in AGENTS.md, for example
  `fix(editor): keep the selection after placing a material`.
- Keep one concern per pull request and fill the template briefly: the checks
  you ran, the engine version and OS you ran them on, any action needed after
  merge and any known gap. Tick only what you ran.
- Do not edit versions or `CHANGELOG.md` by hand.
- Show UI changes with before/after screenshots of the Studio tab or the
  composer, attached to the pull request rather than committed.

## Coding agents

Agents follow AGENTS.md. Name the harness in the pull request and keep accurate
co-author trailers on commits and in the squash message. A human reviews the
complete diff before merge.

## Licensing

The repository is under the [MIT License](LICENSE). By contributing, you agree
that your contributions are licensed under the same terms. Code or assets
adopted from elsewhere keep their compatible license and attribution.
