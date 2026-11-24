# Stage 4 프록시 구현 및 검증

## 시작점과 구현 순서

`feat/stage4-sohee`는 `origin/develop`의 `63300ee`에서 시작했다. 해당 커밋에는 STATIC·CGI, 요청 파서, 라우터와 v1 conn 일부가 구현돼 있었으며, upstream·프록시 처리기·v1 엔진과 app은 스텁이었다. 기준선 테스트는 138개 중 57개 통과, 81개 pending이었다.

| 단계 | 변경 | 검증 |
|---|---|---|
| 1. v1 Reverse Proxy | `app/`과 `engine/` 기동, v1 워커, `upstream/` 직렬화·응답 파싱·relay, `handler/proxy.c` 판단, `downstream/conn.c` 흐름 연결 | U-EN-01·02, U-US·U-PX 계약 최소셋, E-PX-01~17, STATIC·CGI·Keep-Alive |
| 2. 부하 분산 | 그룹 mutex를 통한 RR·Least Requests·P2C 선택과 활성 요청 예약 | RR 순서, LR·P2C 후보 점유 검증, 동시 요청 부하, 종료 시 활성 요청 0 |
| 3. 유휴 연결 풀 | 최근 반납 연결 우선, 재획득 검사, 60초 만료와 30초 정리, 종료 시 정리 스레드 join | 실제 유휴 시간 실험, 업스트림 연결 종료, 100MB 스트리밍, FD·메모리 검사 |
| 4. 장애 복구 | 기본 3회 실패·10초 제외, 시험 1개 및 1·2·4·8 복귀, 후보가 없으면 503 | 복구 단계별 포화·취소, 일부 및 전체 장애 실험 |
| 5. Forward Proxy | 옵트인 루프백 GET·POST, absolute-form과 정확한 포트 검사 | GET·POST, 헤더 변환, 미설정·다른 포트·임의 이름·CONNECT 거부 |
| 6. 벤치마크 | Python 표준 라이브러리로 데이터 수집, 표와 그래프 생성 | 18조건 × 5회와 유휴 만료 3조건 × 5회 |

## 요청 처리와 소유권

1. v1 메인 스레드가 연결을 받아 작업 큐에 넣는다. 워커가 요청과 본문을 읽고 프레이밍을 검증한다. STATIC·CGI에는 기존 라우터와 처리기를 사용한다.
2. 프록시 요청은 그룹 mutex 안에서 가용 대상 선택과 활성 요청 수 증가를 함께 수행한다. RR의 커서는 그룹별이다. Least Requests는 순환 순서로 동률을 푼다. P2C는 잠금 안에서 서로 다른 후보 두 개를 뽑고 동률이면 같은 순환 순서를 사용한다.
3. 유휴 목록에서 연결을 떼거나 non-blocking connect를 시작한다. 선택된 대상으로 origin-form 요청과 Host를 직렬화한다. `keep_host`는 원래 Host를 보존하고, 재시도 시에는 원본 요청에서 새로 직렬화한다.
4. 워커는 poll로 부분 송신·응답 머리·클라이언트 종료를 감시한다. 응답 머리 대기가 끝나면 소유권이 응답 계획으로 넘어간다. 다운스트림은 길이·청크·종료 경계를 확인하며 청크 단위로 중계한다.
5. 정상 완료한 응답을 성공으로 보고한다. 재사용 가능한 소켓만 유휴 풀에 반납한다. 실패·취소·포화·만료 연결은 닫고 시도 슬롯은 한 번 반환한다. 유휴 목록에서 분리한 연결을 목록 잠금 밖에서 닫는다. 서버 종료 시 워커와 연결을 먼저 정리하고 reaper를 깨워 join한 뒤 레지스트리를 해제한다.

## 장애와 HTTP 정책

- 연결 거절과 연결·송신·응답 마감, 잘못된 머리, 응답 중 EOF, 길이 미달과 잘못된 청크를 수송·프레이밍 실패로 센다. 완전히 끝난 HTTP 404·500은 그대로 전달하며 정상 수송으로 기록한다. 클라이언트 취소와 메모리·FD 확보 실패 자체는 백엔드 상태에 반영하지 않는다.
- 제외에 들어갈 때 유휴 연결을 폐기한다. 이미 처리 중인 시도를 유지하고, 해당 시도가 모두 빠진 뒤 만료된 대상에 시험을 하나 허용한다. 단계별 허용 동시성만큼 완전한 성공을 쌓으면 다음 단계로 간다. 8단계 성공 후 정상화하고, 복구 중 실패하면 다시 제외한다.
- 요청 전송 전 연결 실패는 다른 대상으로 재시도할 수 있다. 재사용 연결의 0바이트 송신 실패는 같은 대상의 새 소켓으로 한 번 복구한다. 부분 송신 GET·HEAD에는 U-PX-07을 적용한다. POST 전송 후 재실행을 금지하며 응답 머리 마감은 504, 잘못된 응답은 502다. 머리가 이미 전송됐다면 불완전한 응답은 연결을 닫아서 끝낸다. 계약 정합성은 D-041에 기록했다.
- 표준 hop-by-hop 헤더와 Connection이 지정한 필드를 제거한다. 검증한 요청 길이를 한 번만 직렬화한다. X-Forwarded-For에는 기존 값 뒤에 직접 연결한 주소를 덧붙인다. 앞쪽 체인은 신뢰하는 프록시 뒤에서만 인증 근거로 사용할 수 있다.
- 업스트림 대기 중 클라이언트 읽기 EOF를 취소로 처리한다. 앞에 보낸 요청이 백엔드에서 실행됐는지는 연결 종료로 판단할 수 없다.
- 3초·100개 FIFO는 D-042의 독립 이벤트 엔진 작업이다. v1은 대기열을 만들지 않으며 첫 선택에서 가용 대상이 없으면 즉시 503, 시작한 시도가 모두 실패하면 502를 전송한다.

## 실행 설정

이기종 백엔드를 사용하는 Reverse Proxy 실행 프로필은 Least Requests와 백엔드당 유휴 연결 8개를 적용한다. 로컬 백엔드 세 대를 시작한 뒤 저장소 루트에서 실행한다.

```sh
make reverse-proxy
```

`conf/reverse-proxy.conf`는 127.0.0.1:8080에서 요청을 받고 8081~8083으로 중계한다. 배포 환경에서는 대상 리스너와 백엔드 주소를 설정하고 `pool_max_idle 8`을 명시한다. 별도 파일은 `make reverse-proxy REVERSE_PROXY_CONF=/path/to/reverse-proxy.conf`로 지정한다. `./build/httpd`를 직접 실행할 때 `HTTPD_PROXY_LB`를 생략하면 기존의 RR 기본값을 사용한다.

`HTTPD_PROXY_LB`는 `rr`(기본), `least_requests`, `p2c`를 받는다. `HTTPD_PROXY_RECOVERY`는 `ramp`(기본), `single`을 받는다. `single`은 시험 요청 한 개가 성공하면 바로 정상으로 복귀한다. `pool_max_idle`은 기존 설정 파일의 유휴 상한이며 기본값은 8이다. `HTTPD_PROXY_METRICS=1`은 프로세스 종료 로그에 백엔드별 집계를 기록한다.

Forward Proxy는 프로세스 시작 시 `HTTPD_FORWARD_PROXY=18081`로 허용 포트 하나를 지정한다. 리스너와 클라이언트는 `127.0.0.1`이어야 한다. 같은 포트의 숫자 루프백 주소만 기존 Host route와 연결할 수 있다.

```text
listen 127.0.0.1:8080
root ./tests/fixtures/www
upstream local_forward 127.0.0.1:18081
route / static ./tests/fixtures/www
host 127.0.0.1 route / proxy local_forward
```

```sh
HTTPD_FORWARD_PROXY=18081 ./build/httpd -c forward.conf
curl --noproxy '' -x http://127.0.0.1:8080 http://127.0.0.1:18081/health
```

레지스트리는 숫자 주소를 `inet_pton`으로 고정해 조회 시 이름이 바뀌어 다른 IP로 연결할 여지를 없앤다. 허용 정책을 통과하면 URI의 authority를 Host로 사용한다. HTTPS CONNECT, 호스트 이름 허용 목록, 전용 Forward Proxy route kind는 현재 구현 범위 밖이다.

## 검증 명령

```sh
make
make lint
make test
make test ONLY=E-PX
make asan
python3 tests/bench/proxy_verify.py --idle-expiry
```

macOS 기본 빌드에서 계약·기능 테스트 135개가 통과했다. E-PX 17개는 별도 실행에서도 모두 통과했다. Linux arm64 컨테이너에서 ASan·UBSan 테스트 135개와 추가 로컬 프록시 검증을 통과했다. `U-DS-40`, `U-DS-44`, `U-EN-05`는 v2 미구현으로 pending이다. 팀 계약 검토 항목은 D-041~044에 기록했다.

성능 비교 방법과 결과는 [벤치마크 보고서](benchmarks/README.md)에 있다.
