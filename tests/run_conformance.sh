#!/bin/sh
# Cross-platform conformance runner (Phase 5).
# Usage: run_conformance.sh <test_conformance_bin> [fake_bus_bin]
#
# Always runs the mock conformance. When a fake-bus binary is given,
# it also starts a private dbus-daemon + the fake BlueZ/NetworkManager
# service and runs the Linux conformance against it. The Linux run is
# SKIPPED (not failed) when no fake-bus binary is available, so the
# suite stays green on machines without libdbus.
set -u

TEST_BIN="$1"
FAKE_BIN="${2:-}"
FAIL=0

"$TEST_BIN" mock || FAIL=1

if [ -n "$FAKE_BIN" ] && [ -x "$FAKE_BIN" ]; then
    DAEMON_OUT=$(mktemp)
    FAKE_PID=""
    DAEMON_PID=""
    cleanup() {
        if [ -n "$FAKE_PID" ]; then kill "$FAKE_PID" 2>/dev/null; fi
        if [ -n "$DAEMON_PID" ]; then kill "$DAEMON_PID" 2>/dev/null; fi
        rm -f "$DAEMON_OUT"
    }
    trap cleanup EXIT

    dbus-daemon --session --nofork --print-address \
        >"$DAEMON_OUT" 2>/dev/null &
    DAEMON_PID=$!
    ADDR=""
    i=0
    while [ $i -lt 50 ]; do
        if [ -s "$DAEMON_OUT" ]; then
            ADDR=$(cat "$DAEMON_OUT")
            break
        fi
        sleep 0.1
        i=$((i + 1))
    done
    if [ -z "$ADDR" ]; then
        echo "conformance: could not start dbus-daemon, linux run skipped"
    else
        RB_DBUS_ADDRESS="$ADDR" "$FAKE_BIN" &
        FAKE_PID=$!
        # Wait for both fake service names to appear.
        READY=0
        i=0
        while [ $i -lt 100 ]; do
            if DBUS_SESSION_BUS_ADDRESS="$ADDR" dbus-send --session \
                    --print-reply --dest=org.freedesktop.DBus \
                    /org/freedesktop/DBus \
                    org.freedesktop.DBus.NameHasOwner \
                    string:org.bluez 2>/dev/null | grep -q "boolean true" \
               && DBUS_SESSION_BUS_ADDRESS="$ADDR" dbus-send --session \
                    --print-reply --dest=org.freedesktop.DBus \
                    /org/freedesktop/DBus \
                    org.freedesktop.DBus.NameHasOwner \
                    string:org.freedesktop.NetworkManager 2>/dev/null \
                    | grep -q "boolean true"; then
                READY=1
                break
            fi
            sleep 0.1
            i=$((i + 1))
        done
        if [ "$READY" != "1" ]; then
            echo "conformance: fake bus service did not appear, linux run skipped"
        else
            RB_DBUS_ADDRESS="$ADDR" "$TEST_BIN" linux || FAIL=1
        fi
    fi
else
    echo "conformance: no fake bus binary, linux run skipped"
fi

exit $FAIL
