# A Windows GPU machine for benchmarks: creating it and registering it as a runner

Why: how fast Demucs separates on a real GPU can only be measured on Windows with a DirectX 12 GPU. A rented machine registered as a
**self-hosted GitHub Actions runner** lets `.github/workflows/windows-gpu.yml` run the benchmark with no one at the keyboard. The
results come back as the job log, a summary table and the `bridge.log` files (artifact `gpu-bench-logs`). The machine only needs
**outbound** HTTPS.

Related: [GPU-AUTOSTOP.md](GPU-AUTOSTOP.md) (starting and stopping it automatically, a design, not built yet) and
[GPU-INTERACTIVE.md](GPU-INTERACTIVE.md) (working on it interactively, and Claude on the machine).

> **From memory.** The AWS console names and the driver bucket below are written from memory and weren't checked against AWS's
> current pages. If a name differs, follow the console and tell Claude so the document can be fixed.

## What it costs
About **$0.7 per hour** while a `g4dn.xlarge` (one NVIDIA T4) runs Windows (to confirm on AWS's pricing page), and a few dollars a
month for the 100 GB disk while it is stopped. **Stopped instances don't bill for compute**, so stop it when you aren't testing
(see GPU-AUTOSTOP.md to automate that).

## Part 1: in AWS (about 20 minutes, plus a quota wait)
1. Sign in to AWS and pick a region in the top right, for example **us-east-1**. Use the same region everywhere below.
2. **Raise the GPU quota first.** A new account's limit for GPU instances is 0, so a launch fails without this.
   - Open **Service Quotas** > AWS services > **Amazon EC2**.
   - Find **"Running On-Demand G and VT instances"** and request **4** (a `g4dn.xlarge` has 4 vCPUs).
   - Approval often takes minutes, but can take hours. Carry on with step 3 meanwhile.
3. **Create an IAM role for the machine**, only used to download the NVIDIA driver from AWS's S3 bucket.
   - **IAM** > Roles > Create role > trusted entity **AWS service**, use case **EC2**.
   - Attach the policy **AmazonS3ReadOnlyAccess**. Name the role `rbstems-gpu-driver`.
4. **Launch the instance:** EC2 > Launch instance.
   - **Name:** `rbstems-gpu`
   - **Image:** *Microsoft Windows Server 2022 Base* (Windows, 64-bit)
   - **Instance type:** `g4dn.xlarge`
   - **Key pair:** create one (RSA, `.pem`) and keep the file: you need it to get the Administrator password.
   - **Network settings:** a security group that allows **RDP (TCP 3389) from "My IP" only**. The runner needs no inbound rule.
   - **Storage:** 100 GB gp3.
   - **Advanced details > IAM instance profile:** `rbstems-gpu-driver`. Leave *Shutdown behavior* on **Stop**.
5. When the instance is *running*: select it > **Connect** > **RDP client** > **Get password**. Upload the `.pem` file to decrypt
   it. Connect with Remote Desktop as `Administrator` to the public DNS name shown.

## Part 2: on the machine (about 15 minutes)
Open **PowerShell as administrator** (Start > right-click Windows PowerShell > Run as administrator).

6. **Install the NVIDIA driver.** AWS publishes drivers in the S3 bucket `ec2-windows-nvidia-drivers`, which the role in step 3 can read:
   ```powershell
   Install-PackageProvider -Name NuGet -Force
   Install-Module -Name AWS.Tools.S3 -Force
   $d = Get-S3Object -BucketName ec2-windows-nvidia-drivers -KeyPrefix latest -Region us-east-1 | Where-Object Key -like '*win10-win11*' | Select-Object -First 1
   Copy-S3Object -BucketName ec2-windows-nvidia-drivers -Key $d.Key -LocalFile C:\nvidia.exe -Region us-east-1
   Start-Process C:\nvidia.exe -ArgumentList '/s' -Wait
   Restart-Computer
   ```
   If the `-like` filter finds nothing, list the bucket (`Get-S3Object -BucketName ec2-windows-nvidia-drivers -KeyPrefix latest`) and
   pick the Windows installer by hand. After the restart, connect again. (The default "Microsoft Basic Display Adapter" is **not**
   enough: DirectML needs a real GPU adapter.)
7. **Check that the GPU is visible:**
   ```powershell
   Invoke-WebRequest https://raw.githubusercontent.com/davr/rbstemsplus/claude/dml-model/scripts/gpu-runner-setup.ps1 -OutFile C:\gpu-runner-setup.ps1
   powershell -ExecutionPolicy Bypass -File C:\gpu-runner-setup.ps1
   ```
   Expect "A hardware adapter is present: NVIDIA T4" and a DirectX feature level of 12_x. The script changes nothing.
8. **Copy rekordbox's runtime** (Pioneer's files, so they are **not** kept in this repository). On your own PC copy `onnxruntime.dll`
   and `DirectML.dll` from `C:\Program Files\rekordbox\rekordbox 7.2.14\` (or your version) and paste them, over the Remote Desktop
   window, into `C:\rbstems\pioneer` on the machine (create the folder).

## Part 3: GitHub, where the machine becomes a runner (about 5 minutes)
9. In the repository: **Settings > Actions > Runners > New self-hosted runner > Windows**. Copy only the **token** that follows
   `--token` in the commands it shows (it expires after an hour).
10. On the machine, run the bootstrap script with that token:
    ```powershell
    Invoke-WebRequest https://raw.githubusercontent.com/davr/rbstemsplus/claude/dml-model/scripts/gpu-runner-bootstrap.ps1 -OutFile C:\gpu-runner-bootstrap.ps1
    powershell -ExecutionPolicy Bypass -File C:\gpu-runner-bootstrap.ps1 -Token PASTE_TOKEN_HERE
    ```
    It downloads the newest runner, installs it as a Windows **service** with the labels `windows,gpu`, running as SYSTEM (so it is
    an administrator, which the benchmark needs to lock a folder's permissions with `icacls`, as the installer does). The runner then
    shows as **Idle** in Settings > Actions > Runners.
11. **Tell the workflow where Pioneer's files are:** Settings > Secrets and variables > Actions > **Variables** > New repository
    variable `RBSTEMS_PIONEER_DIR` = `C:\rbstems\pioneer`.
12. **Protect the machine:** Settings > Actions > General > *Fork pull request workflows from outside collaborators* > **Require
    approval for all outside collaborators**. The repository is a public fork; without this a stranger's pull request could ask to
    run code on your machine. (`windows-gpu.yml` only starts by hand with `workflow_dispatch`, and fork pull requests can't start it.)

## Running the benchmark
Actions > **windows-gpu** > Run workflow (on branch `claude/dml-model`: it has the workflow and the fixed-shape model), or ask Claude
to start it. The inputs: `model_tag` (the release whose `stemsplus-model.onnx` and `stemsplus-model-dml.onnx` to use, default
`windows-test-6dml2`) and `chunks` (default 4; the first is warm-up). It runs three times with the same chunk of 485,100 samples,
as rekordbox sends:

| run | what it measures |
|---|---|
| cpu-dynamic | our model on the CPU (`gpu=0`): the baseline (0.88x real time on the first test PC) |
| gpu-dynamic | our model on DirectML (`gpu=1`) |
| gpu-fixed | the fixed-shape model on DirectML (`gpu=1`, `hdemucs-dml.onnx` beside the model) |

The job summary has one row per run: the session line (it says GPU or CPU) and the average speed without the warm-up chunk.

## What this does not cover
- It doesn't run rekordbox (a licence and a GUI), only the bridge and the models against Pioneer's ONNX Runtime and DirectML.
  A final check with rekordbox on a real PC is still the end-to-end test.
- Fixing a problem on the machine (a missing driver, a stuck service) currently needs Remote Desktop. See GPU-INTERACTIVE.md for ways
  to let Claude do that itself.

## Troubleshooting
- **Launch fails with `VcpuLimitExceeded`:** the quota in step 2 isn't approved yet.
- **`gpu-runner-setup.ps1` shows only "Microsoft Basic Display Adapter":** the driver isn't installed (step 6), or the restart is missing.
- **The runner stays Offline:** check the service (`Get-Service actions.runner.*`); the registration token may have expired (get a new one).
- **The benchmark says the folder's permissions can't be set:** the runner must run as SYSTEM or an administrator account (step 10).
