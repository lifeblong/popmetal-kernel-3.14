/*
 * Copyright (C) 2012 Freescale Semiconductor, Inc.
 * Copyright (c) 2014, The Linux Foundation. All rights reserved.
 *
 * The OPP code in function krait_set_target() is reused from
 * drivers/cpufreq/omap-cpufreq.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

static unsigned int transition_latency;

static struct device *cpu_dev;
static DEFINE_PER_CPU(struct clk *, krait_cpu_clks);
static struct cpufreq_frequency_table *freq_table;
static struct thermal_cooling_device *cdev;

static int krait_set_target(struct cpufreq_policy *policy, unsigned int index)
{
	unsigned long volt = 0, volt_old = 0;
	unsigned int old_freq, new_freq;
	long freq_Hz, freq_exact;
	int ret;
	struct clk *cpu_clk;

	cpu_clk = per_cpu(krait_cpu_clks, policy->cpu);

	freq_Hz = clk_round_rate(cpu_clk, freq_table[index].frequency * 1000);
	if (freq_Hz <= 0)
		freq_Hz = freq_table[index].frequency * 1000;

	freq_exact = freq_Hz;
	new_freq = freq_Hz / 1000;
	old_freq = clk_get_rate(cpu_clk) / 1000;

	pr_debug("%u MHz, %ld mV --> %u MHz, %ld mV\n",
		 old_freq / 1000, volt_old ? volt_old / 1000 : -1,
		 new_freq / 1000, volt ? volt / 1000 : -1);

	ret = clk_set_rate(cpu_clk, freq_exact);
	if (ret)
		pr_err("failed to set clock rate: %d\n", ret);

	return ret;
}

static int krait_cpufreq_init(struct cpufreq_policy *policy)
{
	int ret;

	policy->clk = per_cpu(krait_cpu_clks, policy->cpu);

	ret = cpufreq_table_validate_and_show(policy, freq_table);
	if (ret) {
		pr_err("%s: invalid frequency table: %d\n", __func__, ret);
		return ret;
	}

	policy->cpuinfo.transition_latency = transition_latency;

	return 0;
}

static struct cpufreq_driver krait_cpufreq_driver = {
	.flags = CPUFREQ_STICKY,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = krait_set_target,
	.get = cpufreq_generic_get,
	.init = krait_cpufreq_init,
	.name = "generic_krait",
	.attr = cpufreq_generic_attr,
};

static int krait_cpufreq_probe(struct platform_device *pdev)
{
	struct device_node *np;
	int ret;
	unsigned int cpu;
	struct device *dev;
	struct clk *clk;

	cpu_dev = get_cpu_device(0);
	if (!cpu_dev) {
		pr_err("failed to get krait device\n");
		return -ENODEV;
	}

	np = of_node_get(cpu_dev->of_node);
	if (!np) {
		pr_err("failed to find krait node\n");
		return -ENOENT;
	}

	for_each_possible_cpu(cpu) {
		dev = get_cpu_device(cpu);
		if (!dev) {
			pr_err("failed to get krait device\n");
			ret = -ENOENT;
			goto out_put_node;
		}
		per_cpu(krait_cpu_clks, cpu) = clk = devm_clk_get(dev, NULL);
		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			goto out_put_node;
		}
	}

	ret = of_init_opp_table(cpu_dev);
	if (ret) {
		pr_err("failed to init OPP table: %d\n", ret);
		goto out_put_node;
	}

	ret = dev_pm_opp_init_cpufreq_table(cpu_dev, &freq_table);
	if (ret) {
		pr_err("failed to init cpufreq table: %d\n", ret);
		goto out_put_node;
	}

	if (of_property_read_u32(np, "clock-latency", &transition_latency))
		transition_latency = CPUFREQ_ETERNAL;

	ret = cpufreq_register_driver(&krait_cpufreq_driver);
	if (ret) {
		pr_err("failed register driver: %d\n", ret);
		goto out_free_table;
	}
	of_node_put(np);

	/*
	 * For now, just loading the cooling device;
	 * thermal DT code takes care of matching them.
	 */
	for_each_possible_cpu(cpu) {
		dev = get_cpu_device(cpu);
		np = of_node_get(dev->of_node);
		if (of_find_property(np, "#cooling-cells", NULL)) {
			cdev = of_cpufreq_cooling_register(np, cpumask_of(cpu));
			if (IS_ERR(cdev))
				pr_err("running cpufreq without cooling device: %ld\n",
				       PTR_ERR(cdev));
		}
		of_node_put(np);
	}

	return 0;

out_free_table:
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table);
out_put_node:
	of_node_put(np);
	return ret;
}

static int krait_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_cooling_unregister(cdev);
	cpufreq_unregister_driver(&krait_cpufreq_driver);
	dev_pm_opp_free_cpufreq_table(cpu_dev, &freq_table);

	return 0;
}

static struct platform_driver krait_cpufreq_platdrv = {
	.driver = {
		.name	= "cpufreq-krait",
		.owner	= THIS_MODULE,
	},
	.probe		= krait_cpufreq_probe,
	.remove		= krait_cpufreq_remove,
};
module_platform_driver(krait_cpufreq_platdrv);

MODULE_DESCRIPTION("Krait CPUfreq driver");
MODULE_LICENSE("GPL v2");
