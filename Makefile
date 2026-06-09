CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -Isrc

# ---------------------------------------------------------------------------
# Server
# ---------------------------------------------------------------------------
SERVER_SRCS := \
    src/networking/event_loop.cpp \
    src/networking/socket_io.cpp \
    src/protocol/protocol.cpp \
    src/protocol/serializer.cpp \
    src/storage/avl.cpp \
    src/storage/h_map.cpp \
    src/storage/z_set.cpp \
    src/storage/commands.cpp \
    src/persistence/persistence.cpp \
    src/utils/buffer.cpp \
    src/utils/logging.cpp

# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------
CLIENT_SRCS := client.cpp

# ---------------------------------------------------------------------------
# Subscriber  (pub/sub test helper)
# ---------------------------------------------------------------------------
SUBSCRIBER_SRCS := subscriber.cpp

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
TEST_AVL_SRCS := \
    src/tests/test_offset.cpp \
    src/storage/avl.cpp

TEST_PROTO_SRCS := \
    src/tests/test_protocol.cpp \
    src/protocol/protocol.cpp \
    src/protocol/serializer.cpp \
    src/utils/buffer.cpp \
    src/utils/logging.cpp

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all clean test

all: server client subscriber

server: $(SERVER_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built server"

client: $(CLIENT_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built client"

subscriber: $(SUBSCRIBER_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built subscriber"

test_avl: $(TEST_AVL_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built test_avl"

test_protocol: $(TEST_PROTO_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built test_protocol"

# Build everything then run the test suite
test: all
	@bash test.sh

clean:
	rm -f server client subscriber test_avl test_protocol appendonly.aof