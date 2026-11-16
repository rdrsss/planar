FROM planar-t044-agent-preflight:latest
USER root
RUN apt-get update -qq && apt-get install -y --no-install-recommends git=1:2.39.5-0+deb12u3 && rm -rf /var/lib/apt/lists/*
