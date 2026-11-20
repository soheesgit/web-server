#!/usr/bin/env python3
"""E2E용 가짜 백엔드 (Python 3 표준 라이브러리만).

사용법:
    fake_backend.py --port N --id NAME <mode> [arg]

기동하면 stdout에 "READY <port>" 한 줄을 쓴다. 연결마다 스레드 하나, keep-alive 지원.
모든 응답에 "X-Backend-Id: NAME"을 붙인다 (broken·drop·silent 제외).

모드:
    echo        200, 본문 = 받은 요청(요청 라인, 헤더 원본 순서, 본문)을 담은 JSON
                {"request_line", "method", "target", "version", "headers": [[name, value], ...],
                 "body_b64", "body_len", "body_sha256"}
    id          200, 본문 = NAME
    slow MS     MS 밀리초 뒤 id처럼 응답
    silent      요청을 읽고 아무 응답도 하지 않음 (연결 유지)
    broken      "HTTP/1.1 abc\\r\\n\\r\\n"을 보내고 닫음
    drop        요청을 읽고 아무것도 보내지 않고 닫음
    chunked     Transfer-Encoding: chunked로 CHUNKS 세 청크 응답
    close       Content-Length 없이 CLOSE_BODY를 보내고 닫음 (close-delimited)
    big N       N바이트 본문 (big_chunks), Content-Length 있음
    truncate N  Content-Length N을 알리고 본문 앞 N//2바이트만 보낸 뒤 닫음 (D-008)

모든 모드 공통 경로:
    GET /__stats        {"connections": n, "requests": n}. 통계 요청 자체와, 통계 요청만 보낸
                        연결은 세지 않는다.
    GET /__status/NNN   상태 NNN으로 응답, 본문 "status NNN\\n" (D-008). 요청 수에 포함.
"""
import argparse
import base64
import hashlib
import json
import socket
import sys
import threading
import time

CHUNKS = [b"alpha", b"beta", b"gamma"]
CLOSE_BODY = b"close-delimited body\n" * 100
_BLOCK = bytes(((i * 7 + 3) & 0xFF) for i in range(65536))

REASONS = {200: "OK", 201: "Created", 204: "No Content", 301: "Moved Permanently",
           400: "Bad Request", 403: "Forbidden", 404: "Not Found", 500: "Internal Server Error",
           502: "Bad Gateway", 503: "Service Unavailable"}


def big_chunks(n):
    """big·truncate 모드 본문을 64 KiB 조각으로 만든다 (테스트가 같은 함수로 SHA-256을 계산)."""
    left = n
    while left > 0:
        take = min(left, len(_BLOCK))
        yield _BLOCK[:take]
        left -= take


def big_sha256(n):
    h = hashlib.sha256()
    for c in big_chunks(n):
        h.update(c)
    return h.hexdigest()


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.connections = 0
        self.requests = 0

    def snapshot(self):
        with self.lock:
            return {"connections": self.connections, "requests": self.requests}


class Request:
    def __init__(self, line, headers, body):
        self.line = line
        parts = line.split(" ")
        self.method = parts[0] if parts else ""
        self.target = parts[1] if len(parts) > 1 else ""
        self.version = parts[2] if len(parts) > 2 else ""
        self.headers = headers
        self.body = body

    def header(self, name):
        for k, v in self.headers:
            if k.lower() == name.lower():
                return v
        return None

    def keep_alive(self):
        conn = (self.header("Connection") or "").lower()
        tokens = [t.strip() for t in conn.split(",")]
        if "close" in tokens:
            return False
        if self.version == "HTTP/1.1":
            return True
        return "keep-alive" in tokens


class Conn:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _fill(self):
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf += data

    def read_request(self):
        while True:
            for term in (b"\r\n\r\n", b"\n\n"):
                idx = self.buf.find(term)
                if idx >= 0:
                    head = self.buf[:idx]
                    self.buf = self.buf[idx + len(term):]
                    return self._finish(head)
            self._fill()

    def _finish(self, head):
        lines = head.replace(b"\r\n", b"\n").split(b"\n")
        line = lines[0].decode("latin-1")
        headers = []
        for raw in lines[1:]:
            if b":" not in raw:
                continue
            k, v = raw.split(b":", 1)
            headers.append((k.decode("latin-1"), v.strip().decode("latin-1")))
        req = Request(line, headers, b"")
        n = int(req.header("Content-Length") or 0)
        while len(self.buf) < n:
            self._fill()
        req.body = self.buf[:n]
        self.buf = self.buf[n:]
        return req


def response(status, headers, body, name, head_only=False, length=True):
    reason = REASONS.get(status, "Unknown")
    out = [f"HTTP/1.1 {status} {reason}", f"X-Backend-Id: {name}"]
    for k, v in headers:
        out.append(f"{k}: {v}")
    if length:
        out.append(f"Content-Length: {len(body)}")
    data = ("\r\n".join(out) + "\r\n\r\n").encode("latin-1")
    return data if head_only else data + body


def serve(sock, args, stats):
    conn = Conn(sock)
    counted = False
    name = args.id
    mode = args.mode
    try:
        while True:
            req = conn.read_request()
            head_only = req.method == "HEAD"
            if req.method == "GET" and req.target == "/__stats":
                body = json.dumps(stats.snapshot()).encode()
                sock.sendall(response(200, [("Content-Type", "application/json")], body, name))
                if not req.keep_alive():
                    return
                continue
            if not counted:
                counted = True
                with stats.lock:
                    stats.connections += 1
            with stats.lock:
                stats.requests += 1

            if req.target.startswith("/__status/"):
                code = int(req.target.rsplit("/", 1)[1])
                body = f"status {code}\n".encode()
                sock.sendall(response(code, [("Content-Type", "text/plain")], body, name, head_only))
            elif mode == "echo":
                body = json.dumps({
                    "request_line": req.line,
                    "method": req.method,
                    "target": req.target,
                    "version": req.version,
                    "headers": [[k, v] for k, v in req.headers],
                    "body_b64": base64.b64encode(req.body).decode(),
                    "body_len": len(req.body),
                    "body_sha256": hashlib.sha256(req.body).hexdigest(),
                }).encode()
                sock.sendall(response(200, [("Content-Type", "application/json")], body, name,
                                      head_only))
            elif mode in ("id", "slow"):
                if mode == "slow":
                    time.sleep(int(args.arg) / 1000.0)
                sock.sendall(response(200, [("Content-Type", "text/plain")], name.encode(), name,
                                      head_only))
            elif mode == "silent":
                while True:
                    if not sock.recv(65536):
                        return
            elif mode == "broken":
                sock.sendall(b"HTTP/1.1 abc\r\n\r\n")
                return
            elif mode == "drop":
                return
            elif mode == "chunked":
                head = (f"HTTP/1.1 200 OK\r\nX-Backend-Id: {name}\r\nContent-Type: text/plain\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n").encode()
                if head_only:
                    sock.sendall(head)
                else:
                    body = b"".join(b"%x\r\n%s\r\n" % (len(c), c) for c in CHUNKS) + b"0\r\n\r\n"
                    sock.sendall(head + body)
            elif mode == "close":
                head = (f"HTTP/1.1 200 OK\r\nX-Backend-Id: {name}\r\nContent-Type: text/plain\r\n"
                        "Connection: close\r\n\r\n").encode()
                sock.sendall(head if head_only else head + CLOSE_BODY)
                return
            elif mode in ("big", "truncate"):
                n = int(args.arg)
                head = (f"HTTP/1.1 200 OK\r\nX-Backend-Id: {name}\r\n"
                        f"Content-Type: application/octet-stream\r\nContent-Length: {n}\r\n\r\n"
                        ).encode()
                sock.sendall(head)
                if not head_only:
                    limit = n if mode == "big" else n // 2
                    sent = 0
                    for c in big_chunks(n):
                        if sent + len(c) > limit:
                            c = c[:limit - sent]
                        if c:
                            sock.sendall(c)
                        sent += len(c)
                        if sent >= limit:
                            break
                if mode == "truncate":
                    return
            else:
                raise SystemExit(f"unknown mode {mode}")
            if not req.keep_alive():
                return
    except (EOFError, ConnectionError, OSError):
        return
    finally:
        try:
            sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--id", required=True)
    ap.add_argument("mode", choices=["echo", "id", "slow", "silent", "broken", "drop", "chunked",
                                     "close", "big", "truncate"])
    ap.add_argument("arg", nargs="?")
    args = ap.parse_args()
    if args.mode in ("slow", "big", "truncate") and args.arg is None:
        ap.error(f"{args.mode} needs an argument")

    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", args.port))
    ls.listen(128)
    stats = Stats()
    print(f"READY {ls.getsockname()[1]}", flush=True)
    while True:
        try:
            s, _ = ls.accept()
        except OSError:
            continue
        threading.Thread(target=serve, args=(s, args, stats), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
