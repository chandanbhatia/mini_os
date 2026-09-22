# Mini Cooperative OS

A small, self-contained cooperative OS kernel written in C for ARM Cortex-M microcontrollers. Built as a learning and demonstration project — readable source, no external dependencies, no heap.

---

## Overview

This OS provides the minimal kernel primitives needed to structure embedded firmware into independent, cooperating tasks without the weight and complexity of a full RTOS like FreeRTOS.

**Scheduling model:** cooperative round-robin. A task owns the CPU until it voluntarily releases it by calling `os_sleep_ms()`, `os_yield()`, or blocking on a queue or semaphore. No task can be preempted externally.

**Key properties:**
- Zero heap usage — all kernel and task state is statically allocated
- No external dependencies beyond CMSIS device headers and the STM32 HAL
- Context switch in ~8 ARM instructions (4-line naked assembly function)
- ISR-safe queue send and semaphore signal
- Configurable queue depth and message size per queue instance
- Stack overflow detection at runtime 

---

## Demo Application

Four tasks demonstrate the scheduler and both primitives:

```
led_1_task          — blinks LED 1 every 25 ms (fast heartbeat)

queue_pub_task      — sends 8-byte incrementing counter data to
          │           queue every 100 ms
          ▼
     [OS_Queue]
          │
          ▼
queue_recv_task     — receives from queue, stores in a 40-slot
                      ring buffer for inspection via debugger

timer_cb_50ms ──[queue_send_from_isr]──► queue_recv_task (extra source)
              ──[sem_signal_from_isr]──► LED_2_task
                                              │
                                         toggles LED 2 on each
                                         semaphore signal (50 ms rate)
```
---

## Known Limitations

| Limitation | Notes |
|---|---|
| No preemption | A task that never yields starves all others. Use `os_sleep_ms()` or any other blocking call at the top of every task loop. |
| One blocked waiter per primitive | Each queue and semaphore supports one blocked task at a time. |
| No task priorities | Round-robin scheduling only — all ready tasks share equal CPU time. |
| Finite timeout max ~24 days | Signed subtraction comparison works for timeouts < 2³¹ ms. Use `OS_WAIT_FOREVER` for indefinite waits. |
| No preemptive stretch | PendSV-based preemption would require saving r0–r3, r12, xPSR (hardware does this via exception entry frame) plus a full 17-word initial stack frame per task. |

---

## File Structure

```
os/
├── os.h             — Scheduler public API
├── os.c             — Scheduler, context switch, internal task helpers
├── os_internal.h    — PRIMASK helpers, deadline check, internal API
├── os_queue.h       — Queue API
├── os_queue.c       — Queue implementation
├── os_sem.h         — Semaphore API
└── os_sem.c         — Semaphore implementation

demo/
└── demo_app.c       — Four-task demo (LED blink, queue, semaphore)
```

---

## License

MIT License. Free to use, modify, and distribute.
