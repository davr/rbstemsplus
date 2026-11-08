# Starting and stopping the GPU machine automatically

> **Status: a design. Nothing in this file is built yet** (no workflow, script or AWS setting). It says what to build and what the
> user would set up once. The machine itself and its runner are in [GPU-RUNNER.md](GPU-RUNNER.md).

## Why
A `g4dn.xlarge` costs about $0.7/hour while it runs (to confirm on AWS's pricing page). The user doesn't want to remember to stop it,
and Claude should be able to start it when a test needs it.

## Claude can't call AWS directly
The Claude Code session that works on this repository runs in a cloud container whose network reaches GitHub (and hosts the user
allows) over HTTPS. Calling AWS's EC2 API needs each request **signed** with an AWS secret key. The credential form
(`request_credential`) stores a key that the network proxy injects as a header; Claude never receives the secret, so it can't sign
requests, and Claude shouldn't be given the key in the chat.

So AWS is called from **GitHub-hosted runners**, with the AWS keys stored as repository secrets that no one, Claude included, can read
back. Claude starts workflows with `workflow_dispatch` (as it does for CI today) and reads their logs.

## The design (two layers, so one failure can't leave the machine running)

### 1. Workflows
- **`windows-gpu.yml`** (the benchmark, extended): three jobs.
  1. `start` (on `ubuntu-latest`): `aws ec2 start-instances --instance-ids $GPU_INSTANCE_ID`, then `aws ec2 wait instance-running`.
  2. `bench` (`runs-on: [self-hosted, windows, gpu]`, `needs: start`): queues until the runner comes online (usually 1-3 minutes
     after the instance starts), then runs `scripts/gpu-bench.sh` as today.
  3. `stop` (on `ubuntu-latest`, `needs: [start, bench]`, **`if: always()`**): `aws ec2 stop-instances`. Skipped when the input
     `keep_running` is true (for a series of runs).
- **`gpu-power.yml`** (new, `workflow_dispatch`): input `action` = `start` | `stop` | `status`. For working interactively
  (several runs, or a session on the machine, see [GPU-INTERACTIVE.md](GPU-INTERACTIVE.md)): start, work, stop. It prints the state.

### 2. A safety net on the machine
`scripts/gpu-idle-shutdown.ps1` (new) installs a Windows **scheduled task**, run once through a workflow on the runner. Every 5
minutes it checks whether the machine is busy. It shuts Windows down (`Stop-Computer`) when it has been idle for **20 minutes** and
has been up for at least 20. Busy means:
- a runner job is running (a `Runner.Worker` process), or
- an interactive Claude session is active (a `claude` process, if GPU-INTERACTIVE.md's option B or C is set up), or
- someone is signed in over Remote Desktop with an active session.

The instance's **Shutdown behavior** is *Stop* (the default), so an OS shutdown stops the instance and ends compute billing. This
covers a failed `stop` job, a hung run, or a start nobody followed up. Worst case waste: about 20 minutes, around $0.25.

### 3. A cost alarm in AWS
An **AWS Budget** (Billing > Budgets) of, say, $15/month with an email alert at 80%.

## One-time setup by the user (about 10 minutes, after the machine exists)
1. **IAM user for the workflows**, allowed to start and stop only this instance. IAM > Users > Create user `rbstems-gpu-power`
   (no console access), attach this inline policy (replace the region, account number and instance id; `DescribeInstances` can't be
   limited to one resource):
   ```json
   {
     "Version": "2012-10-17",
     "Statement": [
       { "Effect": "Allow",
         "Action": ["ec2:StartInstances", "ec2:StopInstances"],
         "Resource": "arn:aws:ec2:us-east-1:ACCOUNT_ID:instance/i-XXXXXXXXXXXXXXXXX" },
       { "Effect": "Allow", "Action": "ec2:DescribeInstances", "Resource": "*" }
     ]
   }
   ```
   Then Security credentials > **Create access key** (use case: *Command Line Interface*). Keep the two values for the next step.
2. **GitHub:** Settings > Secrets and variables > Actions.
   - Secrets: `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`.
   - Variables: `AWS_REGION` (for example `us-east-1`), `GPU_INSTANCE_ID` (the `i-...` id from the EC2 console).
3. Keep *Fork pull request workflows from outside collaborators* on **Require approval** (GPU-RUNNER.md, step 12). Secrets aren't
   passed to workflows run from fork pull requests, but the setting keeps strangers' workflows off the runner too.
4. Create the **AWS Budget** alert (above).

A more secure alternative to long-lived keys is **GitHub OIDC**: an IAM identity provider for `token.actions.githubusercontent.com`
and an IAM role whose trust policy allows only `repo:davr/rbstemsplus` on the branches you name; the workflows then use
`aws-actions/configure-aws-credentials` with `role-to-assume` and no stored secret. It takes about 10 more minutes of console work.

## Using it
- One benchmark: Run workflow `windows-gpu` (leave `keep_running` off). It starts the machine, benchmarks and stops it. About
  3 minutes to start + 10 for the benchmark: roughly **$0.15**.
- A working session: `gpu-power` with `start`; do the work (benchmarks with `keep_running`, or an interactive session); `gpu-power`
  with `stop`. If it is forgotten, the idle shutdown stops it after 20 minutes.

## Failure modes
| Problem | What happens |
|---|---|
| The instance won't start (quota, no capacity in the zone) | `start` fails; `stop` still runs; Claude reports the error text |
| The runner never comes online (service stopped, expired token) | `bench` waits; cancel the run and `stop` (`always()`) stops the instance; the idle shutdown is the net |
| The `stop` job fails (an AWS error) | the idle shutdown stops the machine within about 25 minutes |
| The AWS keys leak | the worst they can do is start/stop this one machine and list instances; rotate the key in IAM |
| A run hangs | the job's `timeout-minutes` (60) ends it; then `stop` runs |

## Verification (when it is built)
- `gpu-power` with `status`, `start`, `status`, `stop` shows `stopped` → `running` → `stopped` in the logs.
- A full `windows-gpu` run: start, the runner comes online, the summary table appears, then `stop` runs and the state is `stopped`.
- Idle shutdown: start the machine with `gpu-power`, run nothing, confirm it stops by itself within about 25 minutes.
