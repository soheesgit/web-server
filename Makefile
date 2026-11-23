# Makefile — httpd 빌드·테스트·린트 진입점. 사용법은 README.md, 테스트 규칙은 docs/TESTING.md.
#
# 소스 목록은 wildcard로 모으므로 src/<dir>/ 나 tests/ 에 파일을 추가해도 이 파일은 고치지 않는다.

CC           ?= cc
PYTHON       ?= python3
CLANG_FORMAT ?= clang-format
BUILD        ?= build
PENDING      ?= tests/pending.txt
UNAME_S      := $(shell uname -s)

WARN      := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Werror
BASEFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L $(WARN) -O2 -g -Isrc
ifeq ($(UNAME_S),Darwin)
OSFLAGS := -D_DARWIN_C_SOURCE
else ifeq ($(UNAME_S),Linux)
OSFLAGS := -D_GNU_SOURCE
else
OSFLAGS :=
endif

ALL_CFLAGS  := $(BASEFLAGS) $(OSFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)
ALL_LDFLAGS := $(EXTRA_LDFLAGS) $(LDFLAGS)
LDLIBS      := -lpthread

SRCS      := $(wildcard src/*/*.c)
MAIN_SRC  := src/app/main.c
LIB_SRCS  := $(filter-out $(MAIN_SRC),$(SRCS))
TEST_SRCS := $(wildcard tests/unit/*.c tests/unit/contract_min/*.c tests/fakes/*.c)

LIB_OBJS  := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRCS))
MAIN_OBJ  := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(filter $(MAIN_SRC),$(SRCS)))
TEST_OBJS := $(patsubst tests/%.c,$(BUILD)/obj/tests/%.o,$(TEST_SRCS))
PROPS_OBJ := $(BUILD)/obj/tests/robust/props.o
DEPS      := $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJS:.o=.d) $(PROPS_OBJ:.o=.d)

BIN       := $(BUILD)/httpd
UNIT_BIN  := $(BUILD)/run_unit
PROPS_BIN := $(BUILD)/props

FMT_FILES := $(wildcard src/*/*.c src/*/*.h tests/unit/*.c tests/unit/*.h \
                        tests/unit/contract_min/*.c tests/fakes/*.c tests/fakes/*.h tests/robust/*.c)

ONLY_ARG := $(if $(ONLY),--only $(ONLY),)

.PHONY: all unit e2e e2e-loop test lint asan robust robust-run clean

all: $(BIN)

$(BIN): $(LIB_OBJS) $(MAIN_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

$(UNIT_BIN): $(LIB_OBJS) $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

$(PROPS_BIN): $(LIB_OBJS) $(PROPS_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(LDLIBS)

# tests 규칙을 먼저 둔다 (GNU make 3.81은 먼저 정의된 패턴을 고른다).
$(BUILD)/obj/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -Itests -MMD -MP -c -o $@ $<

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

unit: $(UNIT_BIN)
	$(UNIT_BIN) --pending $(PENDING) $(ONLY_ARG)

e2e: $(BIN)
	$(PYTHON) tests/e2e/run_e2e.py --bin $(BIN) --pending $(PENDING) $(ONLY_ARG)

# 같은 E2E를 v2 이벤트 루프 엔진(-e loop)으로 돌린다 (D-040).
e2e-loop: $(BIN)
	HTTPD_ENGINE=loop $(PYTHON) tests/e2e/run_e2e.py --bin $(BIN) --pending $(PENDING) $(ONLY_ARG)

# unit과 e2e를 모두 돌리고 두 요약 줄을 합친 줄을 마지막에 출력한다.
test: $(BIN) $(UNIT_BIN)
	@{ $(UNIT_BIN) --pending $(PENDING) $(ONLY_ARG); echo $$? > $(BUILD)/unit.rc; } | tee $(BUILD)/unit.log
	@{ $(PYTHON) tests/e2e/run_e2e.py --bin $(BIN) --pending $(PENDING) $(ONLY_ARG); \
	   echo $$? > $(BUILD)/e2e.rc; } | tee $(BUILD)/e2e.log
	@echo "=== unit + e2e ==="
	@cat $(BUILD)/unit.log $(BUILD)/e2e.log | awk -F'[ :,]+' \
	  '/^[0-9]+ total:/ { t+=$$1; p+=$$3; pe+=$$5; f+=$$7; u+=$$9; s+=$$11 } \
	   END { printf "%d total: %d pass, %d pending, %d fail, %d unexpected, %d skip\n", t, p, pe, f, u, s }'
	@[ "$$(cat $(BUILD)/unit.rc)" = 0 ] && [ "$$(cat $(BUILD)/e2e.rc)" = 0 ]

lint:
	$(PYTHON) scripts/lint_includes.py
	sh scripts/lint_forbidden.sh
	CC="$(CC)" HTTPD_CFLAGS="$(BASEFLAGS) $(OSFLAGS)" sh scripts/check_headers.sh
	@if command -v $(CLANG_FORMAT) >/dev/null 2>&1; then \
	  if [ -n "$(strip $(FMT_FILES))" ]; then \
	    $(CLANG_FORMAT) --dry-run --Werror $(FMT_FILES) || exit 1; \
	  fi; \
	  echo "clang-format: ok"; \
	else \
	  echo "clang-format: 설치되어 있지 않아 건너뜀"; \
	fi

asan:
	$(MAKE) BUILD=build-asan \
	  EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
	  EXTRA_LDFLAGS="-fsanitize=address,undefined" test

# 기획서 ID 밖의 보조 스위트 (tests/robust/README.md): 성질 검사와 공격·경계 E2E를 두 엔진으로,
# ASan + UBSan 빌드에서 돌린다. SEED로 무작위 입력열을 바꾼다.
robust:
	$(MAKE) BUILD=build-asan \
	  EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
	  EXTRA_LDFLAGS="-fsanitize=address,undefined" robust-run

robust-run: $(BIN) $(PROPS_BIN)
	$(PROPS_BIN) $(SEED)
	$(PYTHON) tests/e2e/run_e2e.py --bin $(BIN) --tests 'tests/robust/test_*.py' $(ONLY_ARG)
	HTTPD_ENGINE=loop $(PYTHON) tests/e2e/run_e2e.py --bin $(BIN) --tests 'tests/robust/test_*.py' \
	  $(ONLY_ARG)

clean:
	rm -rf build build-asan
