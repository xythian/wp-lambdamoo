.PHONY: all test test-unit test-integration test-server-integration \
	test-sanitize benchmark clean

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2
SANITIZE_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
OPENSSL ?= openssl

all: test_protocol edge backend integration_test moo_integration_test session_benchmark

test_protocol: protocol.c protocol.h test_protocol.c
	$(CC) $(CFLAGS) -o $@ protocol.c test_protocol.c

edge: protocol.c protocol.h edge.c
	$(CC) $(CFLAGS) -pthread -o $@ protocol.c edge.c -lssl -lcrypto

backend: protocol.c protocol.h backend.c
	$(CC) $(CFLAGS) -pthread -o $@ protocol.c backend.c

integration_test: integration_test.c
	$(CC) $(CFLAGS) -o $@ integration_test.c -lssl -lcrypto

moo_integration_test: moo_integration_test.c
	$(CC) $(CFLAGS) -o $@ moo_integration_test.c -lssl -lcrypto

session_benchmark: session_benchmark.c
	$(CC) $(CFLAGS) -o $@ session_benchmark.c -lssl -lcrypto

test: test-unit test-integration

test-unit: test_protocol
	./test_protocol

test-tmp/cert.pem test-tmp/key.pem:
	mkdir -p test-tmp
	$(OPENSSL) req -x509 -newkey rsa:2048 -nodes -days 1 \
		-subj /CN=localhost -addext subjectAltName=DNS:localhost \
		-keyout test-tmp/key.pem -out test-tmp/cert.pem >/dev/null 2>&1

test-integration: all test-tmp/cert.pem test-tmp/key.pem
	./integration_test ./edge ./backend test-tmp/cert.pem test-tmp/key.pem test-tmp

test-server-integration: all test-tmp/cert.pem test-tmp/key.pem
	./moo_integration_test ../../moo ./edge ../../Minimal.db \
		test-tmp/cert.pem test-tmp/key.pem test-tmp

benchmark: edge backend session_benchmark test-tmp/cert.pem test-tmp/key.pem
	./session_benchmark ./edge ./backend test-tmp/cert.pem test-tmp/key.pem test-tmp

test-sanitize:
	$(CC) $(CFLAGS) $(SANITIZE_FLAGS) -o test_protocol protocol.c test_protocol.c
	ASAN_OPTIONS=detect_leaks=1 ./test_protocol

clean:
	rm -f test_protocol edge backend integration_test moo_integration_test session_benchmark
	rm -rf test-tmp
