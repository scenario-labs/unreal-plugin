// Single source of truth for commit headers and PR titles. CI installs
// commitlint from pnpm-lock.yaml (.github/workflows/pr-name.yml and
// commitlint.yml). The same check locally, with the Node version in .nvmrc:
//
//   pnpm install --frozen-lockfile
//   printf '%s\n' "fix(editor): my title" | pnpm exec commitlint --verbose

// U+2014 written as a code point so that this file contains no em dash itself.
const EM_DASH = String.fromCharCode(0x2014);

/** @type {import('@commitlint/types').Plugin} */
const houseRules = {
  rules: {
    // House style: no em dashes anywhere, commit messages and PR titles included.
    "no-em-dash": ({ raw }) => [
      !(raw ?? "").includes(EM_DASH),
      "no em dashes in commit messages or PR titles (house style)",
    ],
  },
};

/** @type {import('@commitlint/types').UserConfig} */
export default {
  extends: ["@commitlint/config-conventional"],
  plugins: [houseRules],
  rules: {
    // At most 120 characters, as the PR template says.
    "header-max-length": [2, "always", 120],
    // Off: long URLs and Co-Authored-By trailers exceed the 100-character default.
    "body-max-line-length": [0, "always", 200],
    "footer-max-line-length": [0, "always", 200],
    // Off: Unreal, MCP, PBR and CU start many subjects.
    "subject-case": [0],
    // Warn, never fail: a release commit may have no scope.
    "scope-empty": [1, "never"],
    // The scopes listed in AGENTS.md.
    "scope-enum": [
      2,
      "always",
      [
        "core", // ScenarioCore: MCP client, session, quotes, ledger, transfers, credentials
        "editor", // ScenarioEditor: subsystem, imports, placement, provenance, actions
        "ui", // ScenarioEditor/UI: the Studio tab, composer and their look
        "agent", // ScenarioAgent and ScenarioToolsets: the local MCP server, agent setup, the agent skill
        "lanes", // Resources/Lanes.json: lanes, picks, presets
        "docs", // README.md, docs/
        "tools", // tools/
        "tests", // test harness only; a test for a product area keeps the product scope
        "ci", // .github/workflows
        "deps", // pins: actions, npm
        "release", // versions, CHANGELOG plumbing
        "repo", // LICENSE, community files, templates, CODEOWNERS, editor and lint configs, .agents/
      ],
    ],
    "no-em-dash": [2, "always"],
  },
};
