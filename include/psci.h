/*
 * Power State Coordination Interface (Arm DEN 0022) client.
 * Firmware (or QEMU's built-in implementation) handles these calls at a
 * higher exception level, reached through HVC or SMC.
 */
#ifndef PSCI_H
#define PSCI_H

#include <stdint.h>
#include "platform.h"

#define PSCI_FN_VERSION         0x84000000u
#define PSCI_FN_CPU_ON_64       0xc4000003u
#define PSCI_FN_AFFINITY_INFO64 0xc4000004u
#define PSCI_FN_SYSTEM_OFF      0x84000008u
#define PSCI_FN_SYSTEM_RESET    0x84000009u

#define PSCI_SUCCESS            0
#define PSCI_E_NOT_SUPPORTED    (-1)
#define PSCI_E_INVALID_PARAMS   (-2)
#define PSCI_E_DENIED           (-3)
#define PSCI_E_ALREADY_ON       (-4)
#define PSCI_E_ON_PENDING       (-5)
#define PSCI_E_INTERNAL_FAILURE (-6)
#define PSCI_E_INVALID_ADDRESS  (-9)

void        psci_init(enum psci_conduit conduit);
int         psci_available(void);
const char *psci_conduit_name(void);

uint32_t    psci_version(void);
int64_t     psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context);
void        psci_system_off(void) __attribute__((noreturn));
void        psci_system_reset(void) __attribute__((noreturn));
const char *psci_strerror(int64_t err);

#endif
