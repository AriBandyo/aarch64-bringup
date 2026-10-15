#!/usr/bin/env python3
"""
Automated tests for aarch64-bringup.

Boots the kernel in QEMU under several configurations (entry at EL1, EL2 and
EL3, different CPU models, core counts and RAM sizes), drives the debug shell
over the emulated serial port, and checks the results. Also runs a scripted
GDB session against QEMU's gdbstub.

Usage: python3 tests/run_tests.py [--kernel build/kernel.elf] [--only NAME]
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

PROMPT = "bringup> "
QEMU = os.environ.get("QEMU", "qemu-system-aarch64")
UNMAPPED = "0x4000000000"


class TestFailure(Exception):
    pass


class Qemu:
    """A QEMU instance whose serial console is attached to our stdin/stdout."""

    def __init__(self, kernel, machine, cpu, smp, mem, extra=(), log_path=None):
        self.cmd = [QEMU, "-M", machine, "-cpu", cpu, "-smp", str(smp), "-m", mem,
                    "-display", "none", "-serial", "stdio", "-monitor", "none",
                    "-kernel", kernel, *extra]
        self.proc = subprocess.Popen(self.cmd, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.out = ""
        self.pos = 0
        self.lock = threading.Lock()
        self.log_path = log_path
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        fd = self.proc.stdout.fileno()
        while True:
            try:
                data = os.read(fd, 4096)
            except OSError:
                break
            if not data:
                break
            with self.lock:
                self.out += data.decode("utf-8", "replace").replace("\r", "")

    def expect(self, pattern, timeout=10.0):
        """Wait for regex `pattern` after the current position; consume through it."""
        regex = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                m = regex.search(self.out, self.pos)
                if m:
                    self.pos = m.end()
                    return m
            if self.proc.poll() is not None and not self.reader.is_alive():
                break
            time.sleep(0.01)
        tail = self.out[self.pos:][-600:]
        raise TestFailure(f"timed out waiting for /{pattern}/; recent output:\n{tail}")

    def send(self, line):
        self.proc.stdin.write(line.encode() + b"\r")
        self.proc.stdin.flush()

    def command(self, line, timeout=10.0):
        """Run a shell command; return its output (text before the next prompt)."""
        self.send(line)
        self.expect(re.escape(line) + r"\n", timeout)
        start = self.pos
        self.expect(re.escape(PROMPT), timeout)
        with self.lock:
            return self.out[start:self.pos - len(PROMPT)]

    def wait_exit(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.reader.join(timeout=2)
        if self.log_path:
            with open(self.log_path, "w") as f:
                f.write("$ " + " ".join(self.cmd) + "\n\n" + self.out)


class Runner:
    def __init__(self):
        self.passed = 0
        self.failed = []

    def check(self, name, fn):
        try:
            fn()
        except (TestFailure, AssertionError) as e:
            self.failed.append(name)
            msg = str(e).strip() or "assertion failed"
            print(f"  FAIL  {name}\n        " + msg.replace("\n", "\n        "))
            return False
        self.passed += 1
        print(f"  pass  {name}")
        return True


def require(cond, msg):
    if not cond:
        raise TestFailure(msg)


def has(text, pattern, what):
    m = re.search(pattern, text)
    require(m, f"{what}: /{pattern}/ not found in:\n{text}")
    return m


# ---------------------------------------------------------------------------

CONFIGS = [
    # name,        machine,                    cpu,          smp, mem,    entry EL, conduit, part
    ("el1-a53",    "virt",                     "cortex-a53", 4,   "128M", 1, "hvc", "Cortex-A53"),
    ("el2-a72",    "virt,virtualization=on",   "cortex-a72", 4,   "128M", 2, "smc", "Cortex-A72"),
    ("el3-a53",    "virt,secure=on",           "cortex-a53", 4,   "128M", 3, None,  "Cortex-A53"),
    ("el1-a57-1c", "virt",                     "cortex-a57", 1,   "1G",   1, "hvc", "Cortex-A57"),
]


def run_config(r, kernel, logdir, cfg):
    name, machine, cpu, smp, mem, el, conduit, part = cfg
    smp_expected = smp if conduit else 1
    print(f"\n[{name}] -M {machine} -cpu {cpu} -smp {smp} -m {mem}")
    q = Qemu(kernel, machine, cpu, smp, mem, log_path=os.path.join(logdir, f"{name}.log"))
    try:
        def boot():
            m = q.expect(r"Bring-up complete: (\d+)/(\d+) CPUs online", 20)
            require(int(m.group(1)) == smp_expected and int(m.group(2)) == smp,
                    f"expected {smp_expected}/{smp} CPUs online, got {m.group(0)}")
            q.expect(re.escape(PROMPT))
        if not r.check("boots to shell", boot):
            return

        info = q.command("info")
        r.check(f"entered at EL{el}, runs at EL1",
                lambda: has(info, rf"entered at EL{el}, running at EL1", "info"))
        r.check(f"CPU identified as {part} from MIDR_EL1",
                lambda: has(info, rf"Arm {part} r\d+p\d+", "info"))
        r.check("device tree found and parsed",
                lambda: has(info, r"DTB:\s+0x40000000", "info"))
        mib = 1024 if mem == "1G" else 128
        r.check(f"RAM size ({mib} MiB) discovered from device tree",
                lambda: has(info, rf"RAM:\s+0x40000000 - 0x[0-9a-f]+ \({mib} MiB\)", "info"))
        r.check("MMU and caches enabled",
                lambda: has(info, r"MMU:\s+on.*D-cache on, I-cache on", "info"))
        if conduit:
            r.check(f"PSCI found via {conduit}",
                    lambda: has(info, rf"PSCI:\s+v\d+\.\d+ via {conduit}", "info"))
        else:
            r.check("PSCI correctly unavailable after EL3 entry",
                    lambda: has(info, r"PSCI:\s+not available", "info"))

        dt = q.command("dt")
        r.check("dt lists UART and GIC nodes", lambda: (
            has(dt, r"pl011@9000000\s+\[arm,pl011\]", "dt"),
            has(dt, r"intc@8000000\s+\[arm,cortex-a15-gic\]", "dt")))

        ids = q.command("md 0x09000fe0 8")
        r.check("PL011 PrimeCell ID registers read back", lambda: (
            has(ids, r"00000011 00000010 00000014 00000000", "md"),
            has(ids, r"0000000d 000000f0 00000005 000000b1", "md")))

        def mmu_attrs():
            has(q.command("pt 0x40200000"), r"PA 0x0+40200000\s+attr 0xff \(Normal", "pt RAM")
            has(q.command("pt 0x09000000"), r"attr 0x04 \(Device-nGnRE\)", "pt UART")
            has(q.command(f"pt {UNMAPPED}"), r"translation fault, level 1", "pt unmapped")
        r.check("AT S1E1R: RAM Normal, MMIO Device, hole unmapped", mmu_attrs)

        out = q.command("fault")
        r.check("data abort decoded and recovered", lambda: (
            has(out, r"Data abort \(current EL\) \(EC=0x25", "fault"),
            has(out, r"cause: translation fault, level 1", "fault"),
            has(out, r"access: read", "fault"),
            has(out, r"FAR: 0x0000004000000000", "fault"),
            has(out, r"recovered", "fault")))

        out = q.command(f"md {UNMAPPED} 4")
        r.check("md survives an aborting address",
                lambda: has(out, r"<abort>", "md unmapped"))

        out = q.command("svc")
        r.check("SVC exception taken and returned", lambda: (
            has(out, r"SVC #0x42 taken at EL1", "svc"),
            has(out, r"returned from SVC handler", "svc")))

        out = q.command("undef")
        r.check("undefined instruction decoded and recovered", lambda: (
            has(out, r"Undefined instruction \(EC=0x00", "undef"),
            has(out, r"returned from undefined-instruction handler", "undef")))

        def timer_rate():
            time.sleep(1.0)
            out = q.command("uptime")
            m = has(out, r"up (\d+)\.(\d+) s, (\d+) timer ticks at (\d+) Hz", "uptime")
            secs = int(m.group(1)) + int(m.group(2)) / 1e6
            ticks, hz = int(m.group(3)), int(m.group(4))
            rate = ticks / secs
            require(0.85 * hz < rate <= 1.02 * hz,
                    f"tick rate {rate:.1f}/s does not match {hz} Hz")
        r.check("timer interrupt ticks at the programmed rate", timer_rate)

        def irq_counts():
            out = q.command("irqs")
            has(out, r"\b27\s+[1-9]\d*\s+arch timer", "irqs")
            has(out, r"\b33\s+[1-9]\d*\s+PL011 UART RX", "irqs")
        r.check("timer PPI and UART SPI delivered through the GIC", irq_counts)

        out = q.command("smp 50000", timeout=60)
        exp = smp_expected * 50000
        r.check(f"spinlock counter exact across {smp_expected} CPU(s)",
                lambda: has(out, rf"spinlock test: {exp}/{exp} increments .*\(OK\)", "smp"))
        if smp_expected > 1:
            r.check("IPI (SGI) answered by every secondary",
                    lambda: has(out, rf"IPI test: {smp_expected - 1}/{smp_expected - 1}", "smp"))

        if conduit:
            def poweroff():
                q.send("poweroff")
                require(q.wait_exit(10) == 0, "QEMU did not exit after PSCI SYSTEM_OFF")
            r.check("PSCI SYSTEM_OFF powers the machine off", poweroff)
        else:
            r.check("poweroff refuses without PSCI",
                    lambda: has(q.command("poweroff"), r"PSCI not available", "poweroff"))
    finally:
        q.close()


def run_crash(r, kernel, logdir):
    print("\n[crash] unrecoverable abort")
    q = Qemu(kernel, "virt", "cortex-a53", 2, "128M",
             log_path=os.path.join(logdir, "crash.log"))
    try:
        def crash():
            q.expect(re.escape(PROMPT), 20)
            q.send("crash")
            q.expect(r"Unhandled exception on CPU0: Sync from current EL, SP_ELx")
            q.expect(r"access: write")
            q.expect(r"x30 [0-9a-f]{16}")
            q.expect(r"SYSTEM HALTED")
        r.check("unhandled abort prints decoded fault + register dump and halts", crash)
    finally:
        q.close()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run_gdb(r, kernel, logdir):
    print("\n[gdb] scripted debug session over QEMU's gdbstub")
    gdb = shutil.which("gdb-multiarch") or shutil.which("aarch64-linux-gnu-gdb")
    if not gdb:
        print("  skip  gdb-multiarch not installed")
        return
    port = free_port()
    q = Qemu(kernel, "virt", "cortex-a53", 4, "128M",
             extra=("-S", "-gdb", f"tcp:127.0.0.1:{port}"),
             log_path=os.path.join(logdir, "gdb-qemu.log"))
    try:
        time.sleep(0.5)
        script = [
            f"target remote 127.0.0.1:{port}",
            # QEMU -S halts before the first instruction: pc is the reset entry.
            "printf \"AT_START pc=%#lx\\n\", $pc",
            "break kmain",
            "break secondary_main",
            "continue",                 # -> kmain
            "printf \"AT_KMAIN boot_el=%lu\\n\", $x1",
            "printf \"CURRENT_EL=%lu\\n\", ($cpsr >> 2) & 3",
            "continue",                 # -> secondary_main on some CPU
            "printf \"AT_SECONDARY idx=%lu\\n\", $x0",
            "kill",
        ]
        args = [gdb, "-q", "-batch", "-nx", kernel]
        for line in script:
            args += ["-ex", line]
        res = subprocess.run(args, capture_output=True, text=True, timeout=60)
        out = res.stdout + res.stderr
        with open(os.path.join(logdir, "gdb.log"), "w") as f:
            f.write(out)

        r.check("halted at the reset entry (_start) under -S",
                lambda: has(out, r"AT_START pc=0x40200000", "gdb"))
        r.check("breakpoint at kmain: entered at EL1, x1 holds boot EL", lambda: (
            has(out, r"AT_KMAIN boot_el=1", "gdb"),
            has(out, r"CURRENT_EL=1", "gdb")))
        r.check("breakpoint in secondary_main on a PSCI-started core",
                lambda: has(out, r"AT_SECONDARY idx=[1-3]", "gdb"))
    finally:
        q.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default="build/kernel.elf")
    ap.add_argument("--only", help="run only configs whose name contains this")
    args = ap.parse_args()

    if not os.path.exists(args.kernel):
        sys.exit(f"{args.kernel} not found; run make first")
    logdir = os.path.join(os.path.dirname(args.kernel), "test-logs")
    os.makedirs(logdir, exist_ok=True)

    r = Runner()
    t0 = time.time()
    for cfg in CONFIGS:
        if not args.only or args.only in cfg[0]:
            run_config(r, args.kernel, logdir, cfg)
    if not args.only or args.only == "crash":
        run_crash(r, args.kernel, logdir)
    if not args.only or args.only == "gdb":
        run_gdb(r, args.kernel, logdir)

    total = r.passed + len(r.failed)
    print(f"\n{r.passed}/{total} checks passed in {time.time() - t0:.1f}s "
          f"(serial logs in {logdir}/)")
    if r.failed:
        print("failed: " + ", ".join(r.failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
