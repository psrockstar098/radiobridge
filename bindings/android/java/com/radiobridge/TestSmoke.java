// RadioBridge — JNI smoke test (Phase 3).
//
// Run on a desktop JVM: loads libradiobridge_jni and drives the mock
// backend end to end through the Java wrapper. Exits non-zero on any
// failed check, so CTest reports it. This is the compiled + run proof
// for the JNI bridge; it proves nothing about Android Framework APIs,
// which need a device (see README.md).
package com.radiobridge;

import java.util.ArrayList;
import java.util.List;

public final class TestSmoke {
    private static int failures = 0;

    private static void check(boolean cond, String what) {
        if (cond) {
            System.out.println("PASS: " + what);
        } else {
            System.out.println("FAIL: " + what);
            failures++;
        }
    }

    private static void waitFor(Runnable probe, long timeoutMs,
                                String what) throws Exception {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            try {
                probe.run();
                return;
            } catch (AssertionError e) {
                Thread.sleep(50);
            }
        }
        probe.run();
    }

    public static void main(String[] args) throws Exception {
        System.out.println("version: " + RadioBridge.version());
        check(RadioBridge.version() != null &&
                !RadioBridge.version().isEmpty(),
                "version non-empty");

        RadioBridge rb = new RadioBridge(RadioBridge.BACKEND_MOCK);
        try {
            String caps = rb.capabilities();
            System.out.println("caps: " + caps);
            check(caps != null && caps.contains("mock"),
                    "capabilities report mock backend");

            // Scan: expect the 3 mock devices, incl. RB Heart Rate.
            final List<String> found = new ArrayList<>();
            final List<Integer> rssis = new ArrayList<>();
            long scanOp = rb.bleScanStart((address, name, rssi) -> {
                synchronized (found) {
                    if (!found.contains(address)) {
                        found.add(address);
                        rssis.add(rssi);
                        System.out.println("device: " + address + " " +
                                name + " " + rssi);
                    }
                }
            });
            waitFor(() -> {
                synchronized (found) {
                    if (found.size() < 3) {
                        throw new AssertionError("waiting for 3 devices");
                    }
                }
            }, 10000, "scan 3 devices");
            synchronized (found) {
                check(found.size() == 3, "scan found 3 devices");
                check(found.contains("AA:BB:CC:00:00:01"),
                        "scan found RB Heart Rate");
            }
            int stopSt = rb.bleScanStop(scanOp);
            check(stopSt == 0, "scan stop OK (status " + stopSt + ")");

            // Connect to the heart-rate device.
            final int[] connState = new int[]{-1};
            rb.bleConnect("AA:BB:CC:00:00:01",
                    (address, state, status) -> {
                        synchronized (connState) {
                            connState[0] = state;
                            System.out.println("conn: " + address +
                                    " state=" + state +
                                    " status=" + status);
                        }
                    });
            waitFor(() -> {
                synchronized (connState) {
                    if (connState[0] != RadioBridge.STATE_CONNECTED) {
                        throw new AssertionError("waiting for CONNECTED");
                    }
                }
            }, 10000, "connect");
            synchronized (connState) {
                check(connState[0] == RadioBridge.STATE_CONNECTED,
                        "connected to heart-rate device");
            }

            // Read battery (180F/2A19) -> [87].
            byte[] battery = rb.bleRead("AA:BB:CC:00:00:01", "180F",
                    "2A19");
            check(battery != null && battery.length == 1 &&
                    battery[0] == 87,
                    "battery read [87]");

            // Read notify-only char (180D/2A37) -> null (not readable).
            byte[] hr = rb.bleRead("AA:BB:CC:00:00:01", "180D", "2A37");
            check(hr == null, "notify-only read returns null");

            // Wi-Fi status on the mock backend starts disconnected.
            String wifi = rb.wifiStatus();
            System.out.println("wifi: " + wifi);
            check(wifi != null && wifi.startsWith("0|"),
                    "wifi status starts disconnected");

            int discSt = rb.bleDisconnect("AA:BB:CC:00:00:01");
            check(discSt == 0, "disconnect OK (status " + discSt + ")");
        } finally {
            rb.close();
        }

        if (failures == 0) {
            System.out.println("ALL PASS");
        } else {
            System.out.println(failures + " FAILURES");
            System.exit(1);
        }
    }
}
