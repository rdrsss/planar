"""Exercise portable Linux host trust and certificate diagnostics in Docker.

Run through the host queue with prebuilt binaries and existing toolchain/runtime
images. This acceptance probe never mounts the operator's HOME or database.
"""

import argparse
import os
from pathlib import Path
import subprocess
import time


def run(args, **kwargs):
    """Print the exact invocation and return its captured process result."""
    args = list(map(str, args))
    print("+", subprocess.list2cmdline(args), flush=True)
    return subprocess.run(args, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--toolchain-image", required=True)
    parser.add_argument("--runtime-image", required=True)
    # Release gates pin linux/amd64 so an Apple silicon host runs x86_64 images.
    parser.add_argument("--platform")
    options = parser.parse_args()
    evidence = options.evidence.resolve()
    platform = ["--platform", options.platform] if options.platform else []
    # Each run needs fresh trust directories and retained, distinct evidence.
    evidence.mkdir(parents=True, exist_ok=False)
    fixtures = Path(__file__).resolve().parent / "fixtures/portable-tls"
    for name in ["client.sh", "server.py"]:
        (evidence / name).write_text((fixtures / name).read_text())
    generate = """set -eu
openssl req -x509 -newkey rsa:2048 -nodes -keyout /fixture/ca.key -out /fixture/ca.pem -days 1 -subj /CN=Planar-Test-CA
openssl req -newkey rsa:2048 -nodes -keyout /fixture/server.key -out /fixture/server.csr -subj /CN=localhost
printf 'subjectAltName=DNS:localhost\\n' > /fixture/extensions
openssl x509 -req -in /fixture/server.csr -CA /fixture/ca.pem -CAkey /fixture/ca.key -CAcreateserial -out /fixture/server.pem -days 1 -extfile /fixture/extensions
mkdir /fixture/debian-certs /fixture/no-certs
cp /fixture/ca.pem /fixture/debian-certs/test-ca.pem
openssl rehash /fixture/debian-certs
"""
    run(["docker", "run", "--rm", *platform, "-v", f"{evidence}:/fixture",
         options.toolchain_image, "sh", "-c", generate], check=True)
    server = f"planar-portable-tls-{os.getpid()}"
    run(["docker", "run", "-d", *platform, "--name", server, "-v",
         f"{evidence}:/fixture:ro", options.toolchain_image,
         "python3", "/fixture/server.py"], check=True)
    try:
        time.sleep(1)
        failed = False
        for mode in ["debian", "removed", "redhat"]:
            certs = evidence / ("debian-certs" if mode == "debian" else "no-certs")
            args = ["docker", "run", "--rm", *platform, "--network", f"container:{server}",
                    "-v", f"{options.bin_dir.resolve()}:/opt/planar:ro",
                    "-v", f"{evidence}:/fixture:ro",
                    "-v", f"{certs}:/etc/ssl/certs:ro"]
            if mode == "redhat":
                args += ["-v", f"{evidence / 'ca.pem'}:/etc/pki/tls/certs/ca-bundle.crt:ro"]
            args += [options.runtime_image, "sh", "/fixture/client.sh",
                     "untrusted" if mode == "removed" else "trusted"]
            result = run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(f"MODE={mode} exit={result.returncode}\n{result.stdout}", flush=True)
            (evidence / f"tls-{mode}.log").write_text(
                result.stdout + f"\nprobe_exit={result.returncode}\n")
            failed |= result.returncode != 0
        log = run(["docker", "logs", server], stdout=subprocess.PIPE,
                  stderr=subprocess.STDOUT, text=True, check=True)
        print(log.stdout, flush=True)
        (evidence / "tls-server.log").write_text(log.stdout)
        failed |= "TLS_HANDSHAKE_REJECTION=TLSV1_ALERT_UNKNOWN_CA" not in log.stdout
        return int(failed)
    finally:
        run(["docker", "rm", "-f", server], check=True)


if __name__ == "__main__":
    raise SystemExit(main())
