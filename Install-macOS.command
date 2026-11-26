#!/bin/bash
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
# Double-click in Finder to compile Scenario for your Unreal version and install it into a project.
# Needs Xcode. See README.md.
exec /bin/bash "$(dirname "$0")/tools/install/install-scenario.sh" "$@"
