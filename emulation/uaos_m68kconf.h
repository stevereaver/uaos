/* uaos_m68kconf.h — UAOS-specific Musashi configuration
 *
 * Include this as MUSASHI_CNF to override the defaults in m68kconf.h.
 * We target M68000 only (Amiga programs are 68000/68020 at most).
 * We enable the ILLEGAL and TRAP callbacks so UAOS can intercept them
 * for library dispatch instead of patching ROM vectors.
 */

#ifndef UAOS_M68KCONF_H
#define UAOS_M68KCONF_H

#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2

/* Only build 68000 — keeps code size manageable for a freestanding build */
#define M68K_EMULATE_010            M68K_OPT_OFF
#define M68K_EMULATE_EC020          M68K_OPT_OFF
#define M68K_EMULATE_020            M68K_OPT_OFF
#define M68K_EMULATE_030            M68K_OPT_OFF
#define M68K_EMULATE_040            M68K_OPT_OFF

/* Disable FPU — no softfloat library in freestanding build */
#define M68K_EMULATE_FPOINT         M68K_OPT_OFF

/* Single flat address space — no separate immediate/PC-relative reads */
#define M68K_SEPARATE_READS         M68K_OPT_OFF
#define M68K_SIMULATE_PD_WRITES     M68K_OPT_OFF

/* Autovector all interrupts — UAOS does not use 68k interrupt levels */
#define M68K_EMULATE_INT_ACK        M68K_OPT_OFF

/* No breakpoint support needed */
#define M68K_EMULATE_BKPT_ACK       M68K_OPT_OFF

/* No trace exceptions */
#define M68K_EMULATE_TRACE          M68K_OPT_OFF

/* RESET instruction — treat as no-op */
#define M68K_EMULATE_RESET          M68K_OPT_OFF

/* No CMPI.L callback */
#define M68K_CMPILD_HAS_CALLBACK    M68K_OPT_OFF

/* No RTE callback */
#define M68K_RTE_HAS_CALLBACK       M68K_OPT_OFF

/* No TAS callback */
#define M68K_TAS_HAS_CALLBACK       M68K_OPT_OFF

/* ILLEGAL instruction callback — forward to UAOS library dispatcher */
#define M68K_ILLG_HAS_CALLBACK      M68K_OPT_ON

/* TRAP instruction callback — forward to UAOS DOS/Exec trap handler */
#define M68K_TRAP_HAS_CALLBACK      M68K_OPT_ON

/* No function-code supervision */
#define M68K_EMULATE_FC             M68K_OPT_OFF

/* No PC-change monitor */
#define M68K_MONITOR_PC             M68K_OPT_OFF

/* Instruction hook — enabled for debugging */
#define M68K_INSTRUCTION_HOOK       M68K_OPT_ON

/* No prefetch queue */
#define M68K_EMULATE_PREFETCH       M68K_OPT_OFF

/* No address-error exceptions (simpler) */
#define M68K_EMULATE_ADDRESS_ERROR  M68K_OPT_OFF

/* No logging */
#define M68K_LOG_ENABLE             M68K_OPT_OFF
#define M68K_LOG_1010_1111          M68K_OPT_OFF
#define M68K_LOG_TRAP               M68K_OPT_OFF

/* No PMMU */
#define M68K_EMULATE_PMMU           M68K_OPT_OFF

/* Use 64-bit integers for speed */
#define M68K_USE_64_BIT             M68K_OPT_ON

#endif /* UAOS_M68KCONF_H */
