#ifndef TSD_RUNTIME_GUARD_INTERNAL_H
#define TSD_RUNTIME_GUARD_INTERNAL_H

#include <thermal/simd/simd_width.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Process-wide wide-SIMD admission protocol.
 *
 * Safety-state writers never wait for application kernels. They first close
 * admission atomically, then publish the new guard state under a short writer
 * mutex. A wide execution increments the in-flight counter and revalidates the
 * admission bit before entering user code. Therefore once a revocation closes
 * admission no new AVX2/AVX-512 invocation can begin, while already-admitted
 * non-preemptible calls are allowed to drain naturally.
 *
 * SSE4.1 does not participate in the in-flight counter and remains available
 * during guard updates and shutdown.
 */
int tsd_runtime_execution_enter(simd_width_t width);
void tsd_runtime_execution_leave(simd_width_t width);
int tsd_runtime_wide_admission_is_open(void);
void tsd_runtime_wide_admission_close(void);
int tsd_runtime_wait_for_wide_quiescence(void);

/*
 * Serialize guard-state/selector publication. Enter closes wide admission but
 * deliberately does not wait for in-flight kernels. Leave reopens admission
 * only when the newly published guard state authorizes wider SIMD.
 */
int tsd_runtime_safety_write_enter(void);
void tsd_runtime_safety_write_leave(void);

/* Runtime lifecycle state participates in wide-width authorization. */
void tsd_runtime_set_stopping_locked(int stopping);
int tsd_runtime_is_stopping(void);

/*
 * Hardware perf authority is bound to the thread that started the runtime.
 * While a live guard is active, only that owner thread may enter a wide
 * registered-kernel invocation or contribute work-normalized accounting.
 */
void tsd_runtime_set_owner_tid_locked(int tid);
int tsd_runtime_current_thread_is_owner(void);
int tsd_runtime_work_accounting_allowed(void);

#ifdef __cplusplus
}
#endif

#endif /* TSD_RUNTIME_GUARD_INTERNAL_H */
