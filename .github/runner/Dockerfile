# A self-hosted GitHub Actions runner with everything .github/workflows/windows.yml's linux job needs.
# Ubuntu 24.04: the same mingw-w64 (v11) and wine (9) as GitHub's own ubuntu-24.04 image.
FROM ubuntu:24.04
ARG RUNNER_VERSION=2.338.2
ENV DEBIAN_FRONTEND=noninteractive DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
RUN dpkg --add-architecture i386 && apt-get update && apt-get install -y --no-install-recommends \
      mingw-w64 wine64 wine wine32:i386 xz-utils unzip zip curl wget git python3 ffmpeg ca-certificates jq sudo \
      dotnet-sdk-8.0 build-essential libicu74 \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -s /bin/bash runner
USER runner
WORKDIR /home/runner/actions-runner
RUN curl -fsSL -o r.tgz https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz \
    && tar xzf r.tgz && rm r.tgz
COPY --chown=runner:runner entrypoint.sh /home/runner/entrypoint.sh
RUN chmod +x /home/runner/entrypoint.sh
ENTRYPOINT ["/home/runner/entrypoint.sh"]
