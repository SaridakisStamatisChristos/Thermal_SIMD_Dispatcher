#ifndef TSD_RUNTIME_GUARD_INTERNAL_H
#define TSD_RUNTIME_GUARD_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Process-wide safety gate.
 *
 * Execution paths hold a read lock for the complete non-preemptible kernel
 * invocation. Guard-state writers and SIMD selection changes hold the write
 * lock. This makes the effective authorization/selection boundary linearizable:
 * once a revoking guard update has committed, no new wide invocation can enter.
 */
int tsd_runtime_execution_enter(void);
void tsd_runtime_execution_leave(void);
int tsd_runtime_safety_write_enter(void);
void tsd_runtime_safety_write_leave(void);

/* Runtime lifecycle state participates in wide-width authorization. */
void tsd_runtime_set_stopping_locked(int stopping);
int tsd_runtime_is_stopping(void);

#ifdef __cplusplus
}
#endif

#endif /* TSD_RUNTIME_GUARD_INTERNAL_H */
