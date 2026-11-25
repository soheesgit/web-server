"""발표 경로에서 화면용 관측값과 실제 HTTP 동작이 일치하는지 검사한다."""
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from uuid import uuid4

from run import HOST, ROOT, free_port, stop, wait_for_port


class DemoTest(unittest.TestCase):
    def setUp(self):
        self.port = free_port()
        command = [sys.executable, str(ROOT / "demo/run.py"), "--port", str(self.port)]
        binary = os.environ.get("DEMO_TEST_BINARY")
        if binary:
            command += ["--binary", binary]
        self.process_log = tempfile.TemporaryFile()
        self.addCleanup(self.process_log.close)
        self.demo = subprocess.Popen(command, stdin=subprocess.PIPE, text=True,
                                     stdout=subprocess.DEVNULL, stderr=self.process_log)
        self.addCleanup(self.check_process_log)
        self.addCleanup(self.demo.stdin.close)
        wait_for_port(self.port, self.demo)

    def check_process_log(self):
        stop(self.demo)
        self.process_log.seek(0)
        output = self.process_log.read().decode(errors="replace")
        self.assertNotIn("Traceback", output)

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection(HOST, self.port, timeout=12)
        try:
            conn.request(method, path, body, headers or {})
            with conn.getresponse() as response:
                return response.status, response.headers, response.read()
        finally:
            conn.close()

    def state(self):
        status, _, body = self.request("GET", "/__demo/state")
        self.assertEqual(status, 200)
        return json.loads(body)

    def wait_until(self, predicate, label, timeout=6):
        cutoff = time.monotonic() + timeout
        while time.monotonic() < cutoff:
            result = predicate(self.state())
            if result:
                return result
            time.sleep(0.04)
        self.fail(label)

    def question(self, session, prefix=0, *, cache_session=None):
        status, headers, body = self.request(
            "POST", "/v1/chat/completions", json.dumps({
                "session": cache_session or session, "input_tokens": prefix + 400,
                "prefix_tokens": prefix, "max_tokens": 36, "stream": True,
            }), {"Content-Type": "application/json", "X-Session-Id": session},
        )
        self.assertEqual(status, 200, body)
        self.assertIn(b"data: [DONE]", body)
        data = [json.loads(line[6:]) for line in body.splitlines()
                if line.startswith(b"data: {")]
        final = next(event for event in data if event.get("done"))
        self.assertEqual(final["backend"], headers["X-Backend-Id"])
        return final

    def test_sticky_assignment_cache_and_recovery(self):
        status, _, page = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("학생 추가".encode(), page)

        session = uuid4().hex
        first = self.question(session)
        preferred = first["backend"]
        self.assertEqual(first["usage"]["cached_tokens"], 0)
        follow_up = self.question(session, prefix=400)
        self.assertEqual((follow_up["backend"], follow_up["usage"]["cached_tokens"]), (preferred, 400))
        cold_follow_up = self.question(session, prefix=400, cache_session=uuid4().hex)
        self.assertEqual(cold_follow_up["backend"], follow_up["backend"])
        self.assertEqual(cold_follow_up["usage"]["input_tokens"], follow_up["usage"]["input_tokens"])
        self.assertEqual(cold_follow_up["usage"]["cached_tokens"], 0)
        self.assertLess(follow_up["timing"]["prefill_ms"], cold_follow_up["timing"]["prefill_ms"])

        events = self.wait_until(lambda state: [event for event in state["events"]
                                                 if event["session"] == session
                                                 and event["type"] == "attempt"],
                                 "라우팅 이벤트가 없습니다")
        self.assertEqual(events[0]["backend"], preferred)
        self.assertEqual(events[0]["choices"][0]["policy"], "bounded")
        self.assertEqual(len(events[0]["choices"][0]["candidates"]), 3)

        def toggle_backend():
            self.demo.stdin.write(preferred[1:] + "\n")
            self.demo.stdin.flush()

        def state_of_target(state):
            return next(backend for backend in state["backends"] if backend["id"] == preferred)

        toggle_backend()
        self.wait_until(lambda state: not state_of_target(state)["online"], "백엔드가 중단되지 않았습니다")
        fault_cursor = self.state()["cursor"]
        after_fault = self.question(session, prefix=400)
        self.assertNotEqual(after_fault["backend"], preferred)
        retry = self.wait_until(lambda state: next((event for event in state["events"]
                                                    if event["seq"] > fault_cursor
                                                    and event["session"] == session
                                                    and event["type"] == "attempt"
                                                    and event["attempt"] == 2), None),
                                "두 번째 시도의 관측 기록이 없습니다")
        self.assertEqual(retry["backend"], after_fault["backend"])

        toggle_backend()
        self.wait_until(lambda state: state_of_target(state)["online"], "백엔드가 재시작되지 않았습니다")
        time.sleep(2.6)
        self.assertEqual(self.question(session, prefix=400)["backend"], preferred)
        self.wait_until(lambda state: state_of_target(state)["health"] == "healthy",
                        "복구 시험 요청이 끝난 뒤 정상 상태가 아닙니다")

    def test_simultaneous_streams_show_active_assignments(self):
        session = uuid4().hex
        with ThreadPoolExecutor(max_workers=4) as executor:
            requests = [executor.submit(self.question, session) for _ in range(4)]
            self.wait_until(lambda state: sum(backend["active"] > 0 for backend in state["backends"]) > 1,
                            "활성 요청이 여러 백엔드에 나타나지 않았습니다")
            self.assertGreaterEqual(len({request.result()["backend"] for request in requests}), 2)
        self.wait_until(lambda state: any(candidate["reason"] == "limit"
                                          for event in state["events"] if event["type"] == "attempt"
                                          for choice in event["choices"]
                                          for candidate in choice["candidates"]),
                        "부하 상한에 걸린 후보가 기록되지 않았습니다")
        self.wait_until(lambda state: not any(backend["active"] for backend in state["backends"]),
                        "완료 뒤 활성 요청 감소가 기록되지 않았습니다")


if __name__ == "__main__":
    unittest.main(verbosity=2)
