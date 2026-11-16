FROM node:22.22.1-bookworm-slim@sha256:4f77a690f2f8946ab16fe1e791a3ac0667ae1c3575c3e4d0d4589e9ed5bfaf3d AS node
FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c
COPY --from=node /usr/local/bin/node /usr/local/bin/node
COPY --from=node /usr/local/lib/node_modules /usr/local/lib/node_modules
RUN ln -s /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm && timeout -k 5s 300s npm install --global @openai/codex@0.169.0 && codex --version
RUN useradd -m -s /bin/bash agent && mkdir -p /workspace /home/agent/.planar && chown -R agent:agent /workspace /home/agent
ENV HOME=/home/agent PLANAR_HOME=/home/agent/.planar
WORKDIR /workspace
USER agent
