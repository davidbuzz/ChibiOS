/*
    ChibiOS - Copyright (C) 2006-2026 Giovanni Di Sirio.

    This file is part of ChibiOS.

    ChibiOS is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation version 3 of the License.

    ChibiOS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file    rt/src/chinstances.c
 * @brief   OS instances code.
 *
 * @addtogroup instances
 * @details OS instances management.
 * @{
 */

#include "ch.h"

/*===========================================================================*/
/* Module local definitions.                                                 */
/*===========================================================================*/

#if CH_CFG_SMP_MODE == TRUE
/*
 * SMP registry-protection helpers for chInstanceObjectInit.
 *
 * Problem: chInstanceObjectInit modifies the global ch_system.reglist
 * (a linked list) twice during init — via REG_INSERT (for the main thread)
 * and inside chThdSpawnSuspendedI (for the idle thread).  Neither holds
 * the inter-core spinlock.  Core0 may be running chThdCreateStatic, which
 * DOES hold the spinlock during its own reglist modification, creating a
 * data race that corrupts the list.
 *
 * Fix: wrap each registry modification with a brief critical section that
 * uses PRIMASK=1 (cpsid i) to block ALL IRQs on this core — including
 * TIMER0_IRQ_1 (priority 2, which bypasses BASEPRI=16) — and then acquires
 * the spinlock.  PRIMASK prevents the timer ISR from re-acquiring the same
 * spinlock on the same core (which would deadlock).  The window is tiny
 * (a handful of list-pointer writes).
 */
#define SMP_REG_LOCK()   do {                         \
  __disable_irq();   /* PRIMASK = 1 */                \
  port_spinlock_take();                               \
} while (0)
#define SMP_REG_UNLOCK() do {                         \
  port_spinlock_release();                            \
  __enable_irq();    /* PRIMASK = 0 */                \
} while (0)
/*
 * SMP_REG_KEEP_LOCK: like SMP_REG_LOCK but does NOT release the spinlock.
 * Used for the FINAL critical section in chInstanceObjectInit so that the
 * function returns with the spinlock HELD.  This satisfies the ChibiOS SMP
 * I-Lock contract (BASEPRI=16 + spinlock) that chSysUnlock() in c1_main
 * expects to dissolve: port_unlock() releases the spinlock FIRST and then
 * sets BASEPRI=0.  If the spinlock is not held when chSysUnlock() runs, the
 * port_spinlock_release() call writes 0 to the SIO SPINLOCK register even
 * when another core holds it, silently breaking mutual exclusion and
 * corrupting the scheduler data structures.
 */
#define SMP_REG_KEEP_LOCK() do {                      \
  /* Keep spinlock held; re-enable IRQs only. */      \
  __enable_irq();    /* PRIMASK = 0 */                \
} while (0)
#else
#define SMP_REG_LOCK()      do {} while (0)
#define SMP_REG_UNLOCK()    do {} while (0)
#define SMP_REG_KEEP_LOCK() do {} while (0)
#endif /* CH_CFG_SMP_MODE */

/*===========================================================================*/
/* Module exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Module local types.                                                       */
/*===========================================================================*/

/*===========================================================================*/
/* Module local variables.                                                   */
/*===========================================================================*/

/*===========================================================================*/
/* Module local functions.                                                   */
/*===========================================================================*/

#if (CH_CFG_NO_IDLE_THREAD == FALSE) || defined(__DOXYGEN__)
/**
 * @brief   This function implements the idle thread infinite loop.
 * @details The function puts the processor in the lowest power mode capable
 *          to serve interrupts.<br>
 *          The priority is internally set to the minimum system value so
 *          that this thread is executed only if there are no other ready
 *          threads in the system.
 *
 * @param[in] p         the thread parameter, unused in this scenario
 */
static void __idle_thread(void *p) {

  (void)p;

  while (true) {
    /*lint -save -e522 [2.2] Apparently no side effects because it contains
      an asm instruction.*/
    port_wait_for_interrupt();
    /*lint -restore*/
    CH_CFG_IDLE_LOOP_HOOK();
  }
}
#endif /* CH_CFG_NO_IDLE_THREAD == FALSE */

/*===========================================================================*/
/* Module exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Initializes a system instance.
 * @note    The system instance is in I-Lock state after initialization.
 *
 * @param[out] oip      pointer to an @p os_instance_t object
 * @param[in] oicp      pointer to an @p os_instance_config_t object
 *
 * @special
 */

/* RP2350 SMP bring-up: fine-grained canary to locate core1 crash inside
 * chInstanceObjectInit.  Written unconditionally so core1's progress
 * overwrites the values left by core0's successful run through the same
 * function.  Read c1_inst_stage via GDB/OpenOCD after halt:
 *   0x40 = entered, core_id/config set, about to call port_init
 *   0x41 = port_init returned (BASEPRI=16, FPU, MPU, SIO FIFO IRQ armed)
 *   0x42 = ch_pqueue_init returned (rlist ready)
 *   0x43 = __vt_object_init returned (vtlist ready)
 *   0x44 = __dbg_object_init returned
 *   0x45 = chThdObjectInit (main thread) returned
 *   0x46 = REG_INSERT done, state=CURRENT set
 *   0x47 = __thd_stackfill for idle thread done, about to chThdSpawnRunningI
 *   0x48 = chThdSpawnRunningI returned (function about to return)
 */
extern volatile uint32_t c1_inst_stage;
volatile uint32_t c1_inst_stage = 0xDEAD0001U;

void chInstanceObjectInit(os_instance_t *oip,
                          const os_instance_config_t *oicp) {
  core_id_t core_id;

  /* Registering into the global system structure.*/
#if CH_CFG_SMP_MODE == TRUE
  core_id = port_get_core_id();
#else
  core_id = 0U;
#endif
  chDbgAssert(ch_system.instances[core_id] == NULL, "instance already registered");
  ch_system.instances[core_id] = oip;

  /* Core associated to this instance.*/
  oip->core_id = core_id;

  /* Keeping a reference to the configuration data.*/
  oip->config = oicp;

  c1_inst_stage = 0x40U;
  c1_inst_stage = 0x4AU;  /* just before bl port_init */

  /* Port initialization for the current instance.*/
  port_init(oip);

  c1_inst_stage = 0x41U;

  /* Ready list initialization.*/
  ch_pqueue_init(&oip->rlist.pqueue);

#if (CH_CFG_USE_REGISTRY == TRUE) && (CH_CFG_SMP_MODE == FALSE)
  /* Registry initialization when SMP mode is disabled.*/
  __reg_object_init(&oip->reglist);
#endif

#if CH_CFG_SMP_MODE == FALSE
  /* RFCU initialization when SMP mode is disabled.*/
  __rfcu_object_init(&oip->rfcu);
#endif

  c1_inst_stage = 0x42U;

  /* Virtual timers list initialization — inlined with sub-stage canaries.
   * 0x421 = after ch_dlist_init (dlist.next/prev/delta written)
   * 0x422 = after lasttime = 0
   * 0x423 = after lastdelta = CH_CFG_ST_TIMEDELTA
   * 0x424 = before chVTGetSystemTimeX (stGetCounter call)
   * 0x425 = after laststamp written
   */
  {
    virtual_timers_list_t *_vtlp = &oip->vtlist;
    ch_dlist_init(&_vtlp->dlist);
    c1_inst_stage = 0x421U;
    _vtlp->lasttime = (systime_t)0;
    c1_inst_stage = 0x422U;
    _vtlp->lastdelta = (sysinterval_t)CH_CFG_ST_TIMEDELTA;
    c1_inst_stage = 0x423U;
    c1_inst_stage = 0x424U;
    _vtlp->laststamp = (systimestamp_t)chVTGetSystemTimeX();
    c1_inst_stage = 0x425U;
  }

  c1_inst_stage = 0x43U;

  /* Debug support initialization.*/
  __dbg_object_init(&oip->dbg);

  c1_inst_stage = 0x44U;

#if CH_DBG_TRACE_MASK != CH_DBG_TRACE_MASK_DISABLED
  /* Trace buffer initialization.*/
  __trace_object_init(&oip->trace_buffer);
#endif

  /* Statistics initialization.*/
#if CH_DBG_STATISTICS == TRUE
  __stats_object_init(&oip->kernel_stats);
#endif

  /* Now this instruction flow becomes the main thread or the idle thread
     depending on the CH_CFG_NO_IDLE_THREAD setting.*/
  {
#if CH_CFG_NO_IDLE_THREAD == FALSE
    const THD_DECL(main_thd_desc,
                   "main", oicp->cstack_base, oicp->cstack_end,
                   NORMALPRIO, NULL, NULL, oip
    );

    oip->rlist.current = chThdObjectInit(&oip->mainthread, &main_thd_desc);
#else
    const THD_DECL(idle_thd_desc,
                   "idle", oicp->cstack_base, oicp->cstack_end,
                   IDLEPRIO, NULL, NULL, oip
    );

    oip->rlist.current = chThdObjectInit(&oip->idlethread, &idle_thd_desc);
#endif
  }

  c1_inst_stage = 0x45U;

#if CH_CFG_USE_REGISTRY == TRUE
  SMP_REG_LOCK();
  REG_INSERT(oip, oip->rlist.current);
  SMP_REG_UNLOCK();
#endif

  /* Setting up the caller as current thread.*/
  oip->rlist.current->state = CH_STATE_CURRENT;

  c1_inst_stage = 0x46U;

#if CH_DBG_STATISTICS == TRUE
  /* Starting measurement for this thread.*/
  chTMStartMeasurementX(&oip->rlist.current->stats);
#endif

  c1_inst_stage = 0x461U;  /* after chTMStartMeasurementX */

  /* User instance initialization hook.*/
  CH_CFG_OS_INSTANCE_INIT_HOOK(oip);

  c1_inst_stage = 0x462U;  /* after CH_CFG_OS_INSTANCE_INIT_HOOK */

#if CH_CFG_NO_IDLE_THREAD == FALSE
  {
    c1_inst_stage = 0x463U;  /* before THD_DECL for idle thread */
    const THD_DECL(idle_thd_desc,
                   "idle", oicp->idlestack_base, oicp->idlestack_end,
                   IDLEPRIO, __idle_thread, NULL, oip
    );
    c1_inst_stage = 0x464U;  /* after THD_DECL (idle stack addrs resolved) */

#if CH_DBG_FILL_THREADS == TRUE
    __thd_stackfill((uint8_t *)idle_thd_desc.wbase,
                    (uint8_t *)idle_thd_desc.wend);
    c1_inst_stage = 0x465U;  /* after __thd_stackfill */
#endif

  c1_inst_stage = 0x47U;

    /* This thread has the lowest priority in the system, its role is just to
       serve interrupts in its context while keeping the lowest energy saving
       mode compatible with the system status.
       chThdSpawnRunningI → chThdSpawnSuspendedI modifies ch_system.reglist;
       protect with the same SMP lock used for REG_INSERT above. */
    SMP_REG_LOCK();
    (void) chThdSpawnRunningI(&oip->idlethread, &idle_thd_desc);
    SMP_REG_UNLOCK();
  }
#endif /* CH_CFG_NO_IDLE_THREAD == FALSE */

  c1_inst_stage = 0x48U;
}

/** @} */
