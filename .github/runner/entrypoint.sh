#!/bin/bash
# Registers the runner once (RUNNER_TOKEN, valid an hour, from the repo's Settings > Actions > Runners > New),
# then runs it. The registration lives in the volume, so later starts need no token.
set -e
cd /home/runner/actions-runner
if [ ! -f .runner ]; then
  : "${RUNNER_TOKEN:?first start: set RUNNER_TOKEN (repo Settings > Actions > Runners > New self-hosted runner)}"
  ./config.sh --unattended --url "https://github.com/${REPO:-davr/rbstemsplus}" --token "$RUNNER_TOKEN" \
    --name "${RUNNER_NAME:-rbstems-docker}" --labels "${RUNNER_LABELS:-}" --work _work --replace
fi
exec ./run.sh
