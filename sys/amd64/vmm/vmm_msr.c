/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>

#include <machine/specialreg.h>
#include <x86/apicreg.h>

#include <machine/vmm.h>
#include "vmm_lapic.h"
#include "vmm_msr.h"

#define	VMM_MSR_F_EMULATE	0x01
#define	VMM_MSR_F_READONLY	0x02

struct vmm_msr {
	int		num;
	int		flags;
	uint64_t	hostval;
};

static struct vmm_msr vmm_msr[] = {
	{ MSR_LSTAR,	0 },
	{ MSR_CSTAR,	0 },
	{ MSR_STAR,	0 },
	{ MSR_SF_MASK,	0 },
	{ MSR_APICBASE,	VMM_MSR_F_EMULATE },
	{ MSR_BIOS_SIGN,VMM_MSR_F_EMULATE },
	{ MSR_MCG_CAP,	VMM_MSR_F_EMULATE | VMM_MSR_F_READONLY },
};

#define	vmm_msr_num	(sizeof(vmm_msr) / sizeof(vmm_msr[0]))
CTASSERT(VMM_MSR_NUM >= vmm_msr_num);

#define	readonly_msr(idx)	\
	((vmm_msr[(idx)].flags & VMM_MSR_F_READONLY) != 0)

#define	emulated_msr(idx)	\
	((vmm_msr[(idx)].flags & VMM_MSR_F_EMULATE) != 0)

void
vmm_msr_init(void)
{
	int i;

	for (i = 0; i < vmm_msr_num; i++) {
		if (emulated_msr(i))
			continue;
		/*
		 * XXX this assumes that the value of the host msr does not
		 * change after we have cached it.
		 */
		vmm_msr[i].hostval = rdmsr(vmm_msr[i].num);
	}
}

void
guest_msrs_init(struct vm *vm, int cpu)
{
	int i;
	uint64_t *guest_msrs;

	guest_msrs = vm_guest_msrs(vm, cpu);
	
	for (i = 0; i < vmm_msr_num; i++) {
		switch (vmm_msr[i].num) {
		case MSR_LSTAR:
		case MSR_CSTAR:
		case MSR_STAR:
		case MSR_SF_MASK:
		case MSR_BIOS_SIGN:
		case MSR_MCG_CAP:
			guest_msrs[i] = 0;
			break;
		case MSR_APICBASE:
			guest_msrs[i] = DEFAULT_APIC_BASE | APICBASE_ENABLED |
					APICBASE_X2APIC;
			if (cpu == 0)
				guest_msrs[i] |= APICBASE_BSP;
			break;
		default:
			panic("guest_msrs_init: missing initialization for msr "
			      "0x%0x", vmm_msr[i].num);
		}
	}
}

static boolean_t
x2apic_msr(u_int num)
{

	if (num >= 0x800 && num <= 0xBFF)
		return (TRUE);
	else
		return (FALSE);
}

static u_int
x2apic_msr_to_regoff(u_int msr)
{

	return ((msr - 0x800) << 4);
}

static boolean_t
x2apic_msr_id(u_int num)
{
	return (num == 0x802);
}

static int
msr_num_to_idx(u_int num)
{
	int i;

	for (i = 0; i < vmm_msr_num; i++)
		if (vmm_msr[i].num == num)
			return (i);

	return (-1);
}

int
emulate_wrmsr(struct vm *vm, int cpu, u_int num, uint64_t val)
{
	int handled, idx;
	uint64_t *guest_msrs;

	handled = 0;

	if (x2apic_msr(num))
		return (lapic_write(vm, cpu, x2apic_msr_to_regoff(num), val));

	idx = msr_num_to_idx(num);
	if (idx < 0)
		goto done;

	if (!readonly_msr(idx)) {
		guest_msrs = vm_guest_msrs(vm, cpu);

		/* Stash the value */
		guest_msrs[idx] = val;

		/* Update processor state for non-emulated MSRs */
		if (!emulated_msr(idx))
			wrmsr(vmm_msr[idx].num, val);
	}

	handled = 1;
done:
	return (handled);
}

int
emulate_rdmsr(struct vm *vm, int cpu, u_int num)
{
	int error, handled, idx;
	uint32_t eax, edx;
	uint64_t result, *guest_msrs;

	handled = 0;

	if (x2apic_msr(num)) {
		handled = lapic_read(vm, cpu, x2apic_msr_to_regoff(num),
				     &result);
		/*
		 * The version ID needs to be massaged
		 */
		if (x2apic_msr_id(num)) {
			result = result >> 24;
		}
		goto done;
	}

	idx = msr_num_to_idx(num);
	if (idx < 0)
		goto done;

	guest_msrs = vm_guest_msrs(vm, cpu);
	result = guest_msrs[idx];

	/*
	 * If this is not an emulated msr register make sure that the processor
	 * state matches our cached state.
	 */
	if (!emulated_msr(idx) && (rdmsr(num) != result)) {
		panic("emulate_rdmsr: msr 0x%0x has inconsistent cached "
		      "(0x%016lx) and actual (0x%016lx) values", num,
		      result, rdmsr(num));
	}

	handled = 1;

done:
	if (handled) {
		eax = result;
		edx = result >> 32;
		error = vm_set_register(vm, cpu, VM_REG_GUEST_RAX, eax);
		if (error)
			panic("vm_set_register(rax) error %d", error);
		error = vm_set_register(vm, cpu, VM_REG_GUEST_RDX, edx);
		if (error)
			panic("vm_set_register(rdx) error %d", error);
	}
	return (handled);
}

void
restore_guest_msrs(struct vm *vm, int cpu)
{
	int i;
	uint64_t *guest_msrs;

	guest_msrs = vm_guest_msrs(vm, cpu);

	for (i = 0; i < vmm_msr_num; i++) {
		if (emulated_msr(i))
			continue;
		else
			wrmsr(vmm_msr[i].num, guest_msrs[i]);
	}
}

void
restore_host_msrs(struct vm *vm, int cpu)
{
	int i;

	for (i = 0; i < vmm_msr_num; i++) {
		if (emulated_msr(i))
			continue;
		else
			wrmsr(vmm_msr[i].num, vmm_msr[i].hostval);
	}
}