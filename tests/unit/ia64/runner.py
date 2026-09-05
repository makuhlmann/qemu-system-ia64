"""Deterministic QMP runner for IA-64 architectural microprograms."""

from __future__ import annotations

from dataclasses import dataclass
import os
import subprocess
import tempfile
import time
from typing import Callable, Iterable, Sequence

from .process import kill_process, terminate_process
from .qmp import QmpClient, QmpError
from .state import Ia64State, StateExpectation, parse_state


@dataclass(frozen=True)
class Completion:
    terminal_ip: int | None
    timeout_s: float = 2.0
    poll_initial_s: float = 0.001
    poll_max_s: float = 0.020
    predicate: Callable[[Ia64State], bool] | None = None


@dataclass(frozen=True)
class EncodedBundle:
    address: int
    low: int
    high: int


@dataclass(frozen=True)
class MemoryInitializer:
    address: int
    value: int
    size: int


@dataclass(frozen=True)
class ExpectedExit:
    returncode: int | None = None
    stderr_contains: tuple[str, ...] = ()


@dataclass(frozen=True)
class MicroProgram:
    name: str
    bundles: tuple[EncodedBundle, ...]
    entry: int
    expected: StateExpectation
    completion: Completion
    data: tuple[MemoryInitializer, ...] = ()
    machine_args: tuple[str, ...] = ()
    machine: str = "ia64-vpc"
    cpu: str | None = None
    smp: str = "1"
    memory: str | None = None
    expected_exit: ExpectedExit | None = None


@dataclass
class RunResult:
    state: Ia64State
    register_output: str
    extra_hmp: dict[str, str]
    stderr: str
    polls: int
    elapsed_s: float


def encode_bundles(
        bundles: Iterable[Sequence[int]],
        encoder: Callable[[int, int, int, int], tuple[int, int]],
) -> tuple[EncodedBundle, ...]:
    result = []
    for address, template, slot0, slot1, slot2 in bundles:
        low, high = encoder(template, slot0, slot1, slot2)
        result.append(EncodedBundle(address, low, high))
    return tuple(result)


def _loader_args(program: MicroProgram) -> list[str]:
    args: list[str] = []
    for bundle in program.bundles:
        args += [
            "-device",
            (f"loader,data=0x{bundle.low:016x},data-len=8,"
             f"addr={bundle.address}"),
            "-device",
            (f"loader,data=0x{bundle.high:016x},data-len=8,"
             f"addr={bundle.address + 8}"),
        ]
    for data in program.data:
        args += [
            "-device",
            (f"loader,data=0x{data.value:x},data-len={data.size},"
             f"addr={data.address}"),
        ]
    args += ["-device", f"loader,addr={program.entry},cpu-num=0"]
    return args


def _command(qemu: str, program: MicroProgram) -> list[str]:
    # The battery loads microprograms over the firmware's historical 1 MB
    # home and relies on the identity window there; pin the machine to the
    # unrelocated layout (the shipping firmware runs from the RAM-top
    # shadow by default since rework phase 2.2).
    machine = f"{program.machine},fw-relocate=off"
    if program.machine_args:
        machine += "," + ",".join(program.machine_args)
    command = [
        os.path.abspath(qemu),
        "-machine", machine,
        "-smp", program.smp,
        "-display", "none",
        "-serial", "none",
        "-monitor", "none",
        "-qmp", "stdio",
        "-S",
    ]
    if program.memory is not None:
        command += ["-m", program.memory]
    # The battery was authored against Montecito behaviour for every case
    # that does not name a model.  Pin it so the cases keep their meaning
    # independently of the machine's default CPU type.
    command += ["-cpu", program.cpu if program.cpu is not None
                else "montecito"]
    return command + _loader_args(program)


def run_microprogram(qemu: str, program: MicroProgram,
                     *, extra_hmp: tuple[str, ...] = ()) -> RunResult:
    if program.expected_exit is not None:
        raise ValueError("expected-exit programs use run_expected_exit()")
    if program.completion.terminal_ip is None and \
            program.completion.predicate is None:
        raise ValueError(f"{program.name}: completion condition is empty")

    started = time.monotonic()
    last_state: Ia64State | None = None
    last_registers = ""
    polls = 0
    stderr_text = ""

    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as stderr:
        proc = subprocess.Popen(
            _command(qemu, program),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=stderr,
            text=True,
            bufsize=1,
        )
        if proc.stdin is None or proc.stdout is None:
            kill_process(proc)
            raise RuntimeError("QEMU QMP pipes were not created")

        qmp: QmpClient | None = None
        interval = program.completion.poll_initial_s
        completed = False
        extras: dict[str, str] = {}
        try:
            qmp = QmpClient(proc.stdout, proc.stdin)
            qmp.execute("cont")
            # The completion timeout describes guest execution, not process
            # creation and the initial QMP handshake.
            deadline = time.monotonic() + program.completion.timeout_s
            while time.monotonic() < deadline:
                # This is bounded terminal-condition polling, not startup delay.
                time.sleep(interval)
                interval = min(interval * 2.0, program.completion.poll_max_s)

                remaining = max(0.001, deadline - time.monotonic())
                qmp.execute("stop", timeout_s=remaining)
                remaining = max(0.001, deadline - time.monotonic())
                qmp.wait_event("STOP", timeout_s=remaining)
                polls += 1
                remaining = max(0.001, deadline - time.monotonic())
                last_registers = qmp.hmp("info registers",
                                         timeout_s=remaining)
                last_state = parse_state(last_registers)

                terminal_reached = (
                    program.completion.terminal_ip is not None
                    and last_state.ip == program.completion.terminal_ip
                )
                if terminal_reached:
                    mismatches = program.expected.mismatches(last_state)
                    if mismatches:
                        raise RuntimeError(
                            f"{program.name}: reached terminal IP "
                            f"0x{last_state.ip:x}, but the terminal-state "
                            "expectation failed: " + ", ".join(mismatches) +
                            f"\nterminal state:\n{last_registers}")
                    if program.completion.predicate is not None and not \
                            program.completion.predicate(last_state):
                        raise RuntimeError(
                            f"{program.name}: reached terminal IP "
                            f"0x{last_state.ip:x}, but the terminal-state "
                            f"assertion failed\nterminal state:\n{last_registers}")
                    completed = True
                    for command in extra_hmp:
                        remaining = max(0.001, deadline - time.monotonic())
                        extras[command] = qmp.hmp(command,
                                                  timeout_s=remaining)
                    break
                if program.completion.terminal_ip is None and \
                        program.completion.predicate is not None and \
                        program.completion.predicate(last_state):
                    mismatches = program.expected.mismatches(last_state)
                    if mismatches:
                        raise RuntimeError(
                            f"{program.name}: completion predicate matched, "
                            "but the terminal-state expectation failed: " +
                            ", ".join(mismatches) +
                            f"\nterminal state:\n{last_registers}")
                    completed = True
                    for command in extra_hmp:
                        remaining = max(0.001, deadline - time.monotonic())
                        extras[command] = qmp.hmp(command,
                                                  timeout_s=remaining)
                    break
                remaining = max(0.001, deadline - time.monotonic())
                qmp.execute("cont", timeout_s=remaining)

            if not completed:
                detail = (last_registers if last_registers else
                          "<no architectural state captured>")
                raise RuntimeError(
                    f"{program.name}: terminal condition was not reached in "
                    f"{program.completion.timeout_s:.3f}s after {polls} polls\n"
                    f"last state:\n{detail}")
        except Exception as exc:
            if proc.poll() is None:
                kill_process(proc)
            stderr.seek(0)
            stderr_text = stderr.read()
            diagnostics = [str(exc)]
            if last_registers and "last state:" not in str(exc) and \
                    "terminal state:" not in str(exc):
                diagnostics.append(f"last state:\n{last_registers}")
            if stderr_text:
                diagnostics.append(f"QEMU stderr:\n{stderr_text}")
            if len(diagnostics) > 1:
                raise RuntimeError("\n".join(diagnostics)) from exc
            raise
        finally:
            if proc.poll() is None:
                if qmp is not None:
                    try:
                        qmp.execute("quit", timeout_s=1.0)
                    except (QmpError, BrokenPipeError):
                        pass
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    terminate_process(proc)
            stderr.seek(0)
            stderr_text = stderr.read()

    if last_state is None:
        raise RuntimeError(f"{program.name}: no state was captured")
    if proc.returncode != 0:
        raise RuntimeError(
            f"{program.name}: QEMU exited with {proc.returncode}\n{stderr_text}")
    return RunResult(last_state, last_registers, extras, stderr_text, polls,
                     time.monotonic() - started)


def run_expected_exit(qemu: str, program: MicroProgram) -> str:
    expected = program.expected_exit
    if expected is None:
        raise ValueError("run_expected_exit() requires ExpectedExit")

    proc = subprocess.Popen(
        _command(qemu, program),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.stdin is None or proc.stdout is None:
        kill_process(proc)
        raise RuntimeError("QEMU QMP pipes were not created")
    qmp = QmpClient(proc.stdout, proc.stdin)
    try:
        qmp.execute("cont")
    except (QmpError, BrokenPipeError):
        # An expected fatal CPU path can close QMP before the main loop sends
        # the cont reply.  The process exit status and required diagnostics
        # below remain the oracle; an unrelated disconnect either times out
        # or fails those checks.
        pass
    try:
        stdout, stderr = proc.communicate(timeout=program.completion.timeout_s)
    except subprocess.TimeoutExpired as exc:
        kill_process(proc)
        stdout, stderr = proc.communicate()
        raise RuntimeError(f"{program.name}: QEMU did not exit as expected\n"
                           f"{stdout}\n{stderr}") from exc
    combined = stdout + stderr
    if expected.returncode is not None and proc.returncode != expected.returncode:
        raise RuntimeError(
            f"{program.name}: expected exit {expected.returncode}, got "
            f"{proc.returncode}\n{combined}")
    if expected.returncode is None and proc.returncode == 0:
        raise RuntimeError(f"{program.name}: QEMU unexpectedly succeeded\n{combined}")
    missing = [token for token in expected.stderr_contains
               if token not in combined]
    if missing:
        raise RuntimeError(f"{program.name}: missing diagnostics {missing!r}\n"
                           f"{combined}")
    return combined
