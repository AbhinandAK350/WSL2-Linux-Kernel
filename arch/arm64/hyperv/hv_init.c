// SPDX-License-Identifier: GPL-2.0

/*
 * Initialization of the interface with Microsoft's Hyper-V hypervisor,
 * and various low level utility routines for interacting with Hyper-V.
 *
 * Copyright (C) 2019, Microsoft, Inc.
 *
 * Author : Michael Kelley <mikelley@microsoft.com>
 */


#include <linux/types.h>
#include <linux/version.h>
#include <linux/export.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/hyperv.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/cpuhotplug.h>
#include <linux/psci.h>
#include <linux/sched_clock.h>
#include <asm-generic/bug.h>
#include <asm/hyperv-tlfs.h>
#include <asm/arch_timer.h>
#include <asm/mshyperv.h>
#include <asm/sysreg.h>
#include <clocksource/hyperv_timer.h>

static bool	hyperv_initialized;

struct		ms_hyperv_info ms_hyperv __ro_after_init;
EXPORT_SYMBOL_GPL(ms_hyperv);

u32		*hv_vp_index;
EXPORT_SYMBOL_GPL(hv_vp_index);

void __percpu **hyperv_pcpu_input_arg;
EXPORT_SYMBOL_GPL(hyperv_pcpu_input_arg);

u32		hv_max_vp_index;
EXPORT_SYMBOL_GPL(hv_max_vp_index);

static int hv_cpu_init(unsigned int cpu)
{
	void **input_arg;
	struct page *pg;
	u64 msr_vp_index;
	u32 cntkctl;

	input_arg = (void **)this_cpu_ptr(hyperv_pcpu_input_arg);
	pg = alloc_page(GFP_KERNEL);
	if (unlikely(!pg))
		return -ENOMEM;
	*input_arg = page_address(pg);

	hv_get_vp_index(msr_vp_index);

	hv_vp_index[smp_processor_id()] = msr_vp_index;

	if (msr_vp_index > hv_max_vp_index)
		hv_max_vp_index = msr_vp_index;

	/* Enable EL0 to access cntvct */
	cntkctl = arch_timer_get_cntkctl();
	cntkctl |= ARCH_TIMER_USR_VCT_ACCESS_EN;
	arch_timer_set_cntkctl(cntkctl);

	return 0;
}

static int hv_cpu_die(unsigned int cpu)
{
	void **input_arg;
	void *input_pg;
	unsigned long flags;

	local_irq_save(flags);
	input_arg = (void **)this_cpu_ptr(hyperv_pcpu_input_arg);
	input_pg = *input_arg;
	*input_arg = NULL;
	local_irq_restore(flags);
	free_page((unsigned long)input_pg);

	return 0;
}

/*
 * Functions for allocating and freeing memory with size and
 * alignment HV_HYP_PAGE_SIZE. These functions are needed because
 * the guest page size may not be the same as the Hyper-V page
 * size. And while kalloc() could allocate the memory, it does not
 * guarantee the required alignment. So a separate small memory
 * allocator is needed.  The free function is rarely used, so it
 * does not try to combine freed pages into larger chunks.
 *
 * These functions are used by arm64 specific code as well as
 * arch independent Hyper-V drivers.
 */

static DEFINE_SPINLOCK(free_list_lock);
static struct list_head free_list = LIST_HEAD_INIT(free_list);

void *hv_alloc_hyperv_page(void)
{
	int i;
	struct list_head *hv_page;
	unsigned long addr;

	BUILD_BUG_ON(HV_HYP_PAGE_SIZE > PAGE_SIZE);

	spin_lock(&free_list_lock);
	if (list_empty(&free_list)) {
		spin_unlock(&free_list_lock);
		addr = __get_free_page(GFP_KERNEL);
		spin_lock(&free_list_lock);
		for (i = 0; i < PAGE_SIZE; i += HV_HYP_PAGE_SIZE)
			list_add_tail((struct list_head *)(addr + i),
					&free_list);
	}
	hv_page = free_list.next;
	list_del(hv_page);
	spin_unlock(&free_list_lock);

	return hv_page;
}
EXPORT_SYMBOL_GPL(hv_alloc_hyperv_page);

void *hv_alloc_hyperv_zeroed_page(void)
{
	void *memp;

	memp = hv_alloc_hyperv_page();
	memset(memp, 0, HV_HYP_PAGE_SIZE);

	return memp;
}
EXPORT_SYMBOL_GPL(hv_alloc_hyperv_zeroed_page);


void hv_free_hyperv_page(unsigned long addr)
{
	if (!addr)
		return;
	spin_lock(&free_list_lock);
	list_add((struct list_head *)addr, &free_list);
	spin_unlock(&free_list_lock);
}
EXPORT_SYMBOL_GPL(hv_free_hyperv_page);


/*
 * This function is invoked via the ACPI clocksource probe mechanism. We
 * don't actually use any values from the ACPI GTDT table, but we set up
 * the Hyper-V synthetic clocksource and do other initialization for
 * interacting with Hyper-V the first time.  Using early_initcall to invoke
 * this function is too late because interrupts are already enabled at that
 * point, and hv_init_clocksource() must run before interrupts are enabled.
 *
 * 1. Setup the guest ID.
 * 2. Get features and hints info from Hyper-V
 * 3. Setup per-cpu VP indices.
 * 4. Initialize the Hyper-V clocksource.
 */

static int __init hyperv_init(struct acpi_table_header *table)
{
	struct hv_get_vp_register_output result;
	u32	a, b, c, d;
	u64	guest_id;
	int	i;

	/*
	 * If we're in a VM on Hyper-V, the ACPI hypervisor_id field will
	 * have the string "MsHyperV".
	 */
	if (strncmp((char *)&acpi_gbl_FADT.hypervisor_id, "MsHyperV", 8))
		return -EINVAL;

	/* Setup the guest ID */
	guest_id = generate_guest_id(0, LINUX_VERSION_CODE, 0);
	hv_set_vpreg(HV_REGISTER_GUEST_OSID, guest_id);

	/* Get the features and hints from Hyper-V */
	hv_get_vpreg_128(HV_REGISTER_PRIVILEGES_AND_FEATURES, &result);
	ms_hyperv.features = lower_32_bits(result.registervaluelow);
	ms_hyperv.priv_high = upper_32_bits(result.registervaluelow);
	ms_hyperv.misc_features = upper_32_bits(result.registervaluehigh);

	hv_get_vpreg_128(HV_REGISTER_FEATURES, &result);
	ms_hyperv.hints = lower_32_bits(result.registervaluelow);

	pr_info("Hyper-V: Features 0x%x, privilge high: 0x%x, hints 0x%x\n",
		ms_hyperv.features, ms_hyperv.priv_high, ms_hyperv.hints);

	/*
	 * Direct mode is the only option for STIMERs provided Hyper-V
	 * on ARM64, so Hyper-V doesn't actually set the flag.  But add
	 * the flag so the architecture independent code in
	 * drivers/clocksource/hyperv_timer.c will correctly use that mode.
	 */
	ms_hyperv.misc_features |= HV_STIMER_DIRECT_MODE_AVAILABLE;

	/*
	 * Hyper-V on ARM64 doesn't support AutoEOI.  Add the hint
	 * that tells architecture independent code not to use this
	 * feature.
	 */
	ms_hyperv.hints |= HV_DEPRECATING_AEOI_RECOMMENDED;

	/* Get information about the Hyper-V host version */
	hv_get_vpreg_128(HV_REGISTER_HYPERVISOR_VERSION, &result);
	a = lower_32_bits(result.registervaluelow);
	b = upper_32_bits(result.registervaluelow);
	c = lower_32_bits(result.registervaluehigh);
	d = upper_32_bits(result.registervaluehigh);
	pr_info("Hyper-V: Host Build %d.%d.%d.%d-%d-%d\n",
		b >> 16, b & 0xFFFF, a, d & 0xFFFFFF, c, d >> 24);

	/*
	 * Allocate the per-CPU state for the hypercall input arg.
	 * If this allocation fails, we will not be able to setup
	 * (per-CPU) hypercall input page and thus this failure is
	 * fatal on Hyper-V.
	 */
	hyperv_pcpu_input_arg = alloc_percpu(void  *);

	BUG_ON(hyperv_pcpu_input_arg == NULL);

	/* Allocate and initialize percpu VP index array */
	hv_vp_index = kmalloc_array(num_possible_cpus(), sizeof(*hv_vp_index),
				    GFP_KERNEL);
	if (!hv_vp_index)
		return -ENOMEM;

	for (i = 0; i < num_possible_cpus(); i++)
		hv_vp_index[i] = VP_INVAL;

	if (cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "arm64/hyperv_init:online",
			      hv_cpu_init, hv_cpu_die) < 0)
		goto free_vp_index;

	hv_init_clocksource();

	hyperv_initialized = true;
	return 0;

free_vp_index:
	kfree(hv_vp_index);
	hv_vp_index = NULL;
	return -EINVAL;
}
TIMER_ACPI_DECLARE(hyperv, ACPI_SIG_GTDT, hyperv_init);


/*
 * Called from hv_init_clocksource() to do ARM64
 * specific initialization of the sched clock
 */
void __init hv_setup_sched_clock(void *sched_clock)
{
	sched_clock_register(sched_clock, 64, HV_CLOCK_HZ);
}

/*
 * This routine is called before kexec/kdump, it does the required cleanup.
 */
void hyperv_cleanup(void)
{
	/* Reset our OS id */
	hv_set_vpreg(HV_REGISTER_GUEST_OSID, 0);

}
EXPORT_SYMBOL_GPL(hyperv_cleanup);


/*
 * hv_do_hypercall- Invoke the specified hypercall
 */
u64 hv_do_hypercall(u64 control, void *input, void *output)
{
	u64 input_address;
	u64 output_address;

	input_address = input ? virt_to_phys(input) : 0;
	output_address = output ? virt_to_phys(output) : 0;
	return hv_do_hvc(control, input_address, output_address);
}
EXPORT_SYMBOL_GPL(hv_do_hypercall);

/*
 * hv_do_fast_hypercall8 -- Invoke the specified hypercall
 * with arguments in registers instead of physical memory.
 * Avoids the overhead of virt_to_phys for simple hypercalls.
 */

u64 hv_do_fast_hypercall8(u16 code, u64 input)
{
	u64 control;

	control = (u64)code | HV_HYPERCALL_FAST_BIT;
	return hv_do_hvc(control, input);
}
EXPORT_SYMBOL_GPL(hv_do_fast_hypercall8);


/*
 * Set a single VP register to a 64-bit value.
 */
void hv_set_vpreg(u32 msr, u64 value)
{
	union hv_hypercall_status status;

	status.as_uint64 = hv_do_hvc(
		HVCALL_SET_VP_REGISTERS | HV_HYPERCALL_FAST_BIT |
			HV_HYPERCALL_REP_COUNT_1,
		HV_PARTITION_ID_SELF,
		HV_VP_INDEX_SELF,
		msr,
		0,
		value,
		0);

	/*
	 * Something is fundamentally broken in the hypervisor if
	 * setting a VP register fails. There's really no way to
	 * continue as a guest VM, so panic.
	 */
	BUG_ON(status.status != HV_STATUS_SUCCESS);
}
EXPORT_SYMBOL_GPL(hv_set_vpreg);


/*
 * Get the value of a single VP register, and only the low order 64 bits.
 */
u64 hv_get_vpreg(u32 msr)
{
	union hv_hypercall_status status;
	struct hv_get_vp_register_output output;

	status.as_uint64 = hv_do_hvc_fast_get(
		HVCALL_GET_VP_REGISTERS | HV_HYPERCALL_FAST_BIT |
			HV_HYPERCALL_REP_COUNT_1,
		HV_PARTITION_ID_SELF,
		HV_VP_INDEX_SELF,
		msr,
		&output);

	/*
	 * Something is fundamentally broken in the hypervisor if
	 * getting a VP register fails. There's really no way to
	 * continue as a guest VM, so panic.
	 */
	BUG_ON(status.status != HV_STATUS_SUCCESS);

	return output.registervaluelow;
}
EXPORT_SYMBOL_GPL(hv_get_vpreg);

/*
 * Get the value of a single VP register that is 128 bits in size.  This is a
 * separate call in order to avoid complicating the calling sequence for
 * the much more frequently used 64-bit version.
 */
void hv_get_vpreg_128(u32 msr, struct hv_get_vp_register_output *result)
{
	union hv_hypercall_status status;

	status.as_uint64 = hv_do_hvc_fast_get(
		HVCALL_GET_VP_REGISTERS | HV_HYPERCALL_FAST_BIT |
			HV_HYPERCALL_REP_COUNT_1,
		HV_PARTITION_ID_SELF,
		HV_VP_INDEX_SELF,
		msr,
		result);

	/*
	 * Something is fundamentally broken in the hypervisor if
	 * getting a VP register fails. There's really no way to
	 * continue as a guest VM, so panic.
	 */
	BUG_ON(status.status != HV_STATUS_SUCCESS);

	return;

}
EXPORT_SYMBOL_GPL(hv_get_vpreg_128);

void hyperv_report_panic(struct pt_regs *regs, long err, bool in_die)
{
	static bool panic_reported;
	u64 guest_id;

	if (in_die && !panic_on_oops)
		return;

	/*
	 * We prefer to report panic on 'die' chain as we have proper
	 * registers to report, but if we miss it (e.g. on BUG()) we need
	 * to report it on 'panic'.
	 */
	if (panic_reported)
		return;
	panic_reported = true;

	guest_id = hv_get_vpreg(HV_REGISTER_GUEST_OSID);

	/*
	 * Hyper-V provides the ability to store only 5 values.
	 * Pick the passed in error value, the guest_id, and the PC.
	 * The first two general registers are added arbitrarily.
	 */
	hv_set_vpreg(HV_REGISTER_CRASH_P0, err);
	hv_set_vpreg(HV_REGISTER_CRASH_P1, guest_id);
	hv_set_vpreg(HV_REGISTER_CRASH_P2, regs->pc);
	hv_set_vpreg(HV_REGISTER_CRASH_P3, regs->regs[0]);
	hv_set_vpreg(HV_REGISTER_CRASH_P4, regs->regs[1]);

	/*
	 * Let Hyper-V know there is crash data available
	 */
	hv_set_vpreg(HV_REGISTER_CRASH_CTL, HV_CRASH_CTL_CRASH_NOTIFY);
}
EXPORT_SYMBOL_GPL(hyperv_report_panic);

/*
 * hyperv_report_panic_msg - report panic message to Hyper-V
 * @pa: physical address of the panic page containing the message
 * @size: size of the message in the page
 */
void hyperv_report_panic_msg(phys_addr_t pa, size_t size)
{
	/*
	 * P3 to contain the physical address of the panic page & P4 to
	 * contain the size of the panic data in that page. Rest of the
	 * registers are no-op when the NOTIFY_MSG flag is set.
	 */
	hv_set_vpreg(HV_REGISTER_CRASH_P0, 0);
	hv_set_vpreg(HV_REGISTER_CRASH_P1, 0);
	hv_set_vpreg(HV_REGISTER_CRASH_P2, 0);
	hv_set_vpreg(HV_REGISTER_CRASH_P3, pa);
	hv_set_vpreg(HV_REGISTER_CRASH_P4, size);

	/*
	 * Let Hyper-V know there is crash data available along with
	 * the panic message.
	 */
	hv_set_vpreg(HV_REGISTER_CRASH_CTL,
	       (HV_CRASH_CTL_CRASH_NOTIFY | HV_CRASH_CTL_CRASH_NOTIFY_MSG));
}
EXPORT_SYMBOL_GPL(hyperv_report_panic_msg);

bool hv_is_hyperv_initialized(void)
{
	return hyperv_initialized;
}
EXPORT_SYMBOL_GPL(hv_is_hyperv_initialized);
