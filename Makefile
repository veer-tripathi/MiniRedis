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
    src/protocol/serializer.cpp \
    src/utils/buffer.cpp \
    src/utils/logging.cpp

# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------
CLIENT_SRCS := client.cpp

# ---------------------------------------------------------------------------
# Tests (Commented out)
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
.PHONY: all clean # test

# Removed test_avl and test_protocol from the default 'all' target
all: server client 

server: $(SERVER_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built server"

client: $(CLIENT_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built client"

test_avl: $(TEST_AVL_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built test_avl"

test_protocol: $(TEST_PROTO_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	@echo "[OK] built test_protocol"

test: test_avl test_protocol
	@echo ""
	@echo "=== Running test_avl ==="
	./test_avl
	@echo ""
	@echo "=== Running test_protocol ==="
	./test_protocol

# Removed test executables from the clean target
clean:
	rm -f server client