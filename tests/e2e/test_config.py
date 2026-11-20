"""기능 테스트: 설정·실행 (E-CF-01~04). 원래 표: 기획서 13.7절."""
import os
import socket
import tempfile

from harness import CONF, WWW, Server, expect, expect_eq, free_port, read_fixture, request, run_bin, test


def substituted(name, tmpdir):
    """fixture 설정을 {PORT}·{ROOT}만 치환해 같은 파일 이름으로 tmpdir에 쓴다."""
    with open(os.path.join(CONF, name), encoding="utf-8") as f:
        text = f.read().replace("{PORT}", str(free_port())).replace("{ROOT}", WWW)
    path = os.path.join(tmpdir, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


@test("E-CF-01", "--help prints usage and exits 0")
def _():
    rc, out, _ = run_bin(["--help"])
    expect_eq(rc, 0, "exit code")
    expect("Usage" in out, f"no usage on stdout: {out[:200]!r}")
    for opt in ("-p", "-r", "-c"):
        expect(opt in out, f"usage lacks {opt}")


@test("E-CF-02", "bad config files: exit 1 with file:line message")
def _():
    cases = [("bad_directive.conf", "bad_directive.conf:7"),
             ("bad_port.conf", "bad_port.conf:4"),
             ("bad_upstream_ref.conf", "nowhere")]
    with tempfile.TemporaryDirectory() as tmp:
        for name, needle in cases:
            rc, _, err = run_bin(["-c", substituted(name, tmp)])
            expect_eq(rc, 1, f"{name} exit code")
            expect(needle in err, f"{name}: stderr lacks {needle!r}: {err.strip()[:200]!r}")


@test("E-CF-03", "port in use: 'Address already in use', exit 2")
def _():
    blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    blocker.bind(("0.0.0.0", 0))
    blocker.listen(1)
    port = blocker.getsockname()[1]
    try:
        rc, _, err = run_bin(["-p", str(port), "-r", WWW], timeout=5)
        expect_eq(rc, 2, "exit code")
        expect("Address already in use" in err, f"stderr: {err.strip()[:200]!r}")
    finally:
        blocker.close()


@test("E-CF-04", "-p and -r from the command line")
def _():
    with Server() as s:  # Server()는 -p <빈 포트> -r tests/fixtures/www로 띄운다
        r = request(s.port, "GET", "/index.html")
        expect_eq(r.status, 200, "status")
        expect_eq(r.body, read_fixture("index.html"), "body")
