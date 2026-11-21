// SPDX-License-Identifier: GPL-2.0
/*
 * SBalance: Simple in-kernel IRQ balancer.
 * Adapted for Linux 4.14.
 */

#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kernel_stat.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "internals.h"

#define POLL_INTERVAL_JIFFIES msecs_to_jiffies(CONFIG_IRQ_SBALANCE_POLL_MSEC)

struct sbalance_irq_info {
	unsigned int prev_irqs[NR_CPUS];
	bool initialized;
};

static cpumask_var_t exclude_cpus;
static struct sbalance_irq_info *irq_info;
static unsigned int max_tracked_irqs;

static void sbalance_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(sbalance_work, sbalance_work_fn);

static bool irq_is_eligible(struct irq_desc *desc)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(data);

	if (!desc->action || irq_desc_is_chained(desc))
		return false;

	if (irq_settings_is_per_cpu(desc) || irq_settings_is_per_cpu_devid(desc))
		return false;

	if (irqd_irq_disabled(data) || !irqd_can_balance(data))
		return false;

	if (!chip || !chip->irq_set_affinity)
		return false;

	return true;
}

static void sbalance_balance_irq(unsigned int irq, struct irq_desc *desc,
				 unsigned int *cpu_load)
{
	struct sbalance_irq_info *info = &irq_info[irq];
	struct irq_data *data = irq_desc_get_irq_data(desc);
	unsigned int max_delta = 0;
	int busiest_cpu = -1;
	int best_cpu = -1;
	unsigned int min_load = UINT_MAX;
	unsigned int cpu;
	unsigned long flags;

	if (!info->initialized) {
		for_each_online_cpu(cpu)
			info->prev_irqs[cpu] = kstat_irqs_cpu(irq, cpu);
		info->initialized = true;
		return;
	}

	for_each_online_cpu(cpu) {
		unsigned int cur = kstat_irqs_cpu(irq, cpu);
		unsigned int delta = cur - info->prev_irqs[cpu];

		info->prev_irqs[cpu] = cur;
		cpu_load[cpu] += delta;

		if (delta > max_delta) {
			max_delta = delta;
			busiest_cpu = cpu;
		}
	}

	if (max_delta < CONFIG_IRQ_SBALANCE_THRESH || busiest_cpu < 0)
		return;

	for_each_online_cpu(cpu) {
		if (cpumask_test_cpu(cpu, exclude_cpus))
			continue;
		if (!cpumask_test_cpu(cpu, data->common->affinity))
			continue;
		if (cpu_load[cpu] < min_load) {
			min_load = cpu_load[cpu];
			best_cpu = cpu;
		}
	}

	/*
	 * If no CPU in current affinity is eligible, try any non-excluded
	 * online CPU.
	 */
	if (best_cpu < 0) {
		for_each_online_cpu(cpu) {
			if (cpumask_test_cpu(cpu, exclude_cpus))
				continue;
			if (cpu_load[cpu] < min_load) {
				min_load = cpu_load[cpu];
				best_cpu = cpu;
			}
		}
	}

	if (best_cpu < 0 || best_cpu == busiest_cpu)
		return;

	raw_spin_lock_irqsave(&desc->lock, flags);
	if (irq_is_eligible(desc))
		irq_set_affinity_locked(data, cpumask_of(best_cpu), false);
	raw_spin_unlock_irqrestore(&desc->lock, flags);
}

static void sbalance_work_fn(struct work_struct *work)
{
	unsigned int cpu_load[NR_CPUS] = { 0 };
	struct irq_desc *desc;
	unsigned int irq;

	for_each_irq_desc(irq, desc) {
		if (irq >= max_tracked_irqs)
			break;
		if (!irq_is_eligible(desc))
			continue;
		sbalance_balance_irq(irq, desc, cpu_load);
	}

	queue_delayed_work(system_power_efficient_wq, &sbalance_work,
			   POLL_INTERVAL_JIFFIES);
}

static int __init sbalance_init(void)
{
	if (!zalloc_cpumask_var(&exclude_cpus, GFP_KERNEL))
		return -ENOMEM;

	if (strlen(CONFIG_IRQ_SBALANCE_EXCLUDE_CPUS) > 0) {
		int ret = cpulist_parse(CONFIG_IRQ_SBALANCE_EXCLUDE_CPUS,
					exclude_cpus);
		if (ret)
			pr_warn("sbalance: failed to parse exclude_cpus (%d)\n",
				ret);
	}

	max_tracked_irqs = nr_irqs;
	irq_info = kcalloc(max_tracked_irqs, sizeof(*irq_info), GFP_KERNEL);
	if (!irq_info) {
		free_cpumask_var(exclude_cpus);
		return -ENOMEM;
	}

	queue_delayed_work(system_power_efficient_wq, &sbalance_work,
			   POLL_INTERVAL_JIFFIES);
	return 0;
}
late_initcall(sbalance_init);
