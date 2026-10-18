# Security Policy

## Reporting a vulnerability

Do not report security vulnerabilities through public GitHub issues or pull
requests. Public reports can expose other users before a fix is available, and
copies may remain accessible after deletion.

Report privately through any of these channels:

- [Open a private vulnerability report](https://github.com/scenario-labs/unreal-plugin/security/advisories/new)
  from this repository's Security tab.
- Email [support@scenario.com](mailto:support@scenario.com) with "security" in the subject.
- Use in-app support at [app.scenario.com](https://app.scenario.com).

Include the affected surface (the Unreal plugin, its local agent server, or the
Scenario platform), the Unreal Engine and plugin versions, your operating
system, reproduction steps and the impact you observe. The plugin version is
shown in Edit > Plugins (search "Scenario") and in
`Plugins/Scenario/Scenario.uplugin`. Use synthetic credentials and minimal
sample projects where possible. Scenario support routes reports to the
appropriate team and follows up through the same private channel.

## Scope

This repository ships Scenario for Unreal Engine, an Unreal Editor plugin. It
signs in to Scenario with OAuth or an API key, calls the Scenario MCP server at
`mcp.scenario.com`, downloads generated files, imports them into the open
project, and runs a local MCP server so agents can drive the editor. Relevant
reports include:

- Unexpected exposure of tokens, keys or secrets through the project folder
  (`Config/`, `Saved/`, imported assets and their provenance metadata), logs,
  error messages or network requests. Secrets belong in the macOS Keychain or
  Windows Credential Manager, or, where neither exists, in an owner-only file
  in the user's config folder; `SCENARIO_API_KEY`, `SCENARIO_API_SECRET` and
  `SCENARIO_ACCESS_TOKEN` are read from the environment when set.
- The local agent server (`127.0.0.1:9877` by default, or the next free port):
  requests accepted without the bearer token, from a non-loopback Host, from a
  web page Origin, or that reach another interface; actions an agent can run
  beyond what Editor Preferences > Plugins > Scenario allows (changing the
  project, running Python, spending without the approval prompt when it is on).
- Paid requests sent twice, sent without a matching single-use quote, or sent
  while `SCENARIO_NO_SPEND=1` is set.
- Unsafe downloads, uploads or imports of images, glTF/GLB, audio, video or
  other generated content, including files written outside the project's
  `Saved/Scenario/` folder and the configured content folder, or credentials
  sent to hosts other than Scenario's.
- Files the Connect buttons write into agent configurations (Claude Code,
  Codex, Cursor, Claude Desktop) outside the documented entries.
- The repository's CI and release supply chain: how the release zip is built
  and published, and what it contains.

Platform vulnerabilities involving `app.scenario.com`, `api.cloud.scenario.com`
or `mcp.scenario.com` use the same private channels above, not this
repository's public issue tracker.

## Supported versions

Security fixes target the [latest release](https://github.com/scenario-labs/unreal-plugin/releases).
Report suspected vulnerabilities promptly even if you cannot update or
reproduce them on the latest version. Older releases have no support branch.

## Secrets in issues and pull requests

API keys and secrets, OAuth tokens, the agent server token and signed asset
URLs are credentials. Never commit them or paste them into public issues or
pull requests. Check screenshots, Output Log excerpts and sample projects
before sharing them. If a secret is already exposed, report it privately
instead of pointing to it in a public issue, revoke the key in Scenario, and
press New token in Scenario > Connection > Agents for the agent token.
