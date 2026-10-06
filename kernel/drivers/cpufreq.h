/* cpufreq.h — EIST P-state scaling + CPU busy% sampling (UAOS-272)
 *
 * The MBP4,1's Core 2 Duo sits at max FID/VID whenever it is awake —
 * the -128 Idle task already hlt-loops (~100% idle share measured via
 * taskstat), so the idle heat problem is voltage/frequency, not a
 * spinning CPU.  This driver implements a small ondemand-style governor
 * that writes MSR_IA32_PERF_CTL (FID/VID pairs) when the system is busy
 * and drops to the lowest P-state after sustained idle.
 */

#ifndef UAOS_CPUFREQ_H
#define UAOS_CPUFREQ_H

#include <stdint.h>

#define CPUFREQ_MAX_STATES 8

typedef struct {
    int      est;            /* CPUID EST capability                 */
    int      eist_on;        /* MISC_ENABLE bit16 set (by us or fw)  */
    int      locked;         /* MISC_ENABLE bit20 — EIST locked off  */
    int      c1e;            /* unused — MSR_POWER_CTL #GPs on Core 2  */
    int      tm1;            /* TM1 duty modulation fallback in use  */
    int      inv_tsc;        /* invariant TSC (P-state-safe timing)  */
    int      scale_ok;       /* governor may write PERF_CTL          */
    int      table_source;   /* 0=none 1=_PSS 2=synth 3=heuristic   */
    int      n_states;
    uint32_t state_mhz[CPUFREQ_MAX_STATES];
    uint32_t state_ctl[CPUFREQ_MAX_STATES];
    uint32_t state_sts[CPUFREQ_MAX_STATES];
    int      cur_idx;        /* current target P-state, -1 = unknown */
    uint32_t busy_pct;       /* last-window CPU busy% (0-100)        */
    uint32_t transitions;    /* PERF_CTL writes performed            */
    uint32_t perf_status;    /* PERF_STATUS[15:0] at last read       */
    int      cpu_family;
    int      cpu_model;
    uint32_t pss_hits;       /* _PSS decls seen but unparsed           */
    uint32_t tables_scanned; /* DSDT/SSDT blobs searched for _PSS      */
    uint32_t boot_ctl;       /* PERF_STATUS[15:0] at init              */
    uint32_t dbg_n;
    uint32_t dbg_addr[4];    /* phys addrs of first _PSS hits          */
    uint32_t dbg_tbln;
    uint32_t dbg_tbl[8];     /* phys base of each scanned table        */
    uint32_t dbg_tlen[8];
} CpuFreqInfo;

/* Probe CPU + firmware for frequency-scaling support and arm the
 * governor.  Safe no-op on VMs and non-Core-2 CPUs.  Call after
 * IDT_Init (a #GP on an unexpected MSR would otherwise triple-fault)
 * and before the scheduler starts — anywhere in that window works. */
void     CpuFreq_Init(uint32_t mb2_phys);

/* Per-PIT-tick hook: samples busy% each CPUFREQ window and runs the
 * governor.  Call from PIT_IRQHandler (IRQ context, 100 Hz). */
void     CpuFreq_Tick(void);

/* CPU busy percentage (0-100) over the last sampling window.  Always
 * maintained — even when scaling is unsupported — for the menubar. */
uint32_t CpuFreq_BusyPercent(void);

/* Snapshot of driver state for C:cpu. */
void     CpuFreq_GetInfo(CpuFreqInfo *out);

#endif /* UAOS_CPUFREQ_H */
