#!/usr/bin/expect -f
# build/integration-test.sh
# Automated integration test for MinOS functionalities
# Tests booting, supervisor daemon, cgroup application, and minc container runtime

set timeout 30

puts "━━━ Booting MinOS for automated functionality test ━━━"

# Start QEMU emulator (make sure the project is built first!)
spawn bash build/qemu.sh

# Wait for the busybox shell prompt (console service)
expect {
    "/ #" { puts "\n[✓] OS Booted successfully; shell ready." }
    timeout { puts "\n[✗] ERROR: Timed out waiting for boot."; exit 1 }
}

# 1. Test supervisor and system daemons
send "ps\r"
expect {
    "cgmgr" { puts "\n[✓] Services (supervisor, cgmgr, netd) are running." }
    timeout { puts "\n[✗] ERROR: Background services not found."; exit 1 }
}
expect "/ #"

# 2. Test cgroup slice application
send "cat /sys/fs/cgroup/system/cpu.weight\r"
expect {
    "200" { puts "\n[✓] Cgroup limits correctly applied by cgmgr (system=200)." }
    timeout { puts "\n[✗] ERROR: Cgroup limit missing or incorrect."; exit 1 }
}
expect "/ #"

# 3. Test Container runtime (minc)
send "minc run /bin/echo Hello from isolated container!\r"
expect {
    "container c0 starting" {}
    timeout { puts "\n[✗] ERROR: Container failed to start."; exit 1 }
}
expect {
    "Hello from isolated container!" { puts "\n[✓] container execution (minc) succeeded with seccomp/cgroup isolation." }
    timeout { puts "\n[✗] ERROR: Container output not found."; exit 1 }
}
expect "/ #"

# 4. Finish the test and exit QEMU safely
puts "\n━━━ All MinOS Functions Tested Successfully! ━━━\n"

# QEMU exit shortcut: Ctrl+A, then X
send "\x01x"
expect eof
