# tests/robust — 보조 스위트 (성질 검사 + 공격·경계 E2E)

기획서 ID 체계(계약·기능 스위트) 밖에 둔 보조 테스트다. ID 스위트를 대신하지 않고, ID 스위트가 손으로 고른 예시 하나로는 닿지 않는 입력을 넓게 훑는다.

## 1. ID 스위트와의 차이

| 항목 | 계약·기능 스위트 (`tests/unit`, `tests/e2e`) | 보조 스위트 (`tests/robust`) |
| --- | --- | --- |
| 이름 | 기획서 ID (`U-DS-27`, `E-PX-05`) | 성질 이름, E2E는 `R-NN` |
| 입력 | 테스트마다 고른 입력 하나 | 생성기·변형기가 만든 입력 수십만 개 (시드 고정) |
| 판정 | 기대값 하나와 비교 | 모든 입력에서 성립해야 하는 불변식, 참조 구현과의 비교 |
| pending | `tests/pending.txt` 사용 | 쓰지 않는다. 실패는 바로 실패 |
| 빌드 | 일반 + `make asan` | 항상 ASan + UBSan |

## 2. 실행

```sh
make robust                 # 성질 검사 + E2E(pool) + E2E(loop), ASan 빌드
make robust SEED=31337      # 무작위 입력열을 바꾼다
build-asan/props 777 20     # 시드 777, 반복 20배 (기본 5배)
python3 tests/e2e/run_e2e.py --bin build-asan/httpd --tests 'tests/robust/test_*.py' -v --only R-1
```

E2E 실행기는 `tests/e2e/run_e2e.py`를 그대로 쓴다 (`--tests`로 모듈 위치만 바꾼다). `HTTPD_ENGINE=loop`이면 v2 엔진으로 돈다.

## 3. 성질 검사 (`props.c`)

모든 입력은 길이에 딱 맞는 힙 버퍼(`dup_exact`)에 복사해 넘긴다. 파서가 길이 밖을 한 바이트라도 읽으면 ASan이 `heap-buffer-overflow`로 멈춘다. `str_t`는 NUL 종료가 아니므로 `strlen` 같은 실수도 여기서 잡힌다.

| 대상 | 성질 | 잡으려는 버그 |
| --- | --- | --- |
| `request_parse_head` | 접두사를 1바이트씩 늘려도 한 번 정해진 결과(DONE·ERROR)가 바뀌지 않는다 | TCP 조각 경계에 따라 결과가 달라지는 파서 |
| `request_parse_head` | DONE이면: 메서드 3종, 경로 정규화됨, 모든 `str_t`가 머리 안, 헤더 이름 토큰·값에 CR/LF/NUL 없음, `Transfer-Encoding` 없음, 모든 `Content-Length` 항목이 framing 길이와 같음, Host 1개 | 요청 밀반입(CL·TE 불일치), 경로 탈출, 버퍼 밖 뷰 |
| `response_parse_head` | 같은 접두사 성질 + DONE 불변식 (본문 없는 응답은 NONE, TE가 있으면 LENGTH 아님, CLOSE면 재사용 안 함) | 업스트림 응답 경계 오류 |
| `cgi_parse_head` | 접두사 성질 + `fields`가 `Name: value\r\n` 줄로만 되어 있고 hop-by-hop·Date·Server·Content-Length·Status가 없다 | CGI 출력을 통한 응답 분할 |
| `path_normalize` | 세그먼트 스택 참조 구현과 반환값·결과가 같다, 결과가 길어지지 않는다, 두 번 적용해도 같다 | `..` 처리 오류, 버퍼 넘쳐 쓰기 |
| `uri_decode_path` | 임의 바이트를 일부만 `%XX`로 바꿔도 왕복이 원래대로, NUL 자리가 없는 `cap`은 실패, 쓰레기 입력은 참조 디코더와 같은 것만 받는다 | 잘못된 `%` 처리, `%00` 통과 |
| `http_body_track` (chunked) | 유효한 본문은 마지막 CRLF에서 끝나고 뒤의 다음 메시지를 쓰지 않는다, 임의 분할과 1바이트 분할이 한 번에 넣은 것과 결과·사용 바이트가 같다, MORE면 받은 바이트를 모두 쓴다 | 분할 경계에서 상태를 잃는 chunked 파서 |

시드 하나의 실행 기록은 2절의 명령으로 다시 만들 수 있다. 실패하면 입력을 C 이스케이프로 찍는다.

## 4. 공격·경계 E2E (`test_robust.py`)

| ID | 내용 |
| --- | --- |
| R-01~R-03 | 요청 밀반입: CL+TE, TE 철자 9종(공백·탭·대소문자·접힘·중복), 잘못되거나 서로 다른 Content-Length 9종. 오류 응답 하나만 오고 밀반입된 두 번째 요청은 처리되지 않는다 |
| R-04 | 잘못된 요청 줄·헤더 바이트 11종(접힘, 맨 CR, NUL, 콜론 앞 공백, Host 0개·2개 등) → 400 뒤 연결 닫힘 |
| R-05 | 경로 탈출 17종(`%2e%2e`, `..%2f`, 이중 인코딩, overlong UTF-8, 역슬래시, `%00`, 심볼릭 링크, absolute-form) → 루트 밖 파일 내용이 나오지 않는다 |
| R-06~R-10 | 1바이트씩 보낸 요청, 한 번에 보낸 50개 파이프라인, 파이프라인 중간의 잘못된 요청, POST 본문 경계, 요청 뒤 반쪽 닫기(half-close) |
| R-11~R-14 | CGI: 메타 변수, `Proxy` 헤더가 `HTTP_PROXY`가 되지 않음(httpoxy), HEAD 본문 없음, 이진 본문 왕복, `Expect: 100-continue` |
| R-15~R-22 | 원시 바이트 백엔드(`RawBackend`): 앞 빈 줄, 1xx 여러 개, chunk 확장·트레일러, 1바이트씩 오는 응답, 본문 뒤 남는 바이트, CL+TE 응답, HEAD에 붙은 본문, 헤더 안 맨 CR, LF만 쓴 응답 |
| R-23~R-24 | 동시 접속 32개 × (정적 4 + CGI 4)에서 각자 자기 바이트를 받는다, 중간에 끊는 클라이언트 40개 뒤 fd·자식 프로세스가 남지 않는다 |

## 5. 이 스위트가 찾은 버그 (2026-11-24)

| 버그 | 찾은 테스트 | 원인 | 수정 |
| --- | --- | --- | --- |
| 업스트림 응답이 빈 줄로 시작하고 수신 버퍼가 머리 끝에서 끝나면 502 (한 바이트라도 더 있으면 성공) | `response_parse_head` 접두사 성질 (시드 2·3·777), R-15 (두 엔진) | `head_len`은 앞 빈 줄을 포함한 끝 위치인데 `head_len > len - skip`으로 skip을 한 번 더 뺐다 | `src/upstream/response_parser.c` 검사를 `head_len > len`으로 |
| v2 엔진에서 요청을 보내고 쓰기 쪽만 닫은(half-close) 클라이언트가 응답을 못 받음 (v1은 정상) | R-10 (loop만 실패) | epoll이 `EPOLLRDHUP`, kqueue가 읽기 필터 `EV_EOF`를 오류(4)로 알려 conn이 버퍼의 요청을 읽기 전에 닫았다. `poller.h`의 계약은 "1 읽기 가능(또는 EOF)" | `poller_epoll.c`·`poller_kqueue.c`: 반쪽 닫기는 읽기(1)로만 알리고, `EPOLLRDHUP`은 읽기를 감시할 때만 등록 |

두 테스트 모두 수정 전 코드에서 실패하고 수정 후 통과하는 것을 이 작업에서 실행해 확인했다.

## 6. 버그가 아니라고 판단해 기대값을 바꾼 것

| 관측 | 판단 근거 |
| --- | --- |
| 요청 `Content-Length: 1, 1`을 길이 1로 받는다 | RFC 9110 8.6절은 같은 값을 쉼표로 나열한 값을 거부하거나 값 하나로 바꿔도 된다(MAY)고 한다. 그래서 성질을 "모든 항목이 같은 값"으로 바꿨고 `1, 2`는 여전히 실패로 잡는다 |
| 업스트림 상태 줄의 사유 문구에 맨 CR·NUL이 있어도 DONE | 클라이언트 쪽 상태 줄은 `response_writer.c`가 `http_reason(status)`로 새로 만들어 업스트림 사유 문구는 전달되지 않는다. 다만 RFC 9112 2.2절 문구("무효로 보거나 SP로 바꾼다")보다 느슨하다. 거부(502)로 바꿀지는 정하지 않았다 |

참고 자료 (확인 날짜 2026-11-24): [RFC 9110 8.6 Content-Length](https://www.rfc-editor.org/rfc/rfc9110.html#section-8.6), [RFC 9112 2.2 Message Parsing](https://www.rfc-editor.org/rfc/rfc9112.html#section-2.2)

## 7. 한계

- 무작위 변형 기반이다. 실행 경로를 보고 입력을 고르는 커버리지 유도 퍼징(libFuzzer, AFL++)이 아니라서 깊은 분기에는 덜 닿는다.
- kqueue 수정은 macOS에서 실행하지 않았다 (미확인). CI의 macOS 작업이 `make e2e-loop`로 v2 엔진을 돌린다.
- CGI·프록시 처리 중의 반쪽 닫기 동작(응답을 끝까지 보낼지, 클라이언트 중단으로 볼지)은 이 스위트가 판정하지 않는다.
