
Level 1: existing health-aware scheduling adaptation.
Level 2: cooperative reinitialization of the affected task's application context, followed by measured verification.
Level 3: limited service for recoverable noncritical functionality, with its limited status explicitly retained.
Level 4: controlled MCU reset after a simulated safe-state confirmation, printed notice and delay, when essential service cannot be recovered.
Installation in STM32CubeIDE
Back up the current working v4 project. Copy all 18 integration files to the matching paths. Interfaces changed; do not mix v4 headers with v5 sources.
Files	Purpose
`Core/Inc/hierarchical_recovery.h`, `Core/Src/hierarchical_recovery.c` — NEW	Recovery state machine, task-owned command/acknowledgement protocol, fresh-generation verification, escalation, action counters and table.
`Core/Inc/task_service.h`, `Core/Src/task_service.c` — NEW	Explicit simulated application context, session reinitialization, limited service, persistent/external fault models and simulated charging-output gate.
`Core/Inc/project_experiment.h` — NEW	Default short demonstration and optional separate validation profiles.
`Core/Inc/adaptive_scheduler.h`, `Core/Src/adaptive_scheduler.c`	Preserve the v4 scheduling policy; add recovery ownership so priority/flags remain held until recovery verification succeeds.
`Core/Inc/runtime_monitor.h`, `Core/Src/runtime_monitor.c`	Preserve scoring and tables; add task generations, mixed-window detection and actual injected-job exposure tags. Lifetime counters are retained.
`Core/Inc/fault_injection.h`, `Core/Src/fault_injection.c`	Inject once, without a timed OFF; identify fault exposure from measured jobs.
`Core/Inc/experiment_report.h`, `Core/Src/experiment_report.c`	Keep existing tables/timeline and report real recovery actions, verification times and unchanged task handles.
`Core/Inc/health_engine.h`, `Core/Src/health_engine.c`	Preserve the independent Health Engine table and current scoring interface.
`Core/Inc/FreeRTOSConfig.h`	Preserve the existing task-switch trace hooks and RTOS configuration.
`Core/Src/main.c`	Register actions; invoke recovery after scheduling; process Communication/Logging commands at job boundaries; add safe-state/reset callbacks and recovery table. Monitoring continues after the report finishes.
`EV_Charging_SelfHealing.ioc`	Retain matching base priorities and the existing 512-word monitor stack. No new RTOS task is needed.
Add the two new `.c` files to the project build. Ensure the three new headers are in `Core/Inc`.
Compile exactly one copy of each module. Exclude old scheduler variants and backup source copies from all build configurations, or keep them outside compiled folders.
Do not copy `tests` into firmware source/include paths. Never define `MRG_HOST_TEST` in CubeIDE. Those files contain host-only HAL/RTOS stubs and simulated output.
Use the default `MRG_DEMO_CASE=0`. No CubeMX regeneration is needed before the first run; matching `.ioc` and `main.c` files are supplied.
Clean, then rebuild the complete project including the FreeRTOS kernel. Missing task-switch hooks produce `[CONFIG ERROR]`; adaptation/recovery are held rather than using invalid CPU measurements.
Flash, connect Tera Term at 115200, 8-N-1, and press the board's physical RESET button to start the demonstration. Capture startup through `EXPERIMENT COMPLETE`. A terminal width around 100 columns helps keep the new table readable.
The physical RESET step matters: an intentional software-reset boot suppresses automatic fault reinjection, so the last-resort reset cannot immediately trigger the same scripted fault again. A debugger may also use software reset. If the table says `software-reset boot=YES` and injection is suppressed, press physical RESET or power-cycle to deliberately repeat the demonstration. Reset-cause flags are read before being cleared at startup.
This is an integration update, not a full CubeIDE export. Keep your existing HAL, CMSIS, FreeRTOS distribution, startup code, linker script and project metadata. `changes_from_v4.patch` is for reviewing differences, not required for installation.
Expected default sequence — 13 observations
These events passed the host simulation. Actual event timing on the board must follow its valid measurements; it is not forced by the scheduler/recovery controller.
Cycle	Expected measurement/action	Communication priority
1	All five tasks Healthy; normal baseline.	41
2	Healthy captured window; latch the simulated Communication fault afterwards.	41
3	First bad observation.	41
4	Second bad observation.	41
5	Third bad observation; verified Level-1 demotion below Logging.	38
6	First further bad observation after adaptation.	38
7	Second further bad observation.	38
8	Third further bad observation; pending `0x04`; Level-2 command queued.	38
Between 8 and 9	Communication reaches its next job boundary, reinitializes its own context, advances generation and acknowledges.	38
9	First fresh Healthy generation-2 window; controller observes the acknowledgement.	38
10	Second fresh Healthy window; priority and pending request stay held.	38
11	Third fresh Healthy window; verify full recovery, restore base priority and clear flags.	41
12–13	Stable operation; final summary at 13.	41
The injected workload targets approximately 5 ms, but actual CPU execution is measured. Your previous v4 board run measured roughly 4.36 ms; v5 adds small instrumentation costs, so do not copy model numbers into the final report.
The default successful outcome is one L2 action, one verified full recovery, zero L3 actions and zero MCU resets. L3/L4 evidence rows show `NOT OBSERVED` because escalation was unnecessary. That is expected: the hierarchy stops once the least disruptive effective action succeeds.
After the final report, runtime monitoring, adaptive decisions, recovery updates and reset dispatch continue once per observation. Repeated full tables stop; later recovery events can still print. This keeps the firmware's recovery loop alive beyond the demonstration.
What the new module actually does
The Runtime Monitor remains the single owner of recovery decisions and scheduler changes. The application task executes its own bounded reinitialization/limited-service callback at a checkpoint before `RTM_TaskStartAt`, outside application locks. No callback deletes a task or resets shared diagnostic/health structures.
A monotonically advancing command token identifies each request. A task acknowledges only its current token. Repeated monitor calls on the same frozen window cannot count twice. Acknowledgement alone is insufficient for success: the controller then requires three consecutive fresh Healthy windows with completed-job progress, no window errors/misses and a normal sampled task state.
Every successful task action advances a task generation. Old frozen windows and windows containing jobs from both generations cannot verify recovery. This prevents an earlier Healthy sample from being credited to a later reinitialization. Health scores are still computed by the existing health engine from measured work; recovery does not write scores or erase cumulative errors, releases or deadline misses.
The adapted priority and request flags stay held while the controller verifies recovery. Full success additionally requires a successful base-priority API call/readback. A failed restoration proceeds to the fallback path instead of claiming success.
The default internal fault models corrupted session/work state. `APP_Reinitialize` resets session progress and clears that internal latch. An external persistent source deliberately survives reinitialization. This distinction allows testing whether recovery works for a recoverable state fault and whether escalation works when the cause is outside that state.
Selectivity, graceful degradation and reset policy
Automatic cooperative application actions are registered for Communication and Logging. Their original RTOS task handles remain unchanged. Safety, Charging and Sensor are never deleted or silently reinitialized by this module. An actionable fault in one of those protected functions enters the simulated safe-state/reset path rather than pretending that a generic communication-context reset can repair it.
Limited Communication keeps a shorter periodic workload representing essential heartbeat/service and skips the optional path. Limited Logging retains queue draining and skips optional injected work. Successful limited-mode verification is reported as `LimitedStable`, with `LimitedOK=1` and `FullOK=0`. Its pending flag and recovery ownership remain latched; limited service is not reported as full recovery or automatically switched back to the failing optional path.
After correcting an external cause, future monitor-owned control code may call `HR_RequestFullService(task)`. This queues a new application reinitialization and requires a new acknowledgement/generation and three fresh Healthy windows before full service/base priority/clear flags can return. It is not automatically called in the main demonstration.
If Logging recovery alone fails, its state becomes `Unavailable` and the request remains pending; the module does not reset healthy charging solely for failed diagnostics. It makes no claim that the unavailable logger is Healthy or successfully isolated. Protected-task health/progress continue to be monitored independently.
For Communication essential-service failure, failed/unacknowledged L3 recovery can request controlled reset. The platform callback first disables the simulated charging output, and charging calculations cannot re-enable it while the safe-state latch is set. Reset requires confirmation of that modeled safe state, a successfully printed notice, at least a 1000 ms notice interval and a registered reset callback. The reset callback invokes `NVIC_SystemReset()`; this is a real MCU reset path in a firmware build. The host tests substitute a counter and do not reset hardware.
There is one reinitialization attempt per recovery episode, with at most three issued reinitializations per task per boot across recurring episodes. A task checkpoint acknowledgement times out after 2000 ms. Verification escalates after three consecutive bad windows or six unsuccessful observation windows. These are explicit implementation parameters, not an EV charging standard.
Reading the output
Keep reading the four existing tables as in v4. The new recovery table adds:
Field/state	Meaning
`L2WaitAck`	Controller queued reinitialization; it has not yet consumed the task acknowledgement.
`L2Verify`	Action acknowledged; fresh post-action health is being verified.
`Recovered`	Three valid clean windows and verified base restoration completed.
`L3WaitAck` / `L3Verify`	Limited-service action requested / being verified.
`LimitedStable`	Limited functionality is sustained; pending request remains.
`Unavailable`	Noncritical recovery failed; its unresolved status is retained.
`L4ResetPending` / `L4ResetIssued`	Safe-state/reset path requested / reset callback invoked.
`Gen`	Acknowledged application generation. Zero means no successful action has yet been acknowledged by the controller.
`Good`, `Bad`	Current or last recovery-verification streaks, separate from the scheduler's counters.
`L2Act`, `L3Act`	Task callbacks whose acknowledgements were processed, including failed callbacks. A queued request alone is not counted as an executed action.
`FullOK`, `LimitOK`	Verified full-recovery and limited-service outcomes.
An action can execute while the monitor prints cycle 8, and its acknowledgement can be processed on cycle 9. The log's `ack_tick` records when the task actually acknowledged. This is normal sampling behaviour.
`recovery_ms` runs from the controller accepting the pending request to verified outcome; it includes the three-window verification interval. It is not the CPU execution time of the reinitialization callback. The final table also reports request, acknowledgement and verification ticks, and `SameTask=YES` when the task handle is unchanged. Snapshot fault status in the fault table belongs to the capture/decision instant; jobs can run while UART printing proceeds.
The injector's first clean-exposure window is derived from job exposure tags. In a limited-service case, this means the optional fault effect is bypassed, not that an external source was repaired. Recovery mode and `FullOK/LimitOK` make that distinction visible.
Optional validation profiles — separate from the default demonstration
Do not change the default for your first board run. The implementation also contains these optional build-time profiles for later evaluation:
`MRG_DEMO_CASE`	Fault model	Expected route	Report length
0 — default	Internal Communication state latch	L1 → L2 → verified full recovery	13
1	Persistent fault in optional Communication work	L1 → L2 unsuccessful → L3 limited/stable; charging continues	18
2	Persistent fault in essential Communication work	L1 → L2 unsucce# Mr.G v5 — selective hierarchical recovery

This update implements the master document's hierarchy on top of the board-tested v4 scheduler:

1. **Level 1:** existing health-aware scheduling adaptation.
2. **Level 2:** cooperative reinitialization of the affected task's application context, followed by measured verification.
3. **Level 3:** limited service for recoverable noncritical functionality, with its limited status explicitly retained.
4. **Level 4:** controlled MCU reset after a simulated safe-state confirmation, printed notice and delay, when essential service cannot be recovered.

The default remains **one short 13-observation demonstration**. All original-style Runtime Monitor, Health Engine, Fault Injection and Adaptive Scheduler tables remain, plus a new **SELECTIVE HIERARCHICAL RECOVERY** table and actual recovery results in the final summary.

## What changed from the working v4 experiment

V4 switched the injector OFF on cycle 9. V5 does **not** schedule fault withdrawal. On cycle 2 it corrupts simulated Communication application state with a finite CPU-overload latch. That state persists until its owning task reinitializes the context. The recovery module does not call an injector-OFF function, assign a Healthy score or branch on experiment cycle numbers.

This is **task-level application-state recovery inside the same FreeRTOS task**. It does not delete/recreate the RTOS task, replace its stack or pretend that a task-object restart occurred. The original static task handles, stacks, priorities and task periods are retained. The master document permits recovery/restart of affected functionality; this implements the reinitialization approach for the current software workloads.

## Installation in STM32CubeIDE

Back up the current working v4 project. Copy **all 18 integration files** to the matching paths. Interfaces changed; do not mix v4 headers with v5 sources.

| Files | Purpose |
|---|---|
| `Core/Inc/hierarchical_recovery.h`, `Core/Src/hierarchical_recovery.c` — NEW | Recovery state machine, task-owned command/acknowledgement protocol, fresh-generation verification, escalation, action counters and table. |
| `Core/Inc/task_service.h`, `Core/Src/task_service.c` — NEW | Explicit simulated application context, session reinitialization, limited service, persistent/external fault models and simulated charging-output gate. |
| `Core/Inc/project_experiment.h` — NEW | Default short demonstration and optional separate validation profiles. |
| `Core/Inc/adaptive_scheduler.h`, `Core/Src/adaptive_scheduler.c` | Preserve the v4 scheduling policy; add recovery ownership so priority/flags remain held until recovery verification succeeds. |
| `Core/Inc/runtime_monitor.h`, `Core/Src/runtime_monitor.c` | Preserve scoring and tables; add task generations, mixed-window detection and actual injected-job exposure tags. Lifetime counters are retained. |
| `Core/Inc/fault_injection.h`, `Core/Src/fault_injection.c` | Inject once, without a timed OFF; identify fault exposure from measured jobs. |
| `Core/Inc/experiment_report.h`, `Core/Src/experiment_report.c` | Keep existing tables/timeline and report real recovery actions, verification times and unchanged task handles. |
| `Core/Inc/health_engine.h`, `Core/Src/health_engine.c` | Preserve the independent Health Engine table and current scoring interface. |
| `Core/Inc/FreeRTOSConfig.h` | Preserve the existing task-switch trace hooks and RTOS configuration. |
| `Core/Src/main.c` | Register actions; invoke recovery after scheduling; process Communication/Logging commands at job boundaries; add safe-state/reset callbacks and recovery table. Monitoring continues after the report finishes. |
| `EV_Charging_SelfHealing.ioc` | Retain matching base priorities and the existing 512-word monitor stack. No new RTOS task is needed. |

1. Add the two new `.c` files to the project build. Ensure the three new headers are in `Core/Inc`.
2. Compile exactly one copy of each module. Exclude old scheduler variants and backup source copies from **all** build configurations, or keep them outside compiled folders.
3. **Do not copy `tests` into firmware source/include paths. Never define `MRG_HOST_TEST` in CubeIDE.** Those files contain host-only HAL/RTOS stubs and simulated output.
4. Use the default `MRG_DEMO_CASE=0`. No CubeMX regeneration is needed before the first run; matching `.ioc` and `main.c` files are supplied.
5. **Clean, then rebuild the complete project including the FreeRTOS kernel.** Missing task-switch hooks produce `[CONFIG ERROR]`; adaptation/recovery are held rather than using invalid CPU measurements.
6. Flash, connect Tera Term at **115200, 8-N-1**, and **press the board's physical RESET button** to start the demonstration. Capture startup through `EXPERIMENT COMPLETE`. A terminal width around 100 columns helps keep the new table readable.

The physical RESET step matters: an intentional **software-reset boot suppresses automatic fault reinjection**, so the last-resort reset cannot immediately trigger the same scripted fault again. A debugger may also use software reset. If the table says `software-reset boot=YES` and injection is suppressed, press physical RESET or power-cycle to deliberately repeat the demonstration. Reset-cause flags are read before being cleared at startup.

This is an integration update, not a full CubeIDE export. Keep your existing HAL, CMSIS, FreeRTOS distribution, startup code, linker script and project metadata. `changes_from_v4.patch` is for reviewing differences, not required for installation.

## Expected default sequence — 13 observations

These events passed the host simulation. Actual event timing on the board must follow its valid measurements; it is not forced by the scheduler/recovery controller.

| Cycle | Expected measurement/action | Communication priority |
|---:|---|---:|
| 1 | All five tasks Healthy; normal baseline. | 41 |
| 2 | Healthy captured window; latch the simulated Communication fault afterwards. | 41 |
| 3 | First bad observation. | 41 |
| 4 | Second bad observation. | 41 |
| 5 | Third bad observation; verified Level-1 demotion below Logging. | **38** |
| 6 | First further bad observation after adaptation. | 38 |
| 7 | Second further bad observation. | 38 |
| 8 | Third further bad observation; pending `0x04`; Level-2 command queued. | 38 |
| Between 8 and 9 | Communication reaches its next job boundary, reinitializes its own context, advances generation and acknowledges. | 38 |
| 9 | First fresh Healthy generation-2 window; controller observes the acknowledgement. | 38 |
| 10 | Second fresh Healthy window; priority and pending request stay held. | 38 |
| 11 | Third fresh Healthy window; verify full recovery, restore base priority and clear flags. | **41** |
| 12–13 | Stable operation; final summary at 13. | 41 |

The injected workload targets approximately 5 ms, but actual CPU execution is measured. Your previous v4 board run measured roughly 4.36 ms; v5 adds small instrumentation costs, so do not copy model numbers into the final report.

The default successful outcome is **one L2 action, one verified full recovery, zero L3 actions and zero MCU resets**. L3/L4 evidence rows show `NOT OBSERVED` because escalation was unnecessary. That is expected: the hierarchy stops once the least disruptive effective action succeeds.

After the final report, runtime monitoring, adaptive decisions, recovery updates and reset dispatch continue once per observation. Repeated full tables stop; later recovery events can still print. This keeps the firmware's recovery loop alive beyond the demonstration.

## What the new module actually does

The Runtime Monitor remains the single owner of recovery decisions and scheduler changes. The application task executes its own bounded reinitialization/limited-service callback at a checkpoint **before** `RTM_TaskStartAt`, outside application locks. No callback deletes a task or resets shared diagnostic/health structures.

A monotonically advancing command token identifies each request. A task acknowledges only its current token. Repeated monitor calls on the same frozen window cannot count twice. Acknowledgement alone is insufficient for success: the controller then requires **three consecutive fresh Healthy windows with completed-job progress**, no window errors/misses and a normal sampled task state.

Every successful task action advances a task generation. Old frozen windows and windows containing jobs from both generations cannot verify recovery. This prevents an earlier Healthy sample from being credited to a later reinitialization. Health scores are still computed by the existing health engine from measured work; recovery does not write scores or erase cumulative errors, releases or deadline misses.

The adapted priority and request flags stay held while the controller verifies recovery. Full success additionally requires a successful base-priority API call/readback. A failed restoration proceeds to the fallback path instead of claiming success.

The default internal fault models corrupted session/work state. `APP_Reinitialize` resets session progress and clears that internal latch. An external persistent source deliberately survives reinitialization. This distinction allows testing whether recovery works for a recoverable state fault and whether escalation works when the cause is outside that state.

## Selectivity, graceful degradation and reset policy

Automatic cooperative application actions are registered for **Communication and Logging**. Their original RTOS task handles remain unchanged. Safety, Charging and Sensor are never deleted or silently reinitialized by this module. An actionable fault in one of those protected functions enters the simulated safe-state/reset path rather than pretending that a generic communication-context reset can repair it.

Limited Communication keeps a shorter periodic workload representing essential heartbeat/service and skips the optional path. Limited Logging retains queue draining and skips optional injected work. Successful limited-mode verification is reported as **`LimitedStable`**, with `LimitedOK=1` and `FullOK=0`. Its pending flag and recovery ownership remain latched; limited service is not reported as full recovery or automatically switched back to the failing optional path.

After correcting an external cause, future monitor-owned control code may call `HR_RequestFullService(task)`. This queues a new application reinitialization and requires a new acknowledgement/generation and three fresh Healthy windows before full service/base priority/clear flags can return. It is not automatically called in the main demonstration.

If Logging recovery alone fails, its state becomes **`Unavailable`** and the request remains pending; the module does not reset healthy charging solely for failed diagnostics. It makes no claim that the unavailable logger is Healthy or successfully isolated. Protected-task health/progress continue to be monitored independently.

For Communication essential-service failure, failed/unacknowledged L3 recovery can request controlled reset. The platform callback first disables the **simulated** charging output, and charging calculations cannot re-enable it while the safe-state latch is set. Reset requires confirmation of that modeled safe state, a successfully printed notice, at least a 1000 ms notice interval and a registered reset callback. The reset callback invokes `NVIC_SystemReset()`; this is a real MCU reset path in a firmware build. The host tests substitute a counter and do not reset hardware.

There is one reinitialization attempt per recovery episode, with at most **three issued reinitializations per task per boot** across recurring episodes. A task checkpoint acknowledgement times out after 2000 ms. Verification escalates after three consecutive bad windows or six unsuccessful observation windows. These are explicit implementation parameters, not an EV charging standard.

## Reading the output

Keep reading the four existing tables as in v4. The new recovery table adds:

| Field/state | Meaning |
|---|---|
| `L2WaitAck` | Controller queued reinitialization; it has not yet consumed the task acknowledgement. |
| `L2Verify` | Action acknowledged; fresh post-action health is being verified. |
| `Recovered` | Three valid clean windows and verified base restoration completed. |
| `L3WaitAck` / `L3Verify` | Limited-service action requested / being verified. |
| `LimitedStable` | Limited functionality is sustained; pending request remains. |
| `Unavailable` | Noncritical recovery failed; its unresolved status is retained. |
| `L4ResetPending` / `L4ResetIssued` | Safe-state/reset path requested / reset callback invoked. |
| `Gen` | Acknowledged application generation. Zero means no successful action has yet been acknowledged by the controller. |
| `Good`, `Bad` | Current or last recovery-verification streaks, separate from the scheduler's counters. |
| `L2Act`, `L3Act` | Task callbacks whose acknowledgements were processed, including failed callbacks. A queued request alone is not counted as an executed action. |
| `FullOK`, `LimitOK` | Verified full-recovery and limited-service outcomes. |

An action can execute while the monitor prints cycle 8, and its acknowledgement can be processed on cycle 9. The log's `ack_tick` records when the task actually acknowledged. This is normal sampling behaviour.

`recovery_ms` runs from the controller accepting the pending request to verified outcome; it includes the three-window verification interval. It is not the CPU execution time of the reinitialization callback. The final table also reports request, acknowledgement and verification ticks, and `SameTask=YES` when the task handle is unchanged. Snapshot fault status in the fault table belongs to the capture/decision instant; jobs can run while UART printing proceeds.

The injector's first clean-exposure window is derived from job exposure tags. In a limited-service case, this means the optional fault effect is bypassed, not that an external source was repaired. Recovery mode and `FullOK/LimitOK` make that distinction visible.

## Optional validation profiles — separate from the default demonstration

Do not change the default for your first board run. The implementation also contains these optional build-time profiles for later evaluation:

| `MRG_DEMO_CASE` | Fault model | Expected route | Report length |
|---:|---|---|---:|
| **0 — default** | Internal Communication state latch | L1 → L2 → verified full recovery | **13** |
| 1 | Persistent fault in optional Communication work | L1 → L2 unsuccessful → L3 limited/stable; charging continues | 18 |
| 2 | Persistent fault in essential Communication work | L1 → L2 unsuccessful → L3 unsuccessful → safe state → actual MCU reset | Up to reset; configured 18 |

In profile 2, the host log can show later observations because its reset callback is a stub. The physical MCU reboots around reset dispatch and prints a new startup banner instead of completing those first-boot observations. Its scripted reinjection is suppressed on that software-reset boot. This prevents a synthetic reboot loop; it is not evidence that a real external physical fault was repaired by reset.

For matching comparison builds, use `HR_ENABLE_RECOVERY=0` for adaptation only, and also `AS_ENABLE_ADAPTATION=0` for fixed priority. All use the same once-injected internal state fault. Without application recovery, that latch remains active: no timer removes it to make a baseline look recovered.

## Verification performed and remaining board check

Run all supplied computer checks from this directory:

```sh
ASAN_OPTIONS=detect_leaks=0 sh tests/run_host_tests.sh
```

All five variants passed strict GCC C11 warnings, AddressSanitizer and UndefinedBehaviorSanitizer. Tests exercise the actual AS/RTM/HE/FI/HR/application/report modules, supplied trace macros, preemptive periodic service and an approximate 115200-baud UART-time model. The longest default report was about **398 ms in that model**, below the 1000 ms observation interval.

The main-source check also syntax-compiles the actual startup USER section and all six actual task bodies using host declarations, and checks the calibration branch without the host define. This is useful integration coverage, but it is **not** a full STM32 HAL/FreeRTOS/Cortex-M build or an actual kernel execution test.

Additional tests cover interrupted verification, stale/duplicate/mixed-generation samples, wrong task owner, callback failure, priority restoration failure, task progress/state errors, acknowledgement timeouts, bounded recurring attempts, explicit limited-to-full revalidation, Logging-only failure without global reset, reset notice/safe-state gates and software-reboot injection suppression.

Every file named `HOST_*_trace.txt` is **HOST SIMULATION ONLY — NOT STM32 BOARD OUTPUT**. Model task service times, response improvements and recovery times must not be presented as hardware results.

**Remaining:** Clean/build in your CubeIDE project, flash, physically RESET, and collect the default 13-cycle Tera Term log. Verify the L2 request/acknowledgement, fresh generation, three-window confirmation, base restoration, protected-task progress and final evidence checks. No physical board build/run, Cortex-M stack high-water measurement or real reset was performed here. The 512-word monitor stack is retained from v4; its hardware margin is not measured by these host checks.

The controller needs the monitor to execute and cooperative targets to reach checkpoints. It handles the finite software disturbances used by this project; it does not add a hardware watchdog for disabled interrupts or an arbitrary non-yielding higher-priority loop. Safe-state/output handling is a **software model**, matching the master document's current hardware scope. Physical power-stage interlocks and actuator acknowledgement would require separate integration.

## Platform references

- ARM CMSIS-Core, `NVIC_SystemReset`: https://arm-software.github.io/CMSIS_5/Core/html/group__NVIC__gr.html
- ST STM32F4 HAL/LL manual (reset-cause flags and clearing): https://www.st.com/resource/en/user_manual/um1725-description-of-stm32f4-hal-and-lowlayer-drivers-stmicroelectronics.pdf
ssful → L3 unsuccessful → safe state → actual MCU reset	Up to reset; configured 18
In profile 2, the host log can show later observations because its reset callback is a stub. The physical MCU reboots around reset dispatch and prints a new startup banner instead of completing those first-boot observations. Its scripted reinjection is suppressed on that software-reset boot. This prevents a synthetic reboot loop; it is not evidence that a real external physical fault was repaired by reset.
For matching comparison builds, use `HR_ENABLE_RECOVERY=0` for adaptation only, and also `AS_ENABLE_ADAPTATION=0` for fixed priority. All use the same once-injected internal state fault. Without application recovery, that latch remains active: no timer removes it to make a baseline look recovered.
Verification performed and remaining board check
Run all supplied computer checks from this directory:
```sh
ASAN_OPTIONS=detect_leaks=0 sh tests/run_host_tests.sh
```
All five variants passed strict GCC C11 warnings, AddressSanitizer and UndefinedBehaviorSanitizer. Tests exercise the actual AS/RTM/HE/FI/HR/application/report modules, supplied trace macros, preemptive periodic service and an approximate 115200-baud UART-time model. The longest default report was about 398 ms in that model, below the 1000 ms observation interval.
The main-source check also syntax-compiles the actual startup USER section and all six actual task bodies using host declarations, and checks the calibration branch without the host define. This is useful integration coverage, but it is not a full STM32 HAL/FreeRTOS/Cortex-M build or an actual kernel execution test.
Additional tests cover interrupted verification, stale/duplicate/mixed-generation samples, wrong task owner, callback failure, priority restoration failure, task progress/state errors, acknowledgement timeouts, bounded recurring attempts, explicit limited-to-full revalidation, Logging-only failure without global reset, reset notice/safe-state gates and software-reboot injection suppression.
Every file named `HOST_*_trace.txt` is HOST SIMULATION ONLY — NOT STM32 BOARD OUTPUT. Model task service times, response improvements and recovery times must not be presented as hardware results.
Remaining: Clean/build in your CubeIDE project, flash, physically RESET, and collect the default 13-cycle Tera Term log. Verify the L2 request/acknowledgement, fresh generation, three-window confirmation, base restoration, protected-task progress and final evidence checks. No physical board build/run, Cortex-M stack high-water measurement or real reset was performed here. The 512-word monitor stack is retained from v4; its hardware margin is not measured by these host checks.
The controller needs the monitor to execute and cooperative targets to reach checkpoints. It handles the finite software disturbances used by this project; it does not add a hardware watchdog for disabled interrupts or an arbitrary non-yielding higher-priority loop. Safe-state/output handling is a software model, matching the master document's current hardware scope. Physical power-stage interlocks and actuator acknowledgement would require separate integration.
Platform references
ARM CMSIS-Core, `NVIC_SystemReset`: https://arm-software.github.io/CMSIS_5/Core/html/group__NVIC__gr.html
ST STM32F4 HAL/LL manual (reset-cause flags and clearing): https://www.st.com/resource/en/user_manual/um1725-description-of-stm32f4-hal-and-lowlayer-drivers-stmicroelectronics.pdf[READ_ME_FIRST.md](https://github.com/user-attachments/files/35264389/READ_ME_FIRST.md)
