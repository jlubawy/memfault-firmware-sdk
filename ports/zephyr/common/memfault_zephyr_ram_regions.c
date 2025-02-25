//! @file
//!
//! Copyright (c) Memfault, Inc.
//! See License.txt for details
//!
//! Implements convenience APIs that can be used when building the set of
//! RAM regions to collect as part of a coredump. See header for more details/

#include "memfault/ports/zephyr/coredump.h"

#include <zephyr.h>
#include <kernel.h>
#include <kernel_structs.h>
#include <version.h>

#include "memfault/components.h"
#include "memfault/ports/zephyr/version.h"

// Use this config flag to manually select the old data region names
#if !defined(MEMFAULT_ZEPHYR_USE_OLD_DATA_REGION_NAMES)
#define MEMFAULT_ZEPHYR_USE_OLD_DATA_REGION_NAMES 0
#endif

static struct k_thread *s_task_tcbs[CONFIG_MEMFAULT_COREDUMP_MAX_TRACKED_TASKS];
#define EMPTY_SLOT 0

static bool prv_find_slot(size_t *idx, struct k_thread *desired_tcb) {
  for (size_t i = 0; i < MEMFAULT_ARRAY_SIZE(s_task_tcbs); i++) {
    if (s_task_tcbs[i] == desired_tcb) {
      *idx = i;
      return true;
    }
  }

  return false;
}

// We intercept calls to arch_new_thread() so we can track when new tasks
// are created
//
// It would be nice to use '__builtin_types_compatible_p' and '__typeof__' to
// enforce strict abi matching in the wrapped functions, but the target
// functions are declared in private zephyr ./kernel/include files, which are
// not really supposed to be accessed from user code and would require some
// dubious path hacks to get to.
void __wrap_arch_new_thread(struct k_thread *thread, k_thread_stack_t *stack,
                            char *stack_ptr, k_thread_entry_t entry,
                            void *p1, void *p2, void *p3);
void __real_arch_new_thread(struct k_thread *thread, k_thread_stack_t *stack,
                            char *stack_ptr, k_thread_entry_t entry,
                            void *p1, void *p2, void *p3);

void __wrap_arch_new_thread(struct k_thread *thread, k_thread_stack_t *stack,
                            char *stack_ptr, k_thread_entry_t entry,
                            void *p1, void *p2, void *p3) {
  size_t idx = 0;
  const bool slot_found = prv_find_slot(&idx, EMPTY_SLOT);
  if (slot_found) {
    s_task_tcbs[idx] = thread;
  }

  __real_arch_new_thread(thread, stack, stack_ptr, entry, p1, p2, p3);
}

void __wrap_z_thread_abort(struct k_thread *thread);
void __real_z_thread_abort(struct k_thread *thread);

void __wrap_z_thread_abort(struct k_thread *thread) {
  size_t idx = 0;
  const bool slot_found = prv_find_slot(&idx, thread);
  if (slot_found) {
    s_task_tcbs[idx] = EMPTY_SLOT;
  }

  __real_z_thread_abort(thread);
}

MEMFAULT_WEAK
size_t memfault_platform_sanitize_address_range(void *start_addr, size_t desired_size) {
  // NB: This only works for MCUs which have a contiguous RAM address range. (i.e Any MCU in the
  // nRF53, nRF52, and nRF91 family). All of these MCUs have a contigous RAM address range so it is
  // sufficient to just look at _image_ram_start/end from the Zephyr linker script
  extern uint32_t _image_ram_start[];
  extern uint32_t _image_ram_end[];

  const uint32_t ram_start = (uint32_t)_image_ram_start;
  const uint32_t ram_end = (uint32_t)_image_ram_end;

  if ((uint32_t)start_addr >= ram_start && (uint32_t)start_addr < ram_end) {
    return MEMFAULT_MIN(desired_size, ram_end - (uint32_t)start_addr);
  }

  return 0;
}

size_t memfault_zephyr_get_task_regions(sMfltCoredumpRegion *regions, size_t num_regions) {
  if (regions == NULL || num_regions == 0) {
    return 0;
  }

  size_t region_idx = 0;

  // First we will try to store all the task TCBs. This way if we run out of space
  // while storing tasks we will still be able to recover the state of all the threads
  for (size_t i = 0; i < MEMFAULT_ARRAY_SIZE(s_task_tcbs) && region_idx < num_regions; i++) {
    struct k_thread *thread = s_task_tcbs[i];
    if (thread == EMPTY_SLOT) {
      continue;
    }

    const size_t tcb_size = memfault_platform_sanitize_address_range(
        thread, sizeof(*thread));
    if (tcb_size == 0) {
      // An invalid address, scrub the TCB from the list so we don't try to dereference
      // it when grabbing stacks below and move on.
      s_task_tcbs[i] = EMPTY_SLOT;
      continue;
    }

    regions[region_idx] = MEMFAULT_COREDUMP_MEMORY_REGION_INIT(thread, tcb_size);
    region_idx++;
  }

  // Now we store the region of the stack where context is saved. This way
  // we can unwind the stacks for threads that are not actively running
  for (size_t i = 0; i < MEMFAULT_ARRAY_SIZE(s_task_tcbs) && region_idx < num_regions; i++) {
    struct k_thread *thread = s_task_tcbs[i];
    if (thread == EMPTY_SLOT) {
      continue;
    }

    void *sp = (void*)thread->callee_saved.psp;
    if ((uintptr_t)_kernel.cpus[0].current == (uintptr_t)thread) {
      // thread context is only valid when task is _not_ running so we skip collecting it
      continue;
    }

#if defined(CONFIG_THREAD_STACK_INFO)
    // We know where the top of the stack is. Use that information to shrink
    // the area we need to collect if less than CONFIG_MEMFAULT_COREDUMP_STACK_SIZE_TO_COLLECT
    // is in use
    const uint32_t stack_top = thread->stack_info.start + thread->stack_info.size;
    size_t stack_size_to_collect =
        MEMFAULT_MIN(stack_top - (uint32_t)sp, CONFIG_MEMFAULT_COREDUMP_STACK_SIZE_TO_COLLECT);
#else
    size_t stack_size_to_collect = CONFIG_MEMFAULT_COREDUMP_STACK_SIZE_TO_COLLECT;
#endif

    stack_size_to_collect = memfault_platform_sanitize_address_range(sp, stack_size_to_collect);
    if (stack_size_to_collect == 0) {
      continue;
    }

    regions[region_idx] = MEMFAULT_COREDUMP_MEMORY_REGION_INIT(sp, stack_size_to_collect);
    region_idx++;
  }

  return region_idx;
}

size_t memfault_zephyr_get_data_regions(sMfltCoredumpRegion *regions, size_t num_regions) {
  if (num_regions == 0) {
    return 0;
  }

  // These linker variables are defined in linker.ld in Zephyr RTOS. Note that
  // the data region name changed in v2.7 of the kernel, so check the kernel
  // version and set the name appropriately.
  //
  // Also check for a user override; this is necessary if NCS v1.7.1 is used
  // with a Memfault SDK version >=0.27.3, because that NCS release used an
  // intermediate Zephyr release, so the version number checking is not
  // possible.
#if !MEMFAULT_ZEPHYR_USE_OLD_DATA_REGION_NAMES && MEMFAULT_ZEPHYR_VERSION_GT(2, 6)
#define ZEPHYR_DATA_REGION_START __data_region_start
#define ZEPHYR_DATA_REGION_END __data_region_end
#else
  // The old names are used in previous Zephyr versions (<=2.6)
#define ZEPHYR_DATA_REGION_START __data_ram_start
#define ZEPHYR_DATA_REGION_END __data_ram_end
#endif

  extern uint32_t ZEPHYR_DATA_REGION_START[];
  extern uint32_t ZEPHYR_DATA_REGION_END[];

  const size_t size_to_collect =
    (uint32_t)ZEPHYR_DATA_REGION_END - (uint32_t)ZEPHYR_DATA_REGION_START;
  regions[0] = MEMFAULT_COREDUMP_MEMORY_REGION_INIT(ZEPHYR_DATA_REGION_START, size_to_collect);
  return 1;
}

size_t memfault_zephyr_get_bss_regions(sMfltCoredumpRegion *regions, size_t num_regions) {
  if (num_regions == 0) {
    return 0;
  }

  // Linker variables defined in linker.ld in Zephyr RTOS
  extern uint32_t __bss_start[];
  extern uint32_t __bss_end[];

  const size_t size_to_collect = (uint32_t)__bss_end - (uint32_t)__bss_start;
  regions[0] = MEMFAULT_COREDUMP_MEMORY_REGION_INIT(__bss_start, size_to_collect);
  return 1;
}
