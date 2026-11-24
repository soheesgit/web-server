#!/usr/bin/env python3
"""로컬 v1 서버의 큐 포화, 종료 유예, FD 고갈 뒤 복구를 검증한다."""

import argparse
from contextlib import ExitStack, contextmanager
from pathlib import Path
import resource
import select
import signal
import socket
import subprocess
import sys
from tempfile import TemporaryDirectory
import time

from proxy_bench import ROOT, resources

sys.path.insert(0, str(ROOT / "tests/e2e"))
from harness import Conn, WWW, build_request, free_port, request


@contextmanager
def server(binary, settings, fd_limit=None):
    def apply_limit():
        resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))

    port = free_port()
    with TemporaryDirectory(prefix="httpd-worker-") as directory:
        cfg = Path(directory) / "worker.conf"
        cfg.write_text(
            f"listen 127.0.0.1:{port}\nroot {WWW}\nengine pool\n"
            + "\n".join(settings) + "\n",
            encoding="utf-8",
        )
        log = Path(directory) / "httpd.log"
        with log.open("w") as stderr:
            child = subprocess.Popen(
                [str(binary), "-c", str(cfg)], stdout=subprocess.DEVNULL, stderr=stderr,
                preexec_fn=apply_limit if fd_limit is not None else None,
            )
        try:
            deadline = time.monotonic() + 5
            while child.poll() is None:
                try:
                    ready = request(port, "GET", "/index.html",
                                    headers=[("Connection", "close")], timeout=.2)
                    if ready.status == 200 and ready.body:
                        break
                except (OSError, EOFError):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError(log.read_text())
                time.sleep(.025)
            else:
                raise RuntimeError(log.read_text())
            yield child, port
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


@contextmanager
def active_and_queued(port):
    data = build_request("GET", "/index.html")
    with Conn(port, timeout=2) as active:
        active.send(data)
        response = active.read_response()
        assert response.status == 200 and response.body and response.header("Connection") != "close"
        with Conn(port, timeout=2) as queued:
            queued.send(data)
            assert not select.select([queued.sock], [], [], .2)[0], "queued request ran early"
            yield active, queued


def queue_overflow(binary):
    with server(binary, ["workers 1", "queue_len 1"]) as (_, port):
        with active_and_queued(port) as (active, queued):
            refused = request(port, "GET", "/index.html", timeout=2)
            assert refused.status == 503 and refused.header("Connection") == "close"
            active.close()
            assert queued.read_response().status == 200


def signal_shutdown(binary):
    settings = ["workers 1", "queue_len 1", "shutdown_grace_ms 120"]
    for signum in (signal.SIGINT, signal.SIGTERM):
        with server(binary, settings) as (child, port):
            with active_and_queued(port) as (active, queued):
                started = time.monotonic()
                child.send_signal(signum)
                assert child.wait(timeout=3) == 0
                assert time.monotonic() - started >= .1, "shutdown skipped grace period"
                assert active.is_closed(.5) and queued.is_closed(.5)


def fd_recovery(binary):
    fd_limit = 32
    settings = ["workers 32", "queue_len 32", "backlog 64", "header_timeout_ms 10000"]
    with server(binary, settings, fd_limit) as (child, port), ExitStack() as sockets:
        held = [sockets.enter_context(socket.create_connection(("127.0.0.1", port), timeout=1))
                for _ in range(fd_limit)]
        deadline = time.monotonic() + 3
        while resources(child.pid)[0] != fd_limit:
            assert child.poll() is None and time.monotonic() < deadline, "FD limit not observed"
            time.sleep(.025)
        for peer in held[:12]:
            peer.close()
        assert request(port, "GET", "/index.html", timeout=3).status == 200


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/httpd")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    for check in (queue_overflow, signal_shutdown, fd_recovery):
        check(binary)
        print(f"PASS {check.__name__}", flush=True)


if __name__ == "__main__":
    main()
