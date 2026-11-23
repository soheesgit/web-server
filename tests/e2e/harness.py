"""E2E 테스트 하네스 (Python 3 표준 라이브러리만).

서버·가짜 백엔드 프로세스 관리, 원시 소켓 HTTP 클라이언트, 서버와 독립적인 응답 파서,
fixture 준비를 제공한다. 규칙과 사용법: docs/TESTING.md

응답 파서는 http.client를 쓰지 않는다 (비정상 응답을 감추거나 고쳐 버리기 때문).
"""
import contextlib
import hashlib
import os
import random
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TESTS = os.path.join(REPO, "tests")
FIXTURES = os.path.join(TESTS, "fixtures")
WWW = os.path.join(FIXTURES, "www")
CGI = os.path.join(FIXTURES, "cgi-bin")
CONF = os.path.join(FIXTURES, "conf")
FAKE_BACKEND = os.path.join(TESTS, "fakes", "fake_backend.py")

sys.path.insert(0, os.path.join(TESTS, "fakes"))
import fake_backend  # noqa: E402  (big_chunks, CHUNKS, CLOSE_BODY를 테스트와 공유)

BIN = None          # run_e2e.py가 --bin으로 채움
REGISTRY = []       # (id, name, fn, optional)
_LIVE = []          # 아직 살아 있을 수 있는 Popen (타임아웃 후 정리용)

BIG_SIZE = 100 * 1024 * 1024
BIG_SEED = 20261120
BIG_SHA256 = None   # prepare_fixtures가 계산


class Skip(Exception):
    """테스트를 건너뛴다 (예: root로 실행)."""


class ServerStartError(Exception):
    """서버가 포트를 열기 전에 종료했거나 5초 안에 열지 못했다."""


def test(id, name, optional=False):
    """테스트 등록 데코레이터."""
    def deco(fn):
        REGISTRY.append((id, name, fn, optional))
        return fn
    return deco


def is_root():
    return hasattr(os, "geteuid") and os.geteuid() == 0


def skip_if_root():
    if is_root():
        raise Skip("root")


def kill_all():
    """등록된 모든 하위 프로세스를 강제로 끝낸다 (타임아웃·예외 뒤 정리)."""
    while _LIVE:
        p = _LIVE.pop()
        if p.poll() is None:
            try:
                p.kill()
                p.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass


def free_port():
    """127.0.0.1:0에 bind해 빈 포트를 얻고 닫는다."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _can_connect(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return True
    except OSError:
        return False


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_fixture(rel):
    with open(os.path.join(WWW, rel), "rb") as f:
        return f.read()


def expect(cond, msg):
    """조건이 거짓이면 AssertionError(msg). 실패 사유를 -v 출력에 남기기 위한 도우미."""
    if not cond:
        raise AssertionError(msg)


def expect_eq(actual, expected, what):
    if actual != expected:
        raise AssertionError(f"{what}: expected {expected!r}, got {actual!r}")


def within(elapsed, target, what):
    """타이밍 허용 오차: ±50% 또는 최소 500ms."""
    tol = max(target * 0.5, 0.5)
    if abs(elapsed - target) > tol:
        raise AssertionError(f"{what}: expected {target:.2f}s ± {tol:.2f}s, got {elapsed:.2f}s")


# ── 프로세스 정보 (ps는 POSIX) ───────────────────────────────────────────────

def _ps_rows():
    out = subprocess.run(["ps", "-A", "-o", "pid=", "-o", "ppid=", "-o", "stat="],
                         capture_output=True, text=True, check=False).stdout
    rows = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            rows.append((int(parts[0]), int(parts[1]), parts[2]))
    return rows


def children_of(pid):
    """[(pid, stat)] 직계 자식 목록."""
    return [(p, st) for p, pp, st in _ps_rows() if pp == pid]


def rss_kb(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True,
                         check=False).stdout.strip()
    return int(out) if out.isdigit() else None


def fd_count(pid):
    """열린 fd 수. Linux(/proc)에서만 알 수 있고 그 밖에는 None."""
    path = f"/proc/{pid}/fd"
    try:
        return len(os.listdir(path))
    except OSError:
        return None


# ── 서버 ──────────────────────────────────────────────────────────────────────

class Server:
    """httpd를 띄우는 컨텍스트 매니저.

    conf가 있으면 tests/fixtures/conf/<conf>(또는 절대 경로)를 읽어 {PORT}, {ROOT}, {CGI}와
    subst의 키({BACKEND1} 등)를 치환하고, extra 줄을 덧붙여 같은 파일 이름으로 임시 디렉토리에
    쓴 뒤 -c로 넘긴다. conf가 없으면 -p <port> -r <root>. args는 뒤에 덧붙일 CLI 인자.

    끝날 때(예외가 없을 때) 공통 판정을 한다: 서버가 살아 있는가, 좀비 자식이 없는가,
    (Linux) 열린 fd 수가 3초 안에 시작 때 + fd_slack 이하로 돌아오는가. 그다음 SIGTERM,
    5초 뒤에도 살아 있으면 SIGKILL.
    """

    def __init__(self, args=None, conf=None, subst=None, root=WWW, extra=(), fd_slack=0,
                 check_clean=True):
        self.args = list(args or [])
        self.conf = conf
        self.subst = dict(subst or {})
        self.root = root
        self.extra = list(extra)
        self.fd_slack = fd_slack
        self.check_clean = check_clean
        self.port = None
        self.proc = None
        self.conf_path = None
        self._tmp = None
        self._baseline_fds = None

    def __enter__(self):
        if BIN is None:
            raise RuntimeError("harness.BIN이 설정되지 않음 (run_e2e.py --bin)")
        self._tmp = tempfile.TemporaryDirectory(prefix="httpd-e2e-")
        self.port = free_port()
        if self.conf is not None:
            src = self.conf if os.path.isabs(self.conf) else os.path.join(CONF, self.conf)
            with open(src, encoding="utf-8") as f:
                text = f.read()
            mapping = {"PORT": str(self.port), "ROOT": self.root, "CGI": CGI}
            mapping.update(self.subst)
            for k, v in mapping.items():
                text = text.replace("{" + k + "}", str(v))
            if self.extra:
                text = text.rstrip("\n") + "\n" + "\n".join(self.extra) + "\n"
            self.conf_path = os.path.join(self._tmp.name, os.path.basename(src))
            with open(self.conf_path, "w", encoding="utf-8") as f:
                f.write(text)
            cmd = [BIN, "-c", self.conf_path] + self.args
        else:
            cmd = [BIN, "-p", str(self.port), "-r", self.root] + self.args
        # HTTPD_ENGINE=loop 이면 같은 E2E를 v2 이벤트 루프 엔진으로 돌린다.
        if os.environ.get("HTTPD_ENGINE"):
            cmd += ["-e", os.environ["HTTPD_ENGINE"]]
        self._out = open(os.path.join(self._tmp.name, "stdout"), "wb")
        self._err = open(os.path.join(self._tmp.name, "stderr"), "wb")
        self.proc = subprocess.Popen(cmd, cwd=REPO, stdout=self._out, stderr=self._err,
                                     stdin=subprocess.DEVNULL)
        _LIVE.append(self.proc)
        deadline = time.monotonic() + 5
        while True:
            rc = self.proc.poll()
            if rc is not None:
                self._close_files()
                err = self.stderr_text().strip()
                self._cleanup_tmp()
                raise ServerStartError(f"server exited with {rc} before listening: {err}")
            if _can_connect(self.port):
                break
            if time.monotonic() > deadline:
                self._stop()
                raise ServerStartError("server did not listen within 5s")
            time.sleep(0.05)
        time.sleep(0.1)
        self._baseline_fds = fd_count(self.proc.pid)
        return self

    def stderr_text(self):
        try:
            with open(os.path.join(self._tmp.name, "stderr"), encoding="utf-8",
                      errors="replace") as f:
                return f.read()
        except (OSError, AttributeError):
            return ""

    def rss_kb(self):
        return rss_kb(self.proc.pid)

    def children(self):
        return children_of(self.proc.pid)

    def wait_no_children(self, timeout=3.0):
        """timeout 안에 자식 프로세스가 하나도 없어지면 True."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self.children():
                return True
            time.sleep(0.1)
        return not self.children()

    def verify_clean(self):
        expect(self.proc.poll() is None, f"server died (exit {self.proc.poll()})")
        zombies = [p for p, st in self.children() if st.startswith("Z")]
        deadline = time.monotonic() + 3
        while zombies and time.monotonic() < deadline:
            time.sleep(0.1)
            zombies = [p for p, st in self.children() if st.startswith("Z")]
        expect(not zombies, f"zombie children: {zombies}")
        if self._baseline_fds is not None:
            limit = self._baseline_fds + self.fd_slack
            now = fd_count(self.proc.pid)
            while now is not None and now > limit and time.monotonic() < deadline + 3:
                time.sleep(0.1)
                now = fd_count(self.proc.pid)
            expect(now is None or now <= limit,
                   f"fd leak: {now} open fds, baseline {self._baseline_fds} + slack {self.fd_slack}")

    def __exit__(self, exc_type, exc, tb):
        try:
            if exc_type is None and self.check_clean:
                self.verify_clean()
        finally:
            self._stop()
        return False

    def _stop(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGTERM)
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
            except OSError:
                pass
        if self.proc in _LIVE:
            _LIVE.remove(self.proc)
        self._close_files()
        self._cleanup_tmp()

    def _close_files(self):
        for f in (getattr(self, "_out", None), getattr(self, "_err", None)):
            if f is not None and not f.closed:
                f.close()

    def _cleanup_tmp(self):
        # stderr_text()를 위해 tmp는 프로세스 종료 뒤에만 지운다
        if self._tmp is not None and (self.proc is None or self.proc.poll() is not None):
            self._tmp.cleanup()
            self._tmp = None


def run_bin(args, timeout=10):
    """서버 바이너리를 직접 실행해 (종료 코드, stdout, stderr)를 돌려준다."""
    p = subprocess.run([BIN] + list(args), cwd=REPO, capture_output=True, timeout=timeout,
                       stdin=subprocess.DEVNULL)
    return p.returncode, p.stdout.decode(errors="replace"), p.stderr.decode(errors="replace")


# ── 가짜 백엔드 ───────────────────────────────────────────────────────────────

class Backend:
    """fake_backend.py를 빈 포트(또는 지정 포트)로 띄우는 컨텍스트 매니저."""

    def __init__(self, mode, *args, name=None, port=None):
        self.mode = mode
        self.mode_args = [str(a) for a in args]
        self.port = port
        self.name = name
        self.proc = None

    @property
    def addr(self):
        return f"127.0.0.1:{self.port}"

    def start(self):
        if self.port is None:
            self.port = free_port()
        if self.name is None:
            self.name = f"b{self.port}"
        cmd = [sys.executable, FAKE_BACKEND, "--port", str(self.port), "--id", self.name,
               self.mode] + self.mode_args
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     stdin=subprocess.DEVNULL)
        _LIVE.append(self.proc)
        r, _, _ = select.select([self.proc.stdout], [], [], 5)
        line = self.proc.stdout.readline().decode() if r else ""
        if not line.startswith("READY"):
            self.stop()
            raise RuntimeError(f"fake_backend did not start: {line!r}")
        return self

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=5)
        if self.proc in _LIVE:
            _LIVE.remove(self.proc)

    def stats(self):
        """{"connections": n, "requests": n}"""
        import json
        r = request(self.port, "GET", "/__stats", headers=[("Connection", "close")])
        return json.loads(r.body)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()
        return False


# ── HTTP 클라이언트와 응답 파서 ───────────────────────────────────────────────

class Response:
    def __init__(self, version, status, reason, headers, body, raw_head, chunked):
        self.version = version      # "HTTP/1.1"
        self.status = status        # int
        self.reason = reason        # str
        self.headers = headers      # [(name, value)] 원래 순서, 중복 보존
        self.body = body            # bytes (chunked면 디코딩 후)
        self.raw_head = raw_head    # bytes (상태 줄부터 빈 줄까지)
        self.chunked = chunked      # Transfer-Encoding: chunked였는가

    def header(self, name):
        """첫 번째 값 (대소문자 무시), 없으면 None."""
        for k, v in self.headers:
            if k.lower() == name.lower():
                return v
        return None

    def header_all(self, name):
        return [v for k, v in self.headers if k.lower() == name.lower()]

    def __repr__(self):
        return f"<Response {self.status} {self.reason} {len(self.body)}B>"


class _Reader:
    """소켓(또는 바이트) 위의 버퍼. EOF·타임아웃을 구분한다."""

    def __init__(self, sock=None, data=b"", timeout=5.0):
        self.sock = sock
        self.buf = bytearray(data)
        self.timeout = timeout
        self.eof = sock is None

    def _more(self):
        if self.eof:
            return False
        self.sock.settimeout(self.timeout)
        try:
            data = self.sock.recv(1 << 16)
        except socket.timeout:
            raise TimeoutError(f"no data within {self.timeout}s")
        except ConnectionResetError:
            data = b""
        if not data:
            self.eof = True
            return False
        self.buf += data
        return True

    def read_head(self):
        while True:
            for term in (b"\r\n\r\n", b"\n\n"):
                i = self.buf.find(term)
                if i >= 0:
                    head = bytes(self.buf[:i + len(term)])
                    del self.buf[:i + len(term)]
                    return head
            if not self._more():
                raise EOFError(f"connection closed before end of head ({len(self.buf)} bytes)")

    def read_exact(self, n):
        while len(self.buf) < n:
            if not self._more():
                raise EOFError(f"connection closed after {len(self.buf)} of {n} body bytes")
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def read_line(self):
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line = bytes(self.buf[:i + 1])
                del self.buf[:i + 1]
                return line
            if not self._more():
                raise EOFError("connection closed inside chunked body")

    def read_to_eof(self):
        while self._more():
            pass
        out = bytes(self.buf)
        self.buf.clear()
        return out


def _parse_head(head):
    lines = head.replace(b"\r\n", b"\n").rstrip(b"\n").split(b"\n")
    status_line = lines[0].decode("latin-1")
    parts = status_line.split(" ", 2)
    if len(parts) < 2 or not parts[0].startswith("HTTP/") or not parts[1].isdigit():
        raise ValueError(f"bad status line {status_line!r}")
    headers = []
    for raw in lines[1:]:
        if b":" not in raw:
            raise ValueError(f"bad header line {raw!r}")
        k, v = raw.split(b":", 1)
        headers.append((k.decode("latin-1"), v.strip().decode("latin-1")))
    return parts[0], int(parts[1]), (parts[2] if len(parts) > 2 else ""), headers


def _read_response(rd, method):
    head = rd.read_head()
    version, status, reason, headers = _parse_head(head)
    get = lambda n: [v for k, v in headers if k.lower() == n]  # noqa: E731
    te = ",".join(get("transfer-encoding")).lower()
    chunked = te.split(",")[-1].strip() == "chunked" if te else False
    if method == "HEAD" or 100 <= status < 200 or status in (204, 304):
        body = b""
    elif chunked:
        parts = []
        while True:
            size_line = rd.read_line().strip()
            size = int(size_line.split(b";")[0], 16)
            if size == 0:
                while rd.read_line().strip():   # 트레일러
                    pass
                break
            parts.append(rd.read_exact(size))
            rd.read_line()
        body = b"".join(parts)
    elif get("content-length"):
        body = rd.read_exact(int(get("content-length")[0]))
    else:
        body = rd.read_to_eof()
    return Response(version, status, reason, headers, body, head, chunked)


def parse_response(data, method="GET"):
    """바이트에서 응답 하나를 파싱한다."""
    return _read_response(_Reader(data=data), method)


def parse_responses(data, methods):
    """바이트에서 응답 여러 개를 차례로 파싱한다."""
    rd = _Reader(data=data)
    return [_read_response(rd, m) for m in methods]


class Conn:
    """keep-alive·파이프라이닝용 연결."""

    def __init__(self, port, timeout=5.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.rd = _Reader(self.sock, timeout=timeout)
        self.port = port

    def send(self, data):
        self.sock.sendall(data)

    def read_response(self, method="GET"):
        return _read_response(self.rd, method)

    def is_closed(self, timeout=1.0):
        """timeout 안에 서버가 닫으면(EOF·RST) True. 데이터가 오거나 시간이 지나면 False."""
        if self.rd.buf:
            return False
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(1)
        except socket.timeout:
            return False
        except (ConnectionResetError, BrokenPipeError):
            return True
        if data:
            self.rd.buf += data
            return False
        return True

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def build_request(method, target, headers=(), body=b"", version="1.1", port=None):
    headers = list(headers)
    names = {k.lower() for k, _ in headers}
    if version == "1.1" and "host" not in names:
        headers.insert(0, ("Host", f"127.0.0.1:{port}" if port else "localhost"))
    if body and "content-length" not in names:
        headers.append(("Content-Length", str(len(body))))
    lines = [f"{method} {target} HTTP/{version}"] + [f"{k}: {v}" for k, v in headers]
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


def request(port, method, target, headers=(), body=b"", version="1.1", timeout=10.0):
    """요청 하나를 보내고 응답 하나를 파싱해 돌려준다."""
    with Conn(port, timeout=timeout) as c:
        c.send(build_request(method, target, headers, body, version, port))
        return c.read_response(method)


def raw(port, data, timeout=5.0, half_close=False):
    """바이트를 보내고 서버가 닫거나 timeout이 될 때까지 받은 바이트를 돌려준다.

    서버가 중간에 응답하면(읽을 데이터가 생기면) 남은 송신을 멈춘다. 송신 중 EPIPE·RST는 무시한다.
    """
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        view = memoryview(data)
        try:
            while view:
                r, _, _ = select.select([s], [], [], 0)
                if r:
                    break
                n = s.send(view[:16384])
                view = view[n:]
            if half_close:
                s.shutdown(socket.SHUT_WR)
        except (BrokenPipeError, ConnectionResetError):
            pass
        out = bytearray()
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            s.settimeout(left)
            try:
                chunk = s.recv(1 << 16)
            except socket.timeout:
                break
            except ConnectionResetError:
                break
            if not chunk:
                break
            out += chunk
        return bytes(out)
    finally:
        s.close()


@contextlib.contextmanager
def backends(*specs):
    """Backend 여러 개를 띄운다. spec은 (mode, args..., ) 튜플 또는 Backend."""
    with contextlib.ExitStack() as st:
        out = []
        for sp in specs:
            b = sp if isinstance(sp, Backend) else Backend(*sp)
            out.append(st.enter_context(b))
        yield out


# ── fixture ──────────────────────────────────────────────────────────────────

def _big_chunks():
    rng = random.Random(BIG_SEED)
    left = BIG_SIZE
    while left > 0:
        n = min(left, 1 << 20)
        yield rng.getrandbits(8 * n).to_bytes(n, "little")
        left -= n


def prepare_fixtures():
    """생성 fixture를 멱등하게 만든다 (커밋하지 않음, .gitignore에 있음)."""
    global BIG_SHA256
    big = os.path.join(WWW, "big.bin")
    h = hashlib.sha256()
    for c in _big_chunks():
        h.update(c)
    BIG_SHA256 = h.hexdigest()
    if not (os.path.isfile(big) and os.path.getsize(big) == BIG_SIZE
            and sha256_file(big) == BIG_SHA256):
        tmp = big + ".tmp"
        with open(tmp, "wb") as f:
            for c in _big_chunks():
                f.write(c)
        os.replace(tmp, big)

    secret = os.path.join(WWW, "secret.txt")
    if os.path.lexists(secret):
        os.chmod(secret, 0o600)
    with open(secret, "w") as f:
        f.write("secret\n")
    os.chmod(secret, 0o000)

    escape = os.path.join(WWW, "escape")
    target = os.path.join("..", "conf", "basic.conf")
    if not (os.path.islink(escape) and os.readlink(escape) == target):
        if os.path.lexists(escape):
            os.remove(escape)
        os.symlink(target, escape)

    fifo = os.path.join(WWW, "fifo")
    if os.path.lexists(fifo) and not stat_is_fifo(fifo):
        os.remove(fifo)
    if not os.path.lexists(fifo):
        os.mkfifo(fifo)


def stat_is_fifo(path):
    import stat
    return stat.S_ISFIFO(os.lstat(path).st_mode)


# ── 큰 본문 ───────────────────────────────────────────────────────────────────

def set_linger_rst(sock):
    """close 시 RST를 보내도록 SO_LINGER {1, 0}을 건다."""
    import struct
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))


class _RssSampler:
    """서버 RSS를 0.1초마다 재서 최대 증가량(KB)을 기록한다."""

    def __init__(self, pid):
        import threading
        self.pid = pid
        self.base = rss_kb(pid) if pid else None
        self.peak = self.base
        self._stop = threading.Event()
        self._th = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.wait(0.1):
            v = rss_kb(self.pid)
            if v is not None and (self.peak is None or v > self.peak):
                self.peak = v

    def __enter__(self):
        if self.pid:
            self._th.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._th.is_alive():
            self._th.join(timeout=1)
        return False

    @property
    def growth_kb(self):
        if self.base is None or self.peak is None:
            return 0
        return self.peak - self.base


def stream_get(port, target, headers=(), sample_pid=None, limit=None, timeout=20.0):
    """GET 하나를 보내고 본문을 메모리에 모으지 않고 SHA-256과 길이만 센다.

    Content-Length 또는 연결 종료로 끝나는 본문을 읽는다. limit이 있으면 그 바이트만큼 읽고
    RST로 끊는다. sample_pid가 있으면 그 프로세스의 RSS 최대 증가량을 잰다.
    반환: dict(status, headers, nbytes, sha256, rss_growth_kb)
    """
    c = Conn(port, timeout=timeout)
    try:
        c.send(build_request("GET", target, headers, port=port))
        head = c.rd.read_head()
        _, status, _, hdrs = _parse_head(head)
        cl = [v for k, v in hdrs if k.lower() == "content-length"]
        want = int(cl[0]) if cl else None
        h = hashlib.sha256()
        n = 0
        with _RssSampler(sample_pid) as rs:
            if c.rd.buf:
                chunk = bytes(c.rd.buf)
                c.rd.buf.clear()
                h.update(chunk)
                n += len(chunk)
            while (want is None or n < want) and (limit is None or n < limit):
                c.sock.settimeout(timeout)
                try:
                    data = c.sock.recv(1 << 18)
                except ConnectionResetError:
                    break
                if not data:
                    break
                h.update(data)
                n += len(data)
        if limit is not None:
            set_linger_rst(c.sock)
        return {"status": status, "headers": hdrs, "nbytes": n, "sha256": h.hexdigest(),
                "rss_growth_kb": rs.growth_kb}
    finally:
        c.close()
