#ifndef PROJECT_EXPERIMENT_H
#define PROJECT_EXPERIMENT_H
/* Default: one recoverable internal Communication fault, 13 observations.
 * Optional validation builds: 1 = persistent optional path -> L3;
 * 2 = persistent essential path -> L4. The default does not run these. */
#ifndef MRG_DEMO_CASE
#define MRG_DEMO_CASE 0
#endif
#if MRG_DEMO_CASE < 0 || MRG_DEMO_CASE > 2
#error Invalid MRG_DEMO_CASE
#endif
#ifndef MRG_EXPERIMENT_CYCLES
#if MRG_DEMO_CASE == 0
#define MRG_EXPERIMENT_CYCLES 13U
#else
#define MRG_EXPERIMENT_CYCLES 18U
#endif
#endif
#endif
