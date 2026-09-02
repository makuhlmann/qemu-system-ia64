#!/usr/bin/env python3
#
# IA-64 encoder compatibility facade and microprogram construction helpers.
# Instruction encoders live in encoding_<family>.py beside the matching
# target/ia64/translate/gen-<family>.c implementation boundary.

import re

from .runner import (Completion, ExpectedExit, MicroProgram, encode_bundles,
                     run_expected_exit, run_microprogram)
from .fpmodel import (BINARY32_EDGE_VECTORS, BINARY64_EDGE_VECTORS,
                      binary32_to_spill, binary64_to_spill,
                      deterministic_words, fnorm_setf_sig,
                      spill_to_binary32, spill_to_binary64)
from .state import ExpectedBits, ExpectedFP, StateExpectation
from .case import CaseMetadata, IA64Case
from .encoding_constants import *
from .encoding_common import *
from .encoding_branch import *
from .encoding_integer import *
from .encoding_system import *
from .encoding_memory import *
from .encoding_fp import *
from .encoding_simd import *

IA32_TEST_CSD = 0x09b0ffff00000000
IA32_TEST_DSD = 0x0930ffff00000000
IA32_TEST_GDTD = 0x0800ffff00000000


def ia32_environment_bundles(address, target, reg=31,
                              csd=IA32_TEST_CSD,
                              dsd=IA32_TEST_DSD,
                              ssd=None):
    """Install valid flat segment descriptors before an IA-32 transition."""
    if ssd is None:
        ssd = dsd
    return [
        (address, *movl_mlx(reg, dsd)),
        (address + 0x10, 0x00, nop_m(),
         adds(24, 0, reg), adds(27, 0, reg)),
        (address + 0x20, 0x00, nop_m(),
         adds(28, 0, reg), adds(29, 0, reg)),
        (address + 0x30, *movl_mlx(reg, ssd)),
        (address + 0x40, 0x00, mov_m_gr_ar(reg, 26), nop_i(), nop_i()),
        (address + 0x50, *movl_mlx(reg, csd)),
        # CSD is consumed before the I-slot clears stype/s in the scratch GR,
        # leaving a valid system descriptor in the architected GDTD (GR31).
        (address + 0x60, 0x10, mov_m_gr_ar(reg, 25),
         dep(reg, 0, reg, 52, 5),
         br_cond(address + 0x60, target)),
    ]

def run_program(qemu, bundles, entry=0x10, alat="full",
                terminal_ip=None, expected=None, timeout=2.0,
                name="ia64-microprogram", poll_initial_s=0.001,
                poll_max_s=0.020, cpu=None, smp="1", memory=None):
    """Run until an explicit architectural terminal state."""
    expected = dict(expected or {})
    if terminal_ip is None:
        terminal_ip = expected.get("ip")
    elif "ip" not in expected:
        expected["ip"] = terminal_ip
    expectation = StateExpectation(expected)
    predicate = None
    if terminal_ip is None and expected:
        predicate = expectation.matches
    program = MicroProgram(
        name=name,
        bundles=encode_bundles(normalized_bundles(bundles), bundle_words),
        entry=entry,
        expected=expectation,
        completion=Completion(terminal_ip=terminal_ip, timeout_s=timeout,
                              poll_initial_s=poll_initial_s,
                              poll_max_s=poll_max_s,
                              predicate=predicate),
        machine_args=(() if alat is None else (f"alat={alat}",)),
        cpu=cpu,
        smp=smp,
        memory=memory,
    )
    return run_microprogram(qemu, program)


def run_program_expect_failure(qemu, bundles, entry=0x10, timeout=3,
                               cpu=None):
    program = MicroProgram(
        name="ia64-expected-qemu-failure",
        bundles=encode_bundles(normalized_bundles(bundles), bundle_words),
        entry=entry,
        expected=StateExpectation(),
        completion=Completion(terminal_ip=None, timeout_s=timeout),
        cpu=cpu,
        expected_exit=ExpectedExit(),
    )
    return run_expected_exit(qemu, program)


def require_qemu_failure(name, bundles, expected_substrings, entry=0x10,
                         cpu=None):
    def tc(qemu):
        output = run_program_expect_failure(qemu, bundles, entry=entry,
                                            cpu=cpu)
        missing = [s for s in expected_substrings if s not in output]
        if missing:
            raise RuntimeError(
                f"{name} failed: missing {missing!r}\n{output}")
    features = {"negative-qemu-startup"}
    if cpu is not None:
        features.add(f"cpu-model:{cpu}")
    return IA64Case(
        name=name, runner=tc,
        bundles=tuple(tuple(bundle) for bundle in bundles),
        metadata=CaseMetadata(required_features=frozenset(features)),
    )


def parse_jit_stats(output):
    stats = {}
    for key in [
        "TB count",
        "TB flush count",
        "TB invalidate count",
        "TLB full flushes",
        "TLB partial flushes",
        "TLB elided flushes",
    ]:
        match = re.search(rf"^{re.escape(key)}\s+([0-9]+)",
                          output, re.MULTILINE)
        if match:
            stats[key] = int(match.group(1), 10)
    return stats


def run_program_jit(qemu, bundles, entry=0x10, terminal_ip=None, memory=None):
    bundles = normalized_bundles(bundles)
    if terminal_ip is None:
        terminal_ip = bundles[-1][0]
    program = MicroProgram(
        name="ia64-jit-observation",
        bundles=encode_bundles(bundles, bundle_words),
        entry=entry,
        expected=StateExpectation({"ip": terminal_ip}),
        completion=Completion(terminal_ip=terminal_ip, timeout_s=3.0),
        machine_args=("alat=full",),
        memory=memory,
    )
    result = run_microprogram(qemu, program, extra_hmp=("info jit",))
    output = result.extra_hmp["info jit"]
    return parse_jit_stats(output), output


def require_registers(name, bundles, expected, entry=0x10, alat="full",
                      cpu=None, smp="1"):
    def tc(qemu):
        run_program(qemu, bundles, entry=entry, alat=alat,
                    expected=expected, name=name, cpu=cpu, smp=smp)
    features = set()
    if alat is not None:
        features.add(f"alat:{alat}")
    if cpu is not None:
        features.add(f"cpu-model:{cpu}")
    if smp != "1":
        features.add("smp")
    return IA64Case(
        name=name, runner=tc,
        bundles=tuple(tuple(bundle) for bundle in bundles),
        expected=dict(expected),
        metadata=CaseMetadata(required_features=frozenset(features)),
    )


IA64_EXCEPTION_VECTORS = {
    IA64_EXCP_ILLEGAL: IA64_GENERAL_VECTOR,
    IA64_EXCP_RESERVED_TEMPLATE: IA64_GENERAL_VECTOR,
    IA64_EXCP_NAT_CONSUMPTION: IA64_NAT_CONSUMPTION_VECTOR,
    IA64_EXCP_UNALIGNED: IA64_UNALIGNED_VECTOR,
    IA64_EXCP_UNIMPL_DATA_ADDR: IA64_GENERAL_VECTOR,
    IA64_EXCP_UNIMPL_INST_ADDR: IA64_LOWER_PRIV_TRANSFER_VECTOR,
    IA64_EXCP_PRIVILEGED_OP: IA64_GENERAL_VECTOR,
    IA64_EXCP_PRIVILEGED_REG: IA64_GENERAL_VECTOR,
    IA64_EXCP_RESERVED_REG_FIELD: IA64_GENERAL_VECTOR,
    IA64_EXCP_DISABLED_ISA_TRANSITION: IA64_GENERAL_VECTOR,
    IA64_EXCP_DISABLED_FP: IA64_DISABLED_FP_VECTOR,
    IA64_EXCP_UNSUPPORTED_DATA_REFERENCE:
        IA64_UNSUPPORTED_DATA_REFERENCE_VECTOR,
    IA64_EXCP_VIRTUALIZATION: IA64_VIRTUALIZATION_VECTOR,
    IA64_EXCP_TAKEN_BRANCH: IA64_TAKEN_BRANCH_VECTOR,
    IA64_EXCP_SINGLE_STEP: IA64_SINGLE_STEP_VECTOR,
}


def require_exception(name, bundles, excp, fault_ip=None, fault_imm=None,
                      entry=0x10, cpu=None):
    def tc(qemu):
        vector = IA64_EXCEPTION_VECTORS.get(excp)
        if vector is None:
            expected = {"exception": excp, "fault_code": excp}
            if fault_ip is not None:
                expected["fault_ip"] = fault_ip
            if fault_imm is not None:
                expected["fault_imm"] = fault_imm
            # The faulting IP can also be the initial IP.  Complete on the
            # exception state so an early QMP stop cannot observe reset state.
            run_program(
                qemu, bundles, entry=entry, expected=expected, name=name,
                cpu=cpu)
            return

        setup = 0x100000
        occupied = {address for address, *_ in bundles}
        while setup in occupied or (setup + 0x10) in occupied:
            setup += 0x1000
        if vector in occupied or (vector + 0x10) in occupied:
            raise RuntimeError(
                f"{name} test bundle overlaps exception vector 0x{vector:x}")

        wrapped = list(bundles) + [
            (setup, *movl_mlx(2, 1 << 13)),
            (setup + 0x10, 0x10, mov_gr_psr_full(2), nop_i(),
             br_cond(setup + 0x10, entry)),
            (vector, 0x00, nop_m(), nop_i(), nop_i()),
            (vector + 0x10, 0x10, nop_m(), nop_i(),
             br_cond(vector + 0x10, vector + 0x10)),
        ]
        expected = {
            "ip": vector + 0x10,
            "exception": IA64_EXCP_NONE,
            "fault_code": excp,
        }
        if fault_ip is not None:
            expected["fault_ip"] = fault_ip
        if fault_imm is not None:
            expected["fault_imm"] = fault_imm
        run_program(qemu, wrapped, entry=setup, expected=expected, name=name,
                    cpu=cpu)
    case_expected = {"exception": excp}
    if fault_ip is not None:
        case_expected["fault_ip"] = fault_ip
    if fault_imm is not None:
        case_expected["fault_imm"] = fault_imm
    features = {"alat:full"}
    if cpu is not None:
        features.add(f"cpu-model:{cpu}")
    return IA64Case(
        name=name, runner=tc,
        bundles=tuple(tuple(bundle) for bundle in bundles),
        expected=case_expected,
        metadata=CaseMetadata(required_features=frozenset(features)),
    )


def require_uncollected_reserved_field(name, bundles, fault_ip, fault_imm,
                                       entry=0x10):
    return require_registers(name, bundles, {
        "ip": fault_ip,
        "fault_ip": fault_ip,
        "fault_imm": fault_imm,
        "exception": IA64_EXCP_RESERVED_REG_FIELD,
        "fault_code": IA64_EXCP_RESERVED_REG_FIELD,
    }, entry=entry)



# Shared construction data used by more than one case family.

HIGH_TR_BASE = 0xa000000100000000
LOW_VECTOR_TR_PTE = 0x0010000004000661
LOW_VECTOR_ITIR = 0x38
KERNEL_TR_ITIR = 26 << 2
EIGHT_K_ITIR = 13 << 2
PTE_ED = 1 << 52
DTR_PTE_WB = 0x0010000000000661
DTR_PTE_UC = 0x0010000000000671
DTR_PTE_NATPAGE = DTR_PTE_WB | (7 << 2)
KEY_TEST_KEY = 0x456


def dtr_setup_bundles(start, va, pa, page_shift=16, slot=5,
                      pte_flags=DTR_PTE_WB):
    page_mask = (1 << page_shift) - 1

    if (va & page_mask) != (pa & page_mask):
        raise ValueError("DTR virtual and physical page offsets differ")

    return [
        (start, *movl_mlx(18, (pa & ~page_mask) | pte_flags)),
        (start + 0x10, *movl_mlx(19, va & ~page_mask)),
        (start + 0x20, 0x00, mov_m_gr_cr(19, 20),
         adds(21, page_shift << 2, 0), nop_i()),
        (start + 0x30, 0x00, mov_m_gr_cr(21, 21),
         adds(10, slot, 0), nop_i()),
        (start + 0x40, 0x00, itr_d(10, 18), nop_i(), nop_i()),
        (start + 0x50, 0x00, srlz_d(), nop_i(), nop_i()),
    ]

CHECK_LOAD_DATA = bundle_words(0x00, 0x123456789abcdef0, 0, 0)[0]
ADV_UC_LOAD_VA = HIGH_TR_BASE + 0x9000
ADV_UC_LOAD_PA = 0x409000
ADV_UC_LOAD_DATA = bundle_words(0x00, 0x1122334455667788, 0, 0)[0]
ADV_UC_LOAD_BUNDLE = (ADV_UC_LOAD_PA, 0x00, 0x1122334455667788, 0, 0)

def register_nat_consumption_test(name, fault_bundle, expected_isr=0,
                                  enable_ic=True, initial_ifa=None):
    bundles = [
        (0x10, 0x00, mov_m_imm_ar(36, 1), addl(6, 0x200, 0),
         nop_i()),
        (0x20, 0x08, ld8_fill_postinc(16, 6, 0), nop_i(),
         nop_i()),
    ]
    fault_ip = 0x30
    if initial_ifa is not None:
        bundles.extend([
            (fault_ip, *movl_mlx(19, initial_ifa)),
            (fault_ip + 0x10, 0x00, mov_m_gr_cr(19, 20), nop_i(), nop_i()),
        ])
        fault_ip += 0x20
    if enable_ic:
        bundles.extend([
            (fault_ip, 0x00, ssm(1 << 13), nop_i(),
             nop_i()),
            (fault_ip + 0x10, 0x00, srlz_d(), nop_i(),
             nop_i()),
        ])
        fault_ip += 0x20
    bundles.extend([
        (fault_ip, *fault_bundle),
        (0x5600, 0x00, mov_m_cr_gr(14, 20), nop_i(),
         nop_i()),
        (0x5610, 0x00, mov_m_cr_gr(15, 17), nop_i(),
         nop_i()),
        (0x5620, 0x10, nop_m(), nop_i(),
         br_cond(0x5620, 0x5620)),
        (0x200, 0x00, 0, 0,
         0),
    ])
    # SDM Vol. 2 Table 8-1: CR.IFA is written only when PSR.ic is set.
    expected_ifa = (
        initial_ifa if initial_ifa is not None and not enable_ic else 0
    )
    return require_registers(name, bundles, {
        "ip": 0x5620,
        "exception": IA64_EXCP_NONE,
        "r14": expected_ifa,
        "r15": IA64_ISR_CODE_REG_NAT | expected_isr,
    }, entry=0x10)

_strcpy_pipeline_data = [0x6d6e6f7071727374] * 126 + [0, 0]
