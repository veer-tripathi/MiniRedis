#!/usr/bin/env bash
# =============================================================================
# test.sh — end-to-end test suite for redis_modular
#
# Runs all commands, checks outputs, prints a summary at the end.
# Exit code: 0 = all passed, 1 = one or more failed.
#
# Usage: ./test.sh
# =============================================================================

set -u

CLIENT=./client
SERVER=./server
SUBSCRIBER=./subscriber
SERVER_PID=""
AOF_FILE="appendonly.aof"

PASS=0
FAIL=0
FAILURES=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

start_server() {
    rm -f "$AOF_FILE"
    $SERVER &>/dev/null &
    SERVER_PID=$!
    sleep 0.3   # give the server time to bind
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
}

# check <test_name> <expected_substring> <actual_output>
check() {
    local name="$1"
    local expected="$2"
    local actual="$3"

    if echo "$actual" | grep -qF "$expected"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAILURES="$FAILURES\n  FAIL: $name\n       expected: $expected\n       got:      $actual"
    fi
}

# run a client command and return its output
c() {
    $CLIENT "$@" 2>/dev/null
}

# ---------------------------------------------------------------------------
# Cleanup on exit
# ---------------------------------------------------------------------------
cleanup() {
    stop_server
    rm -f "$AOF_FILE" /tmp/sub_output.txt
    # kill any leftover subscriber
    kill "$SUB_PID" 2>/dev/null || true
}
trap cleanup EXIT

SUB_PID=""

# ===========================================================================
# START
# ===========================================================================

echo "========================================"
echo " redis_modular test suite"
echo "========================================"
echo ""

# ---------------------------------------------------------------------------
# 1. String commands
# ---------------------------------------------------------------------------
echo "[1] String commands"
start_server

check "set returns nil"       "(nil)"  "$(c set foo bar)"
check "get existing key"      '"bar"'  "$(c get foo)"
check "get missing key"       "(nil)"  "$(c get nosuchkey)"
check "set overwrites"        "(nil)"  "$(c set foo baz)"
check "get after overwrite"   '"baz"'  "$(c get foo)"
check "del existing key"      "(int) 1" "$(c del foo)"
check "del missing key"       "(int) 0" "$(c del foo)"
check "get after del"         "(nil)"  "$(c get foo)"
check "type error get"        "(err)"  "$(c set k v && c rpush k x && c get k)"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 2. TTL commands
# ---------------------------------------------------------------------------
echo "[2] TTL commands"
start_server

c set mykey hello >/dev/null
check "ttl no expiry"         "(int) -1"  "$(c ttl mykey)"
check "ttl missing key"       "(int) -2"  "$(c ttl nosuchkey)"
check "expire returns 1"      "(int) 1"   "$(c expire mykey 500)"
check "ttl positive after set" "(int)"    "$(c ttl mykey)"

# Wait for expiry
sleep 0.7
check "get expired key"       "(nil)"     "$(c get mykey)"
check "ttl expired key"       "(int) -2"  "$(c ttl mykey)"

# persist
c set p hello >/dev/null
c expire p 10000 >/dev/null
check "persist returns 1"     "(int) 1"   "$(c persist p)"
check "ttl after persist"     "(int) -1"  "$(c ttl p)"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 3. KEYS command
# ---------------------------------------------------------------------------
echo "[3] KEYS command"
start_server

c set a 1 >/dev/null
c set b 2 >/dev/null
c set c 3 >/dev/null
check "keys returns 3 items"  "len=3"   "$(c keys)"

# Expire one key and wait
c expire b 300 >/dev/null
sleep 0.5
check "keys excludes expired" "len=2"   "$(c keys)"
check "keys has a"            '"a"'     "$(c keys)"
check "keys has c"            '"c"'     "$(c keys)"
check "keys no b"             ""        "$(c keys | grep -v 'len\|arr\|\"a\"\|\"c\"')"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 4. List commands
# ---------------------------------------------------------------------------
echo "[4] List commands"
start_server

check "rpush creates list"    "(int) 1"  "$(c rpush msgs hello)"
check "rpush appends"         "(int) 2"  "$(c rpush msgs world)"
check "lpush prepends"        "(int) 3"  "$(c lpush msgs hi)"
check "llen"                  "(int) 3"  "$(c llen msgs)"
check "llen missing key"      "(int) 0"  "$(c llen nosuchkey)"

check "lrange all"            "len=3"    "$(c lrange msgs 0 -1)"
check "lrange first elem"     '"hi"'     "$(c lrange msgs 0 0)"
check "lrange last elem"      '"world"'  "$(c lrange msgs -1 -1)"
check "lrange middle"         '"hello"'  "$(c lrange msgs 1 1)"
check "lrange empty range"    "len=0"    "$(c lrange msgs 5 10)"

check "lpop returns front"    '"hi"'     "$(c lpop msgs)"
check "rpop returns back"     '"world"'  "$(c rpop msgs)"
check "llen after pops"       "(int) 1"  "$(c llen msgs)"

# Key should be deleted when list becomes empty
c lpop msgs >/dev/null
check "key gone after empty"  "(nil)"    "$(c get msgs)"
check "llen gone key"         "(int) 0"  "$(c llen msgs)"

# Type error
c set strkey val >/dev/null
check "lpush type error"      "(err)"    "$(c lpush strkey x)"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 5. Sorted-set commands
# ---------------------------------------------------------------------------
echo "[5] Sorted-set commands"
start_server

check "zadd alice"    "(int) 1"    "$(c zadd board 100 alice)"
check "zadd bob"      "(int) 1"    "$(c zadd board 200 bob)"
check "zadd carol"    "(int) 1"    "$(c zadd board 150 carol)"
check "zscore alice"  "(dbl) 100"  "$(c zscore board alice)"
check "zscore bob"    "(dbl) 200"  "$(c zscore board bob)"
check "zscore missing" "(nil)"     "$(c zscore board nobody)"

# Update score
check "zadd update"   "(int) 0"    "$(c zadd board 300 alice)"
check "zscore updated" "(dbl) 300" "$(c zscore board alice)"

check "zrem bob"      "(int) 1"    "$(c zrem board bob)"
check "zrem missing"  "(int) 0"    "$(c zrem board nobody)"
check "zscore removed" "(nil)"     "$(c zscore board bob)"

check "zquery result" "carol"      "$(c zquery board 0 "" 0 10)"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 6. Pub/Sub
# ---------------------------------------------------------------------------
echo "[6] Pub/Sub"
start_server

# Start subscriber in background, writing messages to a temp file
rm -f /tmp/sub_output.txt
$SUBSCRIBER sports /tmp/sub_output.txt &
SUB_PID=$!
sleep 0.3   # give subscriber time to subscribe

# Publish messages
check "publish to subscriber"  "(int) 1"  "$(c publish sports goal)"
check "publish second message" "(int) 1"  "$(c publish sports offside)"
check "publish no subscribers" "(int) 0"  "$(c publish empty nothing)"
sleep 0.3   # give subscriber time to receive and write

# Check subscriber received the messages
check "subscriber got goal"    "goal"     "$(cat /tmp/sub_output.txt 2>/dev/null)"
check "subscriber got offside" "offside"  "$(cat /tmp/sub_output.txt 2>/dev/null)"

# Kill subscriber — server should not crash
kill $SUB_PID 2>/dev/null
wait $SUB_PID 2>/dev/null
SUB_PID=""
sleep 0.2

# Publish after subscriber gone — should return 0, not crash
check "publish after disconnect" "(int) 0" "$(c publish sports test)"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 7. AOF persistence
# ---------------------------------------------------------------------------
echo "[7] AOF persistence"
start_server

# Write some data
c set persist_key hello >/dev/null
c rpush persist_list a >/dev/null
c rpush persist_list b >/dev/null
c rpush persist_list c >/dev/null
c zadd persist_zset 1.0 alpha >/dev/null
c zadd persist_zset 2.0 beta  >/dev/null
c set will_be_deleted gone >/dev/null
c del will_be_deleted >/dev/null

# Verify data exists before restart
check "before restart: string"  '"hello"'  "$(c get persist_key)"
check "before restart: list"    "len=3"    "$(c lrange persist_list 0 -1)"
check "before restart: zset"    "(dbl) 1"  "$(c zscore persist_zset alpha)"

# Check AOF file was created and has content
check "aof file exists"         "set"      "$(cat $AOF_FILE 2>/dev/null | head -1)"

# Restart server — it will replay the AOF
stop_server
start_server

# Verify data survived the restart
check "after restart: string"   '"hello"'  "$(c get persist_key)"
check "after restart: list len" "len=3"    "$(c lrange persist_list 0 -1)"
check "after restart: list[0]"  '"a"'      "$(c lrange persist_list 0 0)"
check "after restart: zset"     "(dbl) 1"  "$(c zscore persist_zset alpha)"
check "after restart: deleted"  "(nil)"    "$(c get will_be_deleted)"

stop_server
echo ""

# ---------------------------------------------------------------------------
# 8. Error handling
# ---------------------------------------------------------------------------
echo "[8] Error handling"
start_server

check "unknown command"      "(err)"  "$(c foobar)"
check "wrong arg count"      "(err)"  "$(c get)"
check "type err set on list" "(err)"  "$(c rpush tkey x && c set tkey val)"
check "type err get on list" "(err)"  "$(c get tkey)"

stop_server
echo ""

# ===========================================================================
# SUMMARY
# ===========================================================================

TOTAL=$((PASS + FAIL))
echo "========================================"
echo " Results: $PASS/$TOTAL passed"
echo "========================================"

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failures:"
    printf "$FAILURES\n"
    echo ""
    exit 1
fi

echo ""
echo "All tests passed."
exit 0