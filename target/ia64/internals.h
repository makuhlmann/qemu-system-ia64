/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal IA-64 CPU subsystem state.  Include from cpu.h only after the
 * architectural entry types have been declared.
 */

#ifndef TARGET_IA64_INTERNALS_H
#define TARGET_IA64_INTERNALS_H

typedef struct IA64ExceptionState {
    /* Restart-visible exception and interruption-delivery state. */
    uint64_t fault_ip;
    /*
     * Faulting data address kept apart from architected CR.IFA, which is
     * only written for a collected interruption (PSR.ic = 1, SDM Vol. 2
     * Table 8-1).
     */
    uint64_t fault_addr;
    uint64_t fault_imm;
    uint64_t fault_tmpl;
    uint32_t exception;
    uint32_t fault_exception;
    uint32_t fault_slot;
    bool ia32_trap;
    /* Trap raised by the IA-32 side of an ISA transition boundary. */
    bool ia32_transition_trap;

    /* Transient state spanning one serialization/fault-suppression window. */
    bool psr_ic_inflight;
    uint64_t psr_suppression_before_insn;
    uint64_t suppressed_tlb_pages[IA64_SUPPRESSED_TLB_MAX];
    uint16_t suppressed_tlb_idxmaps[IA64_SUPPRESSED_TLB_MAX];
    uint8_t suppressed_tlb_count;
    bool suppressed_tlb_overflow;
} IA64ExceptionState;

typedef struct IA64MMUState {
    /* Derived translation caches; all entries are reconstructible. */
    IA64TlbEntry tlb_data[IA64_TLB_MAX];
    IA64TlbEntry tlb_inst[IA64_TLB_MAX];
    uint16_t tlb_data_count;
    uint16_t tlb_inst_count;
    uint16_t tlb_data_replace;
    uint16_t tlb_inst_replace;
    uint32_t tlb_data_generation;
    uint32_t tlb_inst_generation;
    IA64MicroTlbEntry tlb_data_micro[IA64_MICRO_TLB_SIZE];
    IA64MicroTlbEntry tlb_inst_micro[IA64_MICRO_TLB_SIZE];
    IA64CodeTlbEdCache code_tlb_ed;

    /* Transient bookkeeping for architected purge operations. */
    uint16_t pending_purge_data_count;
    uint16_t pending_purge_inst_count;

    /*
     * Upper bound (region-7 offset) of the persistent KSEG physical alias:
     * IA64_FW_REGION7_DIRECTMAP_BASE + guest RAM size.  Set at reset from the
     * machine RAM size; see ia64_sal_boot_identity_pa_type().
     */
    uint64_t region7_directmap_limit;
} IA64MMUState;

typedef struct IA64InterruptState {
    /* Architected Local SAPIC and pending external interrupt state. */
    uint8_t pending_extint;
    bool pal_halt_wake;
    uint64_t sapic_irr[4];
    uint64_t sapic_isr[4];

    /* Derived host timer state for architected ITC/ITM registers. */
    int64_t itc_delta;
    uint64_t itm_armed_value;
    uint64_t itm_last_match;
    bool itm_armed;
    bool itm_last_match_valid;
} IA64InterruptState;

typedef struct IA64PalState {
    /* Architected PAL registration and machine-check state. */
    bool pal_mc_expected;
    uint64_t pal_mc_save_addr;
    uint64_t pal_pmi_entry;
    bool pal_proc_copy_valid;
    uint64_t pal_proc_copy_addr;
    uint64_t pal_interrupt_block_addr;
    uint64_t pal_io_block_addr;
} IA64PalState;

typedef struct IA64RSEState {
    /* Architected RSE physical register file and partition state. */
    uint64_t rse_pgr[IA64_STACKED_GR_COUNT];
    uint64_t rse_pgr_nat[2];
    uint64_t rse_gr_dirty[2];
    uint32_t rse_bol;
    int32_t rse_dirty;
    int32_t rse_dirty_nat;
    int32_t rse_clean;
    int32_t rse_clean_nat;
    int32_t rse_invalid;
    bool rse_cfle;
    /*
     * Lowest backing-store address whose NaT collection bit is represented
     * in AR.RNAT.  Consecutive stores extend it downward; a collection-word
     * load or a software AR.RNAT write makes the whole current group
     * authoritative; a software BSPSTORE write resets it (RNAT is undefined
     * then until software rewrites it).  Boundary stores merge the word in
     * memory below this floor, and mandatory loads below it take their NaT
     * bit from the collection word in memory (see ia64_rse_store_one and
     * ia64_rse_load_one).
     */
    uint64_t rse_rnat_low;
} IA64RSEState;

typedef struct IA64AlatState {
    /* Architected ALAT contents; active_count is a derived fast-path cache. */
    IA64AlatEntry alat[IA64_ALAT_ENTRIES];
    uint32_t alat_active_count;
    bool alat_full;
} IA64AlatState;

typedef struct IA64FPTransactionState {
    /* Transient rollback snapshot for one faultable FP instruction. */
    uint64_t backup_fr[IA64_FR_COUNT];
    uint64_t backup_fr_nat[2];
    uint64_t backup_fr_sig[2];
    uint64_t backup_fr_ext_mant[IA64_FR_COUNT];
    uint32_t backup_fr_ext_exp[IA64_FR_COUNT];
    uint64_t backup_fr_ext_sign[2];
    uint64_t backup_fr_ext_valid[2];
    uint64_t backup_fr_int_value[IA64_FR_COUNT];
    uint64_t backup_fr_int_origin[2];
    uint64_t backup_pr[IA64_PR_COUNT];
    uint64_t backup_fr_mask[2];
    uint64_t backup_pr_mask;
    uint64_t backup_psr_mf;
    bool active;
} IA64FPTransactionState;

typedef struct IA64FPState {
    /*
     * Authoritative IA-64 register-format representation.  fr is the
     * significand/binary64 execution cache; fr_ext_* and fr_int_* retain
     * values that cannot be represented losslessly by that cache.
     */
    uint64_t fr[IA64_FR_COUNT];
    uint64_t fr_nat[2];
    uint64_t fr_sig[2];
    uint64_t fr_ext_mant[IA64_FR_COUNT];
    uint32_t fr_ext_exp[IA64_FR_COUNT];
    uint64_t fr_ext_sign[2];
    uint64_t fr_ext_valid[2];
    uint64_t fr_int_value[IA64_FR_COUNT];
    uint64_t fr_int_origin[2];
    bool rotating_fr_live;

    IA64FPTransactionState transaction;

    /* Architected software-assist handoff retained until FPSWA consumes it. */
    uint64_t fpswa_result_low;
    uint64_t fpswa_result_high;
    uint64_t fpswa_flags;
    uint8_t fpswa_dest_fr;
    uint8_t fpswa_dest_pr;
    uint8_t fpswa_sf;
    bool fpswa_pending;
    bool fpswa_fpa;

    /* Derived SoftFloat execution status. */
    float_status fp_status;
} IA64FPState;

/*
 * Caller context saved across a virtual-mode SAL_PROC call that the machine
 * re-enters physically (see ia64_sal_runtime_enter).  SAL calls are not
 * re-entrant - the OS serializes them (SAL spec 3.1) - so one slot suffices.
 */
typedef struct IA64SalBridgeState {
    IA64FirmwareDebugRseState rse;
    uint64_t psr;
    uint64_t b0;
    uint64_t sp;
    uint64_t gp;
    uint64_t rsc;
    bool active;
} IA64SalBridgeState;

typedef struct IA64FirmwareDebugState {
    /* Device/firmware bridge state, deliberately outside CPUArchState. */
    uint8_t context[IA64_FW_DEBUG_CONTEXT_SIZE];
    IA64FirmwareDebugRseState rse;
    uint16_t vector;
    bool context_valid;
    bool handler_active;
    bool rse_valid;
} IA64FirmwareDebugState;

#endif /* TARGET_IA64_INTERNALS_H */
