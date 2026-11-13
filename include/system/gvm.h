/*
 * Experimental shared-memory GiantVM configuration.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SYSTEM_GVM_H
#define SYSTEM_GVM_H

#include "hw/core/boards.h"

#ifdef CONFIG_GVM
static inline bool gvm_enabled(void)
{
    return current_machine && current_machine->gvm_enabled;
}

static inline bool gvm_cpu_is_local(int index)
{
    MachineState *ms = current_machine;

    return !gvm_enabled() ||
           (index >= ms->local_cpu_start_index &&
            index < ms->local_cpu_start_index + ms->local_cpus);
}
#else
static inline bool gvm_enabled(void)
{
    return false;
}

static inline bool gvm_cpu_is_local(int index)
{
    return true;
}
#endif

#endif /* SYSTEM_GVM_H */
