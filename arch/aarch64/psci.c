#include "psci.h"
#include "sysreg.h"

static enum psci_conduit conduit;

void psci_init(enum psci_conduit c)
{
    conduit = c;
}

int psci_available(void)
{
    return conduit != PSCI_NONE;
}

const char *psci_conduit_name(void)
{
    return conduit == PSCI_HVC ? "hvc" : conduit == PSCI_SMC ? "smc" : "none";
}

/* SMC Calling Convention: function ID in x0, args in x1-x3, result in x0. */
static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;

    if (conduit == PSCI_HVC)
        __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    else if (conduit == PSCI_SMC)
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    else
        return PSCI_E_NOT_SUPPORTED;
    return (int64_t)x0;
}

uint32_t psci_version(void)
{
    return (uint32_t)psci_call(PSCI_FN_VERSION, 0, 0, 0);
}

int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context)
{
    return psci_call(PSCI_FN_CPU_ON_64, mpidr, entry, context);
}

void psci_system_off(void)
{
    psci_call(PSCI_FN_SYSTEM_OFF, 0, 0, 0);
    for (;;)
        wfi();
}

void psci_system_reset(void)
{
    psci_call(PSCI_FN_SYSTEM_RESET, 0, 0, 0);
    for (;;)
        wfi();
}

const char *psci_strerror(int64_t err)
{
    switch (err) {
    case PSCI_SUCCESS:            return "success";
    case PSCI_E_NOT_SUPPORTED:    return "not supported";
    case PSCI_E_INVALID_PARAMS:   return "invalid parameters";
    case PSCI_E_DENIED:           return "denied";
    case PSCI_E_ALREADY_ON:       return "already on";
    case PSCI_E_ON_PENDING:       return "on pending";
    case PSCI_E_INTERNAL_FAILURE: return "internal failure";
    case PSCI_E_INVALID_ADDRESS:  return "invalid address";
    default:                      return "unknown error";
    }
}
