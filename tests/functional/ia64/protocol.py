"""Parser and stream waiter for the structured IA64TEST serial protocol."""

# SPDX-License-Identifier: GPL-2.0-or-later

from dataclasses import dataclass, field
import re
import select
import time
from typing import Callable, Iterable


IDENTIFIER = r"[A-Za-z0-9_.-]+"
CASE_RE = re.compile(
    rf"^IA64TEST suite=(?P<suite>{IDENTIFIER}) "
    rf"case=(?P<case>{IDENTIFIER}) status=(?P<status>PASS|FAIL)"
    r"(?: code=(?P<code>[0-9a-fA-F]+) detail=(?P<detail>[^\r\n]+))?$"
)
DONE_RE = re.compile(
    rf"^IA64TEST suite=(?P<suite>{IDENTIFIER}) status=DONE "
    r"passed=(?P<passed>[0-9]+) failed=(?P<failed>[0-9]+)$"
)


class ProtocolError(AssertionError):
    pass


@dataclass(frozen=True)
class CaseResult:
    case_id: str
    passed: bool
    code: int | None = None
    detail: str | None = None


@dataclass
class SuiteResult:
    suite: str
    cases: dict[str, CaseResult] = field(default_factory=dict)
    passed: int | None = None
    failed: int | None = None
    raw_console: str = ""

    @property
    def done(self) -> bool:
        return self.passed is not None

    def assert_valid(self, required_cases: Iterable[str]) -> None:
        if not self.done:
            raise ProtocolError(f"suite {self.suite!r} did not emit DONE")
        missing = sorted(set(required_cases) - self.cases.keys())
        if missing:
            raise ProtocolError(
                f"suite {self.suite!r} omitted cases: {', '.join(missing)}")
        observed_passed = sum(case.passed for case in self.cases.values())
        observed_failed = len(self.cases) - observed_passed
        if (self.passed, self.failed) != (observed_passed, observed_failed):
            raise ProtocolError(
                f"suite {self.suite!r} DONE counts "
                f"{self.passed}/{self.failed} do not match cases "
                f"{observed_passed}/{observed_failed}")
        failures = [case for case in self.cases.values() if not case.passed]
        if self.failed != 0 or failures:
            detail = ", ".join(
                f"{case.case_id}(0x{case.code:x}:{case.detail})"
                for case in failures)
            raise ProtocolError(
                f"suite {self.suite!r} reported failures: {detail}")


class ProtocolParser:
    def __init__(self, suite: str):
        self.result = SuiteResult(suite=suite)
        self._line_buffer = ""

    def feed(self, data: bytes | str) -> list[CaseResult]:
        if isinstance(data, bytes):
            text = data.decode("utf-8", errors="replace")
        else:
            text = data
        self.result.raw_console += text
        self._line_buffer += text.replace("\r", "")
        new_cases = []
        while "\n" in self._line_buffer:
            line, self._line_buffer = self._line_buffer.split("\n", 1)
            case = self._parse_line(line.strip())
            if case is not None:
                new_cases.append(case)
        return new_cases

    def _parse_line(self, line: str) -> CaseResult | None:
        if not line.startswith("IA64TEST "):
            return None
        match = CASE_RE.fullmatch(line)
        if match:
            if match["suite"] != self.result.suite:
                raise ProtocolError(
                    f"expected suite {self.result.suite!r}, got "
                    f"{match['suite']!r}")
            if self.result.done:
                raise ProtocolError("case appeared after DONE")
            case_id = match["case"]
            if case_id in self.result.cases:
                raise ProtocolError(f"duplicate case ID {case_id!r}")
            passed = match["status"] == "PASS"
            if passed and (match["code"] is not None or
                           match["detail"] is not None):
                raise ProtocolError("PASS line contains failure fields")
            if not passed and (match["code"] is None or
                               match["detail"] is None):
                raise ProtocolError("FAIL line omits code/detail")
            case = CaseResult(
                case_id, passed,
                int(match["code"], 16) if match["code"] else None,
                match["detail"],
            )
            self.result.cases[case_id] = case
            return case

        match = DONE_RE.fullmatch(line)
        if match:
            if match["suite"] != self.result.suite:
                raise ProtocolError(
                    f"expected suite {self.result.suite!r}, got "
                    f"{match['suite']!r}")
            if self.result.done:
                raise ProtocolError("duplicate DONE line")
            self.result.passed = int(match["passed"])
            self.result.failed = int(match["failed"])
            return None
        raise ProtocolError(f"malformed IA64TEST line: {line!r}")


def wait_for_menu(console_socket, timeout: float = 60.0,
                  marker: str = "to change option") -> None:
    """Wait for the boot manager menu prompt on the serial console.

    The menu waits forever on the default Timeout=0xFFFF (as the EFI sample
    does), so a test must interact with it: select the highlighted default to
    boot (see select_default_boot), or navigate to another entry.
    """
    deadline = time.monotonic() + timeout
    buf = ""
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        readable, _, _ = select.select(
            [console_socket], [], [], min(0.5, remaining))
        if not readable:
            continue
        data = console_socket.recv(4096)
        if not data:
            raise ProtocolError("console closed before the boot menu appeared")
        buf += data.decode("utf-8", errors="replace")
        if marker in buf:
            return
    raise ProtocolError(
        f"timed out waiting for the boot menu\n{buf[-2000:]}")


def select_default_boot(console_socket, timeout: float = 60.0) -> None:
    """Boot 'Removable Media Boot' (the first menu entry) from the boot menu.

    Wait for the menu, move the highlight to the top -- the boot manager keeps
    its selection across activations, so it may be sitting on a different entry
    if the shell was opened and exited -- then press Enter.  Up-arrow clamps at
    the first entry, so three of them reach it from anywhere in the menu.
    """
    wait_for_menu(console_socket, timeout)
    console_socket.sendall(b"\x1b[A\x1b[A\x1b[A\r")   # Up x3 (VT100), Enter


def open_menu_entry(console_socket, send_navigation, banner: str,
                    timeout: float = 60.0, settle: float = 0.5,
                    resend_interval: float = 3.0) -> None:
    """Select a boot-manager entry and wait for its banner, tolerating a slow
    host.

    The menu waits forever on the default Timeout=0xFFFF and polls for input,
    so a heavily loaded CI runner can drop the first keystrokes when they land
    before the input loop starts reading; the menu then sits idle and a plain
    wait would block until the test harness kills the process.  Wait for the
    menu, settle briefly, then send the navigation and re-send it every
    ``resend_interval`` seconds until ``banner`` appears -- raising with the
    console tail on timeout rather than hanging.

    ``send_navigation`` is a callable that emits the keystrokes; it must
    re-anchor to the top entry before stepping down (the up-arrow clamps at the
    first entry), so a re-send lands on the same entry no matter which
    keystrokes a previous attempt consumed.
    """
    wait_for_menu(console_socket, timeout)
    deadline = time.monotonic() + timeout
    next_send = time.monotonic() + settle
    buf = ""
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            send_navigation()
            next_send = now + resend_interval
        window = min(next_send, deadline) - time.monotonic()
        readable, _, _ = select.select(
            [console_socket], [], [], max(0.0, min(0.5, window)))
        if not readable:
            continue
        data = console_socket.recv(4096)
        if not data:
            raise ProtocolError(
                "console closed before the boot-manager entry opened")
        buf += data.decode("utf-8", errors="replace")
        if banner in buf:
            return
    raise ProtocolError(
        f"timed out opening a boot-manager entry (want {banner!r})\n"
        f"{buf[-2000:]}")


def wait_for_suite(console_socket, suite: str, required_cases: Iterable[str],
                   timeout: float,
                   on_case: Callable[[CaseResult], None] | None = None,
                   process_alive: Callable[[], bool] | None = None
                   ) -> SuiteResult:
    parser = ProtocolParser(suite)
    try:
        return _wait_for_suite(parser, console_socket, required_cases,
                               timeout, on_case, process_alive)
    except ProtocolError as error:
        # Keep what arrived so the caller can log it: a lost or garbled
        # line is only visible in the console stream itself.
        error.raw_console = parser.result.raw_console
        raise


def _wait_for_suite(parser: ProtocolParser, console_socket,
                    required_cases: Iterable[str], timeout: float,
                    on_case: Callable[[CaseResult], None] | None,
                    process_alive: Callable[[], bool] | None
                    ) -> SuiteResult:
    suite = parser.result.suite
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if parser.result.done:
            parser.result.assert_valid(required_cases)
            return parser.result
        if process_alive is not None and not process_alive():
            raise ProtocolError(
                f"QEMU exited before suite {suite!r} completed\n"
                f"{parser.result.raw_console[-4000:]}")
        remaining = max(0.0, deadline - time.monotonic())
        readable, _, _ = select.select(
            [console_socket], [], [], min(0.1, remaining))
        if not readable:
            continue
        data = console_socket.recv(4096)
        if not data:
            raise ProtocolError(
                f"console closed before suite {suite!r} completed")
        for case in parser.feed(data):
            if on_case is not None:
                on_case(case)
    raise ProtocolError(
        f"timed out waiting for suite {suite!r}\n"
        f"{parser.result.raw_console[-4000:]}")
