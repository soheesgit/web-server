# bench/

LLM 트래픽을 흉내 내 게이트웨이(httpd)를 재는 도구. `make test`(CI)와 분리되어 있다.
모두 Python 3 표준 라이브러리만 쓴다. 저장소 루트에서 `make` 후 실행한다.

## 파일

| 파일 | 역할 |
| --- | --- |
| `fake_llm.py` | 가짜 LLM 백엔드: 배치, prefill, 슬롯·KV 상한과 선점, prefix 캐시, 부하 보고 헤더, SSE |
| `loadgen.py` | 부하 생성기: 열린 루프(포아송) / 닫힌 루프, 시나리오 A·B·C, 요청별 JSONL |
| `run_bench.py` | 실행기: 백엔드 N대 + httpd + loadgen을 띄우고 `results/<시각>-<실험>/`에 모은다 |
| `calib/calib.py` | 보정 측정 (Ollama): prefill, 동시성, 캐시, 컨텍스트 길이 |
| `calib/results-*/SUMMARY.md` | 보정 결과와 fake_llm 기본 계수의 근거 |

`results/`는 git에 올리지 않는다 (`.gitignore`). 남길 결과는 요약을 문서로 옮긴다.

## 시간 측정

- 모든 도구가 `time.clock_gettime(CLOCK_MONOTONIC)`(ms)를 쓴다. macOS의 `time.monotonic()`은
  프로세스마다 기준이 달라 프로세스 사이 비교에 쓸 수 없다.
- fake_llm은 마지막 SSE 이벤트에 자기 쪽 시간(`queue_ms`, `prefill_ms`, `ttft_backend_ms`)을
  싣는다. 클라이언트 TTFT − 백엔드 TTFT = 게이트웨이와 네트워크가 더한 시간.
- fake_llm의 LLM 처리 시간은 측정값이 아니라 보정한 입력값이다. 절대값이 아니라 같은 조건에서
  picker·엔진끼리의 상대 비교에 쓴다.

## 실험

| 이름 | 명령 | 보는 것 |
| --- | --- | --- |
| E0 | `python3 -I bench/run_bench.py e0` | 프록시가 더하는 TTFT·토큰 간격, 끊김 전파 (직접 / loop) |
| E1 | `python3 -I bench/run_bench.py e1` | 오래 열린 스트림 동시성 (loop, 백엔드는 병목 아님) |
| E2 | `python3 -I bench/run_bench.py scenario --name B --rate 3 --repeat 3` | 시나리오별 TTFT p50/p99, 백엔드별 분배 |

게이트웨이는 기본 엔진인 이벤트 루프(loop)로 잰다. v1 스레드 풀은 비교할 때만
`--engines loop pool`로 함께 돌린다.

백엔드 1대(슬롯 4, 기본 계수)는 시나리오 A 요청을 초당 약 1개 처리한다. 4대면 약 4개.
시나리오 B의 긴 요청(입력 5만)은 prefill만 약 45초라 실행기는 게이트웨이의
`upstream_response_timeout_ms`·`body_timeout_ms`를 600초로 둔다.

## 보정 다시 하기

```
ollama pull llama3.2:1b
python3 -I bench/calib/calib.py out.jsonl prefill cache
OLLAMA_HOST=127.0.0.1:11435 OLLAMA_NUM_PARALLEL=4 ollama serve &   # 병렬 측정용 서버
OLLAMA_PORT=11435 python3 -I bench/calib/calib.py out.jsonl concurrency ctx
```
