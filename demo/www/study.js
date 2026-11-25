const byId = (id) => document.getElementById(id);

const chat = byId("chat");
const question = byId("question");
const logs = byId("logs");
const studentList = byId("student-list");
const studentNodes = byId("map-students");
const backendNodes = byId("map-backends");
const canvas = byId("connections");
const context = canvas.getContext("2d");
const reduceMotion = matchMedia("(prefers-reduced-motion: reduce)");

const examples = [
  ["이진 탐색은 언제 써요?", "정렬된 목록에서 값을 빠르게 찾을 때 써요. 가운데 항목을 비교해 탐색 범위를 절반씩 줄입니다."],
  ["정렬되지 않은 배열에서는요?", "정렬되지 않았다면 어느 절반을 버릴지 정할 수 없어요. 먼저 정렬하거나 처음부터 차례대로 확인해야 해요."],
  ["이진 탐색의 시간 복잡도는요?", "비교할 때마다 범위가 절반이 되므로, 원소가 n개일 때 최대 비교 횟수는 O(log n)입니다."],
];
const lessonContextTokens = 800;

const policyName = {
  rr: "라운드로빈", hash: "세션 해시", bounded: "부하 상한 해시",
  least_requests: "최소 활성 요청", p2c: "두 후보 비교",
};
const reasonName = {
  eligible: "선택 가능", limit: "부하 상한 도달", excluded: "이전 시도 대상 제외",
  cooldown: "장애로 일시 제외", "recovery-limit": "복구 시험 한도 도달",
  "not-sampled": "비교 대상 밖",
};
const failureName = {
  connect: "연결 실패", send: "전송 실패", timeout: "응답 시간 초과", response: "응답 스트림 오류",
};
const healthName = {
  healthy: "정상", cooldown: "일시 제외", recovering: "복구 확인 중", offline: "중단", unknown: "대기",
};
const colors = ["#4358c5", "#a4509c", "#057c76", "#b36124", "#6f4bc6", "#146e9f"];

function create(tag, value, className) {
  const node = document.createElement(tag);
  if (value != null) node.textContent = value;
  if (className) node.className = className;
  return node;
}

function makeStudent(name, index) {
  const id = crypto.randomUUID();
  return {
    name, id, color: colors[index % colors.length], history: [], prefix: lessonContextTokens,
    lastBackend: null, current: null, draft: "",
  };
}

const students = ["민지", "준호", "서연", "지우"].map(makeStudent);
let selected = students[0];
let totalRequests = 0;
let cursor = 0;
let backends = [];
let live = false;

function active(student) {
  return student.current?.pending === true;
}

function matchedBackend(student) {
  const request = student.current;
  return request?.backend ?? request?.assigned ?? (active(student) ? null : student.lastBackend);
}

function renderStudents() {
  studentList.replaceChildren(...students.map((student) => {
    const button = create("button", student.name);
    button.type = "button";
    button.dataset.student = student.id;
    button.dataset.pending = String(active(student));
    button.setAttribute("aria-pressed", String(student === selected));
    if (active(student)) button.setAttribute("aria-label", student.name + " 응답 중");
    button.addEventListener("click", () => selectStudent(student, true));
    return button;
  }));
}

function selectStudent(student, focusButton = false) {
  selected = student;
  question.value = student.draft;
  renderStudents();
  renderChat();
  updateComposer();
  if (focusButton) studentList.querySelector('[data-student="' + student.id + '"]').focus();
}

function updateComposer() {
  const group = byId("audience").value === "group";
  byId("question-send").textContent = group ? "함께 보내기" : "질문 보내기";
  byId("question-send").disabled = !live || (group ? students.every(active) : active(selected));
}

function renderChat() {
  const messages = selected.history.flatMap((request) => {
    const sent = create("p", request.text, "message student");
    const box = create("div", null, "message");
    box.append(create("small", "모의 학습 답변"));
    let body = request.answer;
    if (request.pending) {
      body = request.firstMs == null ? "응답을 기다리고 있어요…" : "모의 백엔드가 답변을 생성하고 있어요…";
    }
    if (request.error) {
      body = request.error;
      box.dataset.error = "true";
    }
    box.append(create("p", body));
    return [sent, box];
  });
  if (!messages.length) messages.push(create("p", selected.name + "의 첫 질문을 보내보세요.", "chat-empty"));
  chat.replaceChildren(...messages);
  chat.scrollTop = chat.scrollHeight;

  const suggestion = examples[Math.min(selected.history.length, examples.length - 1)][0];
  const button = create("button", suggestion);
  button.type = "button";
  button.addEventListener("click", () => {
    question.value = suggestion;
    selected.draft = suggestion;
    question.focus();
  });
  byId("suggestions").replaceChildren(button);
}

function renderMap() {
  const count = students.filter(active).length;
  const counter = byId("active-questions");
  const label = "진행 중 " + count + "건";
  if (counter.textContent !== label) counter.textContent = label;
  counter.dataset.active = String(count > 0);

  studentNodes.replaceChildren(...students.map((student) => {
    const target = matchedBackend(student);
    let label = target ?? "대기";
    if (active(student)) {
      label = "배정 대기";
      if (target) label = target + (student.current.backend ? " 응답 중" : " 연결 중");
    }
    const item = create("li", null, "map-node");
    item.dataset.student = student.id;
    item.dataset.active = String(active(student));
    item.append(create("span", student.name, "node-title"), create("span", label, "node-state"));
    return item;
  }));
  backendNodes.replaceChildren(...backends.map((backend) => {
    const item = create("li", null, "map-node");
    const health = backend.online ? (backend.health ?? "unknown") : "offline";
    item.dataset.backend = backend.id;
    item.dataset.health = health;
    item.dataset.active = String(backend.active > 0);
    item.append(create("span", backend.id, "node-title"),
                create("span", healthName[health] + " · " + backend.active + "건", "node-state"));
    return item;
  }));
  queueDrawing();
}

let drawing = 0;
const displayed = new Map();

function queueDrawing() {
  if (context && !drawing) drawing = requestAnimationFrame(draw);
}

function draw(now) {
  drawing = 0;
  const frame = canvas.getBoundingClientRect();
  if (!frame.width || !frame.height) return;
  const scale = devicePixelRatio || 1;
  const width = Math.round(frame.width * scale);
  const height = Math.round(frame.height * scale);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  context.setTransform(scale, 0, 0, scale, 0, 0);
  context.clearRect(0, 0, frame.width, frame.height);
  let animate = false;

  for (const student of students) {
    const target = matchedBackend(student);
    const sourceNode = studentNodes.querySelector('[data-student="' + student.id + '"]');
    const targetNode = target && backendNodes.querySelector('[data-backend="' + target + '"]');
    if (!sourceNode || !targetNode) continue;
    const source = sourceNode.getBoundingClientRect();
    const destination = targetNode.getBoundingClientRect();
    const x1 = source.right - frame.left;
    const y1 = source.top + source.height / 2 - frame.top;
    const x2 = destination.left - frame.left;
    const y2 = destination.top + destination.height / 2 - frame.top;

    const visual = displayed.get(student.id) ?? {target, from: y1, y: y1, since: now};
    if (visual.target !== target) {
      visual.target = target;
      visual.from = visual.y;
      visual.since = now;
    }
    const time = reduceMotion.matches ? 1 : Math.min(1, (now - visual.since) / 320);
    visual.y = visual.from + (y2 - visual.from) * (1 - (1 - time) ** 3);
    displayed.set(student.id, visual);
    const pending = active(student);
    context.strokeStyle = student.color;
    context.globalAlpha = pending ? .85 : .31;
    context.lineWidth = pending ? 2.7 : 1.8;
    context.setLineDash(pending && !reduceMotion.matches ? [6, 5] : []);
    context.lineDashOffset = -now / 28;
    const middle = (x1 + x2) / 2;
    context.beginPath();
    context.moveTo(x1, y1);
    context.bezierCurveTo(middle, y1, middle, visual.y, x2, visual.y);
    context.stroke();
    animate ||= time < 1 || (pending && !reduceMotion.matches);
  }
  context.globalAlpha = 1;
  if (animate) queueDrawing();
}

new ResizeObserver(queueDrawing).observe(byId("map"));

function metric(label, value) {
  const node = create("div");
  node.append(create("dt", label), create("dd", value));
  return node;
}

function timeText(value) {
  return value == null ? "—" : Math.round(value) + " ms";
}

function createLog(request) {
  request.row = create("details", null, "request");
  request.row.open = totalRequests === 0;
  request.summary = create("summary");
  request.detail = create("div", null, "request-detail");
  request.row.append(request.summary, request.detail);
  logs.querySelector(".empty-log")?.remove();
  logs.prepend(request.row);
  byId("log-count").textContent = ++totalRequests + "건";
}

function redistributedForLoad(request) {
  if (!request.backend || request.error) return false;
  return request.attempts.some(attempt =>
    attempt.backend === request.backend && !attempt.error && attempt.failures.length === 0 &&
    attempt.choices.some(choice => choice.policy === "bounded" &&
      choice.candidates.some(candidate => candidate.rank === 1 && candidate.reason === "limit" &&
        candidate.backend !== attempt.backend)));
}

function renderLog(request) {
  const status = request.status == null ? (request.pending ? "진행 중" : "응답 없음") : String(request.status);
  const badge = create("span", status, "status");
  badge.dataset.kind = request.error || request.status >= 400 ? "error" : (request.pending ? "pending" : "ok");
  const assigned = request.backend ?? request.assigned;
  const meta = create("span", null, "request-summary-meta");
  meta.append(create("span", assigned ?? (request.pending ? "배정 대기" : "미배정")), badge);
  if (request.error && request.status === 200) {
    meta.append(create("span", "스트림 중단"));
  } else if (request.firstMs != null) {
    meta.append(create("span", "첫 토큰 " + timeText(request.firstMs)));
  } else if (request.totalMs != null) {
    meta.append(create("span", "전체 " + timeText(request.totalMs)));
  }
  const cacheStatus = request.cached == null ? null : (request.cached > 0 ? "HIT" : "MISS");
  if (cacheStatus) {
    const cacheBadge = create("span", "캐시 " + cacheStatus, "status cache-status");
    cacheBadge.dataset.kind = cacheStatus === "HIT" ? "ok" : "neutral";
    meta.append(cacheBadge);
  }
  if (redistributedForLoad(request)) {
    meta.append(create("span", "부하 분산", "status balance-status"));
  }
  request.summary.replaceChildren(create("span", request.student.name, "request-name"), meta);

  const path = create("p", "POST /v1/chat/completions · " + request.id.slice(0, 8), "request-path");
  const choices = request.attempts.flatMap((attempt) => attempt.choices);
  const selectionTime = choices.length ? choices.reduce((sum, choice) => sum + choice.duration_us, 0) + " µs" : "—";
  const metrics = create("dl", null, "metrics");
  metrics.append(metric("라우팅 판단", selectionTime), metric("첫 토큰까지", timeText(request.firstMs)),
                 metric("전체 응답", timeText(request.totalMs)));
  const cache = cacheStatus ?? (request.pending ? "확인 대기" : "—");
  const affinity = assigned && request.previousBackend ?
    (assigned === request.previousBackend ? "같은 서버" : "다른 서버") :
    (request.previousBackend ? "배정 대기" : "첫 배정");
  const session = create("p", "세션 " + affinity + " · prefix 캐시 " + cache, "session-note");
  request.detail.replaceChildren(path, create("p", request.text, "request-question"), metrics, session);
  if (request.inputTokens != null) {
    const prefill = "입력 " + request.inputTokens + "토큰 중 " + request.cached + "토큰 재사용" +
      " · 모의 입력 처리 " + timeText(request.prefillMs);
    request.detail.append(create("p", prefill, "session-note"));
  }

  for (const attempt of request.attempts) {
    const block = create("section", null, "attempt");
    const header = (attempt.attempt === 1 ? "첫 시도" : attempt.attempt + "번째 시도") +
      (attempt.backend ? " · " + attempt.backend : " · 선택된 서버 없음");
    block.append(create("h3", header));
    for (const choice of attempt.choices) {
      let name = policyName[choice.policy];
      if (choice.limit) name += " · 상한 " + choice.limit + "건";
      if (choice.preferred) name += " · 선택 " + choice.preferred;
      block.append(create("p", name));
      const list = create("ul");
      for (const candidate of choice.candidates) {
        const rank = ["bounded", "hash"].includes(choice.policy) && candidate.rank ?
          " · 해시 " + candidate.rank + "순위" : "";
        list.append(create("li", candidate.backend + " · 처리 중 " + candidate.active +
                           "건" + rank + " · " + reasonName[candidate.reason]));
      }
      block.append(list);
    }
    if (attempt.error) block.append(create("p", "연결을 확보하지 못했어요.", "failure"));
    for (const failure of attempt.failures) {
      block.append(create("p", failureName[failure] ?? failure, "failure"));
    }
    request.detail.append(block);
  }
  if (request.error) request.detail.append(create("p", request.error, "failure"));
}

function applyEvent(event) {
  const student = students.find((entry) => entry.id === event.session);
  const request = student?.current;
  if (!request || event.seq <= request.cursor || event.at_ms < request.startedAt) return false;
  if (event.type === "attempt") {
    request.attempts.push({...event, failures: []});
    request.assigned = event.backend;
  } else {
    const attempt = request.attempts.find((item) => item.attempt === event.attempt);
    if (attempt) attempt.failures.push(event.reason);
  }
  renderLog(request);
  return true;
}

async function poll() {
  try {
    const response = await fetch("/__demo/state?after=" + cursor, {cache: "no-store"});
    if (!response.ok) throw Error("monitor");
    const state = await response.json();
    if (!live) {
      live = true;
      byId("connection").textContent = "서버 연결됨";
      byId("connection").dataset.status = "live";
      updateComposer();
    }
    byId("runtime-config").textContent = (state.engine === "loop" ? "이벤트 루프" : "스레드 풀") +
      " · " + state.picker;
    let changed = JSON.stringify(backends) !== JSON.stringify(state.backends);
    backends = state.backends;
    for (const event of state.events) changed = applyEvent(event) || changed;
    cursor = state.cursor;
    if (changed) renderMap();
  } catch {
    live = false;
    byId("connection").textContent = "서버 연결 끊김 · 재시도 중";
    byId("connection").dataset.status = "error";
    updateComposer();
  } finally {
    setTimeout(poll, 170);
  }
}

async function readStream(response, request, started) {
  const decoder = new TextDecoder();
  const reader = response.body.getReader();
  let buffered = "";
  let doneEvent;
  let finished = false;
  try {
    for (;;) {
      const {done, value} = await reader.read();
      if (done) break;
      buffered += decoder.decode(value, {stream: true});
      let boundary;
      while ((boundary = buffered.indexOf("\n\n")) !== -1) {
        const line = buffered.slice(0, boundary);
        buffered = buffered.slice(boundary + 2);
        if (!line.startsWith("data: ")) continue;
        const payload = line.slice(6);
        if (payload === "[DONE]") {
          finished = true;
          continue;
        }
        const event = JSON.parse(payload);
        if (event.done) doneEvent = event;
        if (event.i && request.firstMs == null) {
          request.firstMs = performance.now() - started;
          renderLog(request);
          if (selected === request.student) renderChat();
        }
      }
    }
  } finally {
    if (!finished) await reader.cancel().catch(() => {});
    reader.releaseLock();
  }
  if (!finished || !doneEvent) throw Error("incomplete");
  return doneEvent;
}

async function sendQuestion(student, text) {
  if (active(student)) return;
  const request = {
    id: crypto.randomUUID(), student, text, startedAt: Date.now(), cursor, pending: true,
    attempts: [], previousBackend: student.lastBackend, backend: null, assigned: null,
    status: null, firstMs: null, totalMs: null, inputTokens: null, prefillMs: null,
    cached: null, error: null, answer: "",
  };
  student.current = request;
  student.history.push(request);
  createLog(request);
  renderLog(request);
  renderStudents();
  renderMap();
  updateComposer();
  if (student === selected) renderChat();

  // 공통 학습 자료와 완료된 대화가 모의 모델에 전달할 prefix를 이룬다.
  const prefix = Math.min(student.prefix, 2400);
  const input = prefix + 160 + [...text].length;
  const started = performance.now();
  try {
    const response = await fetch("/v1/chat/completions?request=" + request.id, {
      method: "POST",
      headers: {"Content-Type": "application/json", "X-Session-Id": student.id},
      body: JSON.stringify({session: student.id, input_tokens: input,
                            prefix_tokens: prefix, max_tokens: 66, stream: true}),
    });
    request.status = response.status;
    request.backend = response.headers.get("X-Backend-Id");
    renderLog(request);
    renderMap();
    if (!response.ok) {
      await response.text();
      request.error = "서버가 HTTP " + response.status + " 응답을 보냈어요.";
      return;
    }
    const end = await readStream(response, request, started);
    request.backend = end.backend;
    request.cached = end.usage.cached_tokens;
    request.inputTokens = end.usage.input_tokens;
    request.prefillMs = end.timing.prefill_ms;
    student.prefix = end.usage.input_tokens + end.usage.output_tokens;
    student.lastBackend = request.backend;
    request.answer = examples.find(([prompt]) => prompt === text)?.[1] ??
      "모의 백엔드가 " + end.usage.output_tokens + "개의 토큰을 생성했어요.";
  } catch {
    request.error = request.status === 200 ? "응답 스트림이 끝나기 전에 끊겼어요." : "서버 응답을 받지 못했어요.";
  } finally {
    request.pending = false;
    request.totalMs = performance.now() - started;
    renderLog(request);
    renderStudents();
    renderMap();
    updateComposer();
    if (selected === student) renderChat();
  }
}

byId("composer").addEventListener("submit", (event) => {
  event.preventDefault();
  const text = question.value.trim();
  if (!text || !live) return;
  const group = byId("audience").value === "group";
  const targets = (group ? students : [selected]).filter((student) => !active(student));
  for (const student of targets) {
    student.draft = "";
    void sendQuestion(student, text);
  }
  question.value = selected.draft;
});

question.addEventListener("input", () => { selected.draft = question.value; });
byId("audience").addEventListener("change", updateComposer);
const dialog = byId("student-dialog");
const nameInput = byId("student-name");
byId("student-add").addEventListener("click", () => {
  nameInput.value = "";
  dialog.showModal();
  nameInput.focus();
});
byId("student-cancel").addEventListener("click", () => dialog.close());
byId("student-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const name = nameInput.value.trim();
  if (!name) return;
  const student = makeStudent(name, students.length);
  students.push(student);
  dialog.close();
  selectStudent(student);
  renderMap();
  question.focus();
});

selectStudent(selected);
renderMap();
void poll();
