# upstream/ AGENTS

- 대상 테스트: 계약 U-US-01, 09, 12, 17, 24 · 기능 E-PX-01~17.
- include 가능한 다른 디렉토리 헤더: `core/*`, `common/*`.
- 함정:
  - 재사용 연결은 백엔드가 이미 닫았을 수 있다. acquire 때 MSG_PEEK로 확인하고, 그래도 첫 송신·첫 응답에서 죽으면 같은 백엔드로 1회 새 연결 (D-011).
  - non-blocking connect는 EINPROGRESS 뒤 쓰기 가능 + SO_ERROR로 결과를 본다. 바로 실패로 보지 않는다.
  - 요청 라인은 디코딩하지 않은 `raw_path`를 쓴다. 다시 인코딩하지 않는다 (U-US-05).
  - 응답 본문 끝을 모른 채(길이 미달 EOF, chunked 오류) 연결을 풀에 넣으면 다음 요청이 앞 응답의 찌꺼기를 받는다.
  - `upstream_pick`·풀은 여러 워커가 동시에 부른다. 라운드로빈 카운터와 유휴 목록은 mutex로.
- 작업을 나눌 때는 파일 단위로.
