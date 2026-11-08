# Working on the GPU machine interactively

> **Status: a proposal.** None of it is set up. The machine and its runner are in [GPU-RUNNER.md](GPU-RUNNER.md); starting and
> stopping it automatically is in [GPU-AUTOSTOP.md](GPU-AUTOSTOP.md).

## The problem
Through the GitHub runner, every action is a workflow run: dispatch, wait for a runner, check out, run, read the log. That is
30-90 seconds per step, fine for a benchmark, slow for debugging ("why did DirectML refuse this node", "try that flag"). The user asked
whether Claude can be installed on the machine so that it and the cloud session can communicate.

## What the cloud session can reach
- **Over the network:** GitHub, and the hosts the user allows, over HTTPS only. No SSH, WinRM or RDP to the machine (option E below).
- **Remote Control tools:** the session has `list_computer_folders` and `request_computer_folder`. They connect **this session to
  a folder on one of the user's own computers** that is serving it through Remote Control, so that the session's Bash and file tools
  run there. At the time of writing `list_computer_folders` returns no computers (none is connected), and its documentation says
  using a folder from a cloud session "is not available to everyone yet". **So option B needs a trial on the user's account; it
  isn't a promise.**
- **Session messaging:** `list_sessions`, `ListAgents` and `send_message` let this session send a message to another Claude session
  of the same account (a Remote Control session on another machine appears there).

## The options

| | How it works | Speed and feel | The user does | Risks |
|---|---|---|---|---|
| **A. Runner, one workflow per action** (today) | Claude dispatches a workflow and reads its log | 30-90 s per step | the runner setup (GPU-RUNNER.md) | slow to debug; robust, logged, and the only option where the machine stops by itself |
| **B. Remote Control: the cloud session attaches to the machine** | Claude Code (or the Claude desktop app) on the machine serves a folder such as `C:\work`; Claude calls `request_computer_folder` and its shell, file and edit tools run on the machine | seconds per command, like a shell: build, run the harness, read logs, edit | install Claude on the machine and sign in once; approve the attach prompt for each session | depends on the account having the feature; the machine bills while Claude works; an attached session can run any command as the machine's user |
| **C. A second Claude session on the machine that the cloud session messages** | a CLI session on the machine (connected through Remote Control) appears in `list_sessions`; Claude sends it tasks with `send_message` ("build, run the bench, report") | fewer round trips; it works while Claude does something else | same install as B | two agents to keep in step; its own usage; a message isn't a shell: Claude depends on its report |
| **D. A "command pump" job on the runner** | a long workflow job (hours) polls a file or branch for command files, runs them, writes the output back | about 5-15 s per command, GitHub path only | the runner | a hand-built remote shell: it must be limited to what we want to allow; noise in git history |
| **E. Direct SSH / WinRM / RDP from the cloud session** | not possible: only HTTPS to allowed hosts leaves the container | - | - | not recommended |

## Recommendation
- Keep **A** for repeatable benchmarks: it is the only one that starts and stops the machine by itself.
- Add **B** for interactive debugging **if** `list_computer_folders` shows the machine after the setup below. It is the closest to
  sitting at the machine.
- Use **C** if B isn't available but the machine's session is visible in `list_sessions`.
- Build **D** only if neither B nor C works.

Whichever is used, the machine must not be shut down under an active session: GPU-AUTOSTOP.md's idle check counts an active Claude
process as busy.

## Setup for B and C (on the machine; about 20 minutes, using Remote Desktop once)
These steps are a plan, not verified. The Claude Code and Remote Control details may differ; check Anthropic's current
documentation (`claude --help`, the Remote Control page in the Claude Code docs).
1. **Install the tools** in PowerShell as administrator: Git for Windows, Node.js LTS, and Claude Code (the native installer from
   Anthropic's documentation, or `npm install -g @anthropic-ai/claude-code`). MSYS2 isn't needed here: the benchmark's workflow
   installs its own.
2. **Make a working folder:** `git clone https://github.com/davr/rbstemsplus C:\work\rbstemsplus` (check out `claude/dml-model`).
3. **Sign in once:** run `claude` in `C:\work\rbstemsplus` and complete the browser sign-in (over Remote Desktop, use the browser on
   the machine, or the device code if it offers one). This uses the user's Claude plan.
4. **Start Remote Control** in that folder: `claude remote-control` (or the Claude desktop app's Settings > Claude Code > Remote
   Control, if the desktop app is installed on the machine and the feature is available). Keep it running: register it as a scheduled
   task *at startup* so it comes back after the machine is started again. Don't run it as a service under SYSTEM: it needs the
   signed-in account's credentials.
5. **In the cloud session:** Claude calls `list_computer_folders`. If the machine and the folder are listed, it calls
   `request_computer_folder`; the user approves the prompt. From then on Claude's tools run on the machine.
6. **First test:** Claude runs `scripts\gpu-runner-setup.ps1` through the attached session and shows the GPU adapter it found.

## What is risky about this
- **Account and plan:** Claude on the machine uses the user's sign-in and usage allowance.
- **Reach:** an attached session can run any command as the machine's user and can read anything on its disk. Keep nothing else
  there: no other credentials, no personal files. The AWS keys for start/stop live in GitHub secrets, not on the machine.
- **Cost:** the machine bills while Claude works. `gpu-power stop` at the end, and the idle shutdown as the net.
- **Pioneer's files:** `onnxruntime.dll` and `DirectML.dll` stay on the machine, not in the repository.

## Open question for the user
Is signing Claude in on the machine (using the plan's usage) acceptable? If not, option A (and D if needed) works without it.
