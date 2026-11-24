# 보정 측정: Ollama 0.40.1 + llama3.2:1b, Apple M4 16GB (2026-11-24)

가짜 LLM 백엔드(`bench/fake_llm.py`)의 계수를 맞추기 위한 측정. 절대 속도는 노트북 1B 모델 기준이므로
실제 GPU 서버와 다르다. 쓰는 것은 곡선의 모양(선형인가, 무엇이 무엇을 막는가)이다.

재현: `python3 -I bench/calib/calib.py <out.jsonl> [prefill|concurrency|cache|ctx]`
(병렬 측정은 `OLLAMA_HOST=127.0.0.1:11435 OLLAMA_NUM_PARALLEL=4 ollama serve`를 따로 띄우고
`OLLAMA_PORT=11435`로 실행)

| 파일 | 내용 |
| --- | --- |
| `prefill_concurrency_cache.jsonl` | [1] prefill, [3] 캐시. **이 파일의 concurrency 행은 무효** (모델이 9토큰에서 멈춤, 프롬프트 수정 전) |
| `concurrency_parallel1.jsonl` | [2] 기본 서버(병렬 1), 출력 128토큰 |
| `concurrency_parallel4.jsonl` | [2] `OLLAMA_NUM_PARALLEL=4`, 출력 128토큰 |
| `ctx_parallel4.jsonl` | [4] 입력 길이 × 동시성, 병렬 4 |

## [1] prefill 대 입력 길이 (캐시 회피, 3회 중앙값)

| 입력 토큰 | prefill ms | ms/토큰 |
| --- | --- | --- |
| 102 | 92 | 0.90 |
| 264 | 247 | 0.94 |
| 901 | 837 | 0.93 |
| 1,757 | 1,701 | 0.97 |
| 3,464 | 3,589 | 1.04 |
| 6,874 | 8,057 | 1.17 |

거의 선형, 길면 조금 가팔라진다.

## [2] 동시 요청 수 대 토큰 간격 (짧은 입력, 출력 128토큰)

| 동시 | 병렬1 TTFT 중앙 | 병렬1 ITL | 병렬4 TTFT 중앙 | 병렬4 ITL | 병렬4 처리량 |
| --- | --- | --- | --- | --- | --- |
| 1 | 53 ms | 19.7 ms | 51 ms | 16.9 ms | 57 tok/s |
| 2 | 1,443 ms | 19.9 ms | 210 ms | 23.1 ms | 81 tok/s |
| 4 | 4,107 ms | 19.6 ms | 372 ms | 36.2 ms | 103 tok/s |
| 8 | 7,943 ms | 17.0 ms | 2,793 ms | 36.8 ms | 103 tok/s |

병렬 1은 순수 대기열(ITL 불변, TTFT 계단). 병렬 4는 배치: 요청 하나 늘 때 ITL 약 +6.3 ms,
슬롯(4)을 넘으면 내부 대기열.

## [3] prefix 캐시 (같은 3,464토큰 반복)

| 회차 | 캐시 적중 | prefill ms |
| --- | --- | --- |
| 0 | 21 | 3,667 |
| 1 | 3,463 | 76 |
| 2 | 3,463 | 17 |
| 3 | 3,463 | 17 |

## [4] 컨텍스트 길이 × 동시성 (병렬 4, 출력 64토큰)

| 입력 토큰 | 단독 ITL | 4개 동시 ITL |
| --- | --- | --- |
| 111 | 15.0 ms | 35.3 ms |
| 910 | 14.7 ms | 33.6 ms |
| 2,619 | 15.0 ms | 540 ms |

단독에서는 컨텍스트 길이가 ITL에 거의 영향이 없다. 긴 입력이 동시에 들어오면 다른 요청의
prefill이 진행 중인 decode를 막아 ITL이 폭증한다.

## 가짜 백엔드 계수 (위 측정에서)

```
prefill_ms_per_token = 0.9     # 캐시 안 된 입력 토큰당
step_base_ms         = 17      # 배치 1의 토큰 간격
step_per_seq_ms      = 6.3     # 배치에 요청 하나 늘 때마다
slots                = 4       # 동시 decode 상한 (넘으면 내부 대기열)
단계 규칙: 그 단계에 새로 들어온 요청의 prefill 시간이 그 단계 전체를 막는다
캐시: 적중한 앞부분 토큰은 prefill에서 뺀다 (적중 시 prefill 약 99% 감소)
```
