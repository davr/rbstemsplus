# A Windows GPU machine for benchmarks (self-hosted runner)

Why: the separation speed on a real GPU can only be measured on Windows with a DirectX 12 GPU. A rented machine registered as a
**self-hosted GitHub Actions runner** lets `windows-gpu.yml` run the benchmark with no one at the keyboard; results come back as
the job log, a summary table and the `bridge.log` files (artifact `gpu-bench-logs`). The machine only needs outbound HTTPS.

## One-time setup (about 30 minutes)
1. **A Windows machine with a GPU.** Recommended: AWS EC2 `g4dn.xlarge` (NVIDIA T4) with a Windows Server 2022 image, started
   only for tests and stopped after. Any DirectX 12 GPU works (NVIDIA, AMD, Intel). Install the GPU driver (for g4dn: AWS's
   "NVIDIA GRID/gaming driver"; the default Windows display driver is *not* enough: DirectML needs a real adapter).
2. Run `scripts/gpu-runner-setup.ps1` in PowerShell on it: it lists the display adapters and Windows' DirectX level and says what
   is missing.
3. **Register the runner.** GitHub, repository Settings > Actions > Runners > New self-hosted runner > Windows; run its commands.
   When it asks for labels, add `windows,gpu` (it adds `self-hosted` itself). Run it as a service (`./svc install`), with an
   account that is a member of Administrators: the benchmark locks a folder's permissions with `icacls`, as the installer does.
4. **Put rekordbox's runtime on the machine** (Pioneer's files, not in this repository): copy `onnxruntime.dll` and `DirectML.dll`
   from `C:\Program Files\rekordbox\rekordbox 7.2.14\` (or the version you use) to a folder, e.g. `C:\rbstems\pioneer`. Then set a
   repository variable: Settings > Secrets and variables > Actions > Variables > `RBSTEMS_PIONEER_DIR` = `C:\rbstems\pioneer`.
   The files stay on the machine; the workflow only reads them.

## Running it
Actions > windows-gpu > Run workflow (branch `claude/dml-model`, which has the workflow and the fixed-shape model), or ask me to
start it. It runs three times with the same chunk (485,100 samples, as rekordbox sends):

| run | what it measures |
|---|---|
| cpu-dynamic | our model on the CPU (`gpu=0`): the baseline (0.88x real time on the first test PC) |
| gpu-dynamic | our model on DirectML (`gpu=1`) |
| gpu-fixed | the fixed-shape model on DirectML (`gpu=1`, `hdemucs-dml.onnx` beside the model) |

The summary table shows each run's session line (says GPU or CPU) and its average speed, without the warm-up chunk.

## Cost
About $0.7/hour while the g4dn is running (to confirm on AWS's pricing page), a few dollars a month for its disk while stopped.
GitHub's own Windows GPU runners ($0.102/min) are an alternative but need a Team/Enterprise organization.

## Limits
This tests the bridge and the models against Pioneer's ONNX Runtime and DirectML. It does not run rekordbox (a licence and a
GUI). A final check with rekordbox on a real PC is still the end-to-end test.
