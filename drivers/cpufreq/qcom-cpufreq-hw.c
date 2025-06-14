// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/energy_model.h>
#include <linux/sched.h>
#include <linux/cpu_cooling.h>

#include <linux/sec_debug.h>
#include <linux/sec_smem.h>
#include <trace/events/power.h>

#define LUT_MAX_ENTRIES			40U
#define CORE_COUNT_VAL(val)		(((val) & (GENMASK(18, 16))) >> 16)
#define LUT_ROW_SIZE			32
#define CLK_HW_DIV			2
#define GT_IRQ_STATUS			BIT(2)
#define MAX_FN_SIZE			20
#define LIMITS_POLLING_DELAY_MS		1

#define CYCLE_CNTR_OFFSET(c, m, acc_count)				\
			(acc_count ? ((c - cpumask_first(m) + 1) * 4) : 0)


enum {
	CPUFREQ_HW_LOW_TEMP_LEVEL,
	CPUFREQ_HW_HIGH_TEMP_LEVEL,
};

enum {
	REG_ENABLE,
	REG_FREQ_LUT_TABLE,
	REG_VOLT_LUT_TABLE,
	REG_PERF_STATE,
	REG_CYCLE_CNTR,
	REG_DOMAIN_STATE,
	REG_INTR_EN,
	REG_INTR_CLR,
	REG_INTR_STATUS,
	REG_ARRAY_SIZE,
};

static unsigned int lut_row_size = LUT_ROW_SIZE;
static unsigned int lut_max_entries = LUT_MAX_ENTRIES;
static bool accumulative_counter;

struct skipped_freq {
	bool skip;
	u32 freq;
	u32 cc;
	u32 prev_index;
	u32 prev_freq;
	u32 prev_cc;
	u32 high_temp_index;
	u32 low_temp_index;
	u32 final_index;
	spinlock_t lock;
};

struct cpufreq_qcom {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	cpumask_t related_cpus;
	unsigned int max_cores;
	unsigned int lut_max_entries;
	unsigned long xo_rate;
	unsigned long cpu_hw_rate;
	struct delayed_work freq_poll_work;
	struct device_attribute freq_limit_attr;
	struct skipped_freq skip_data;
	bool is_irq_enabled;
	bool is_irq_requested;
};

struct cpufreq_counter {
	u64 total_cycle_counter;
	u32 prev_cycle_counter;
	spinlock_t lock;
};

struct cpufreq_cooling_cdev {
	int cpu_id;
	bool cpu_cooling_state;
	struct thermal_cooling_device *cdev;
	struct device_node *np;
};

static const u16 cpufreq_qcom_std_offsets[REG_ARRAY_SIZE] = {
	[REG_ENABLE]		= 0x0,
	[REG_FREQ_LUT_TABLE]	= 0x110,
	[REG_VOLT_LUT_TABLE]	= 0x114,
	[REG_PERF_STATE]	= 0x920,
	[REG_CYCLE_CNTR]	= 0x9c0,
};

static const u16 cpufreq_qcom_epss_std_offsets[REG_ARRAY_SIZE] = {
	[REG_ENABLE]		= 0x0,
	[REG_FREQ_LUT_TABLE]	= 0x100,
	[REG_VOLT_LUT_TABLE]	= 0x200,
	[REG_PERF_STATE]	= 0x320,
	[REG_CYCLE_CNTR]	= 0x3c4,
	[REG_DOMAIN_STATE]	= 0x020,
	[REG_INTR_EN]		= 0x304,
	[REG_INTR_CLR]		= 0x308,
	[REG_INTR_STATUS]	= 0x30C,
};

static struct cpufreq_counter qcom_cpufreq_counter[NR_CPUS];
static struct cpufreq_qcom *qcom_freq_domain_map[NR_CPUS];

static unsigned int qcom_cpufreq_hw_get(unsigned int cpu);

static u64 qcom_cpufreq_get_cpu_cycle_counter(int cpu)
{
	struct cpufreq_counter *cpu_counter;
	struct cpufreq_qcom *cpu_domain;
	u64 cycle_counter_ret;
	unsigned long flags;
	u16 offset;
	u32 val;

	cpu_domain = qcom_freq_domain_map[cpu];
	cpu_counter = &qcom_cpufreq_counter[cpu];
	spin_lock_irqsave(&cpu_counter->lock, flags);

	offset = CYCLE_CNTR_OFFSET(cpu, &cpu_domain->related_cpus,
					accumulative_counter);
	val = readl_relaxed_no_log(cpu_domain->reg_bases[REG_CYCLE_CNTR] +
				   offset);

	if (val < cpu_counter->prev_cycle_counter) {
		/* Handle counter overflow */
		cpu_counter->total_cycle_counter += UINT_MAX -
			cpu_counter->prev_cycle_counter + val;
		cpu_counter->prev_cycle_counter = val;
	} else {
		cpu_counter->total_cycle_counter += val -
			cpu_counter->prev_cycle_counter;
		cpu_counter->prev_cycle_counter = val;
	}
	cycle_counter_ret = cpu_counter->total_cycle_counter;
	spin_unlock_irqrestore(&cpu_counter->lock, flags);

	return cycle_counter_ret;
}

static int
qcom_cpufreq_hw_target_index(struct cpufreq_policy *policy,
			     unsigned int index)
{
	struct cpufreq_qcom *c = policy->driver_data;
	unsigned long flags;
	unsigned int target_index = index;

	if (c->skip_data.skip && index == c->skip_data.high_temp_index) {
		spin_lock_irqsave(&c->skip_data.lock, flags);
		writel_relaxed(c->skip_data.final_index,
				c->reg_bases[REG_PERF_STATE]);
		target_index = c->skip_data.final_index;
		spin_unlock_irqrestore(&c->skip_data.lock, flags);
	} else {
		writel_relaxed(index, c->reg_bases[REG_PERF_STATE]);
	}

	sec_smem_clk_osm_add_log_cpufreq(policy->cpu,
				policy->freq_table[target_index].frequency, policy->kobj.name);

	sec_smem_clk_osm_add_log_cpufreq(policy->cpu,
				policy->freq_table[index].frequency, policy->kobj.name);

	arch_set_freq_scale(policy->related_cpus,
			    policy->freq_table[index].frequency,
			    policy->cpuinfo.max_freq);

	return 0;
}

static unsigned int qcom_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_qcom *c;
	struct cpufreq_policy *policy;
	unsigned int index;

	policy = cpufreq_cpu_get_raw(cpu);
	if (!policy)
		return 0;

	c = policy->driver_data;

	index = readl_relaxed(c->reg_bases[REG_PERF_STATE]);
	index = min(index, c->lut_max_entries - 1);

	return policy->freq_table[index].frequency;
}

static unsigned int
qcom_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
			    unsigned int target_freq)
{
	int index;

	index = policy->cached_resolved_idx;
	if (index < 0)
		return 0;

	if (qcom_cpufreq_hw_target_index(policy, index))
		return 0;

	return policy->freq_table[index].frequency;
}

static int qcom_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct em_data_callback em_cb = EM_DATA_CB(of_dev_pm_opp_get_cpu_power);
	struct cpufreq_qcom *c;
	struct device *cpu_dev;
	int ret;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__,
				policy->cpu);
		return -ENODEV;
	}

	c = qcom_freq_domain_map[policy->cpu];
	if (!c) {
		pr_err("No scaling support for CPU%d\n", policy->cpu);
		return -ENODEV;
	}

	cpumask_copy(policy->cpus, &c->related_cpus);

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0)
		dev_err(cpu_dev, "OPP table is not ready\n");

	policy->fast_switch_possible = true;
	policy->freq_table = c->table;
	policy->driver_data = c;
	policy->dvfs_possible_from_any_cpu = true;

	em_register_perf_domain(policy->cpus, ret, &em_cb);

	return 0;
}

static struct freq_attr *qcom_cpufreq_hw_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&cpufreq_freq_attr_scaling_boost_freqs,
	NULL
};

static void qcom_cpufreq_ready(struct cpufreq_policy *policy)
{
	static struct thermal_cooling_device *cdev[NR_CPUS];
	struct device_node *np;
	unsigned int cpu = policy->cpu;

	if (cdev[cpu])
		return;

	np = of_cpu_device_node_get(cpu);
	if (WARN_ON(!np))
		return;

	/*
	 * For now, just loading the cooling device;
	 * thermal DT code takes care of matching them.
	 */
	if (of_find_property(np, "#cooling-cells", NULL)) {
		cdev[cpu] = of_cpufreq_cooling_register(policy);
		if (IS_ERR(cdev[cpu])) {
			pr_err("running cpufreq for CPU%d without cooling dev: %ld\n",
			       cpu, PTR_ERR(cdev[cpu]));
			cdev[cpu] = NULL;
		}
	}

	of_node_put(np);
}

static struct cpufreq_driver cpufreq_qcom_hw_driver = {
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= qcom_cpufreq_hw_target_index,
	.get		= qcom_cpufreq_hw_get,
	.init		= qcom_cpufreq_hw_cpu_init,
	.fast_switch    = qcom_cpufreq_hw_fast_switch,
	.name		= "qcom-cpufreq-hw",
	.attr		= qcom_cpufreq_hw_attr,
	.boost_enabled	= true,
	.ready		= qcom_cpufreq_ready,
};

static bool of_find_freq(u32 *of_table, int of_len, long frequency)
{
	int i;
	if (!of_table)
		return true;
	for (i = 0; i < of_len; i++) {
		if (frequency == of_table[i])
			return true;
	}
	return false;
}

static int qcom_cpufreq_hw_read_lut(struct platform_device *pdev,
				    struct cpufreq_qcom *c,
				    int domain_index)
				    
{
	struct device *dev = &pdev->dev, *cpu_dev;
	void __iomem *base_freq, *base_volt;
	u32 data, src, lval, i, core_count, prev_cc, prev_freq, cur_freq, volt;
	u32 vc;
	unsigned long cpu;
	int ret, of_len;
	u32 *of_table = NULL;
	char tbl_name[] = "qcom,cpufreq-table-##";

	c->table = devm_kcalloc(dev, lut_max_entries + 1,
				sizeof(*c->table), GFP_KERNEL);
	if (!c->table)
		return -ENOMEM;

	snprintf(tbl_name, sizeof(tbl_name), "qcom,cpufreq-table-%d",
		 domain_index);
	if (of_find_property(dev->of_node, tbl_name, &of_len) && of_len > 0) {
		of_len /= sizeof(*of_table);
		of_table = devm_kcalloc(dev, of_len, sizeof(*of_table),
					GFP_KERNEL);
		if (!of_table) {
			ret = -ENOMEM;
			goto err_cpufreq_table;
		}
		ret = of_property_read_u32_array(dev->of_node, tbl_name,
						 of_table, of_len);
		if (ret)
			goto err_of_table;
	}

	spin_lock_init(&c->skip_data.lock);
	base_freq = c->reg_bases[REG_FREQ_LUT_TABLE];
	base_volt = c->reg_bases[REG_VOLT_LUT_TABLE];

	prev_cc = 0;

	for (i = 0; i < lut_max_entries; i++) {
		data = readl_relaxed(base_freq + i * lut_row_size);
		src = (data & GENMASK(31, 30)) >> 30;
		lval = data & GENMASK(7, 0);
		core_count = CORE_COUNT_VAL(data);

		data = readl_relaxed(base_volt + i * lut_row_size);
		volt = (data & GENMASK(11, 0)) * 1000;
		vc = data & GENMASK(21, 16);

		if (src)
			c->table[i].frequency = c->xo_rate * lval / 1000;
		else
			c->table[i].frequency = c->cpu_hw_rate / 1000;

		cur_freq = c->table[i].frequency;

		dev_info(dev, "cpu=%lu, index=%d, freq=%d, volt=%d\n",
		    	cpu, i, c->table[i].frequency, volt);

				if (!of_find_freq(of_table, of_len, c->table[i].frequency)) {
			c->table[i].frequency = CPUFREQ_ENTRY_INVALID;
			cur_freq = CPUFREQ_ENTRY_INVALID;
		} else {
			if (core_count != c->max_cores) {
				if (core_count == (c->max_cores - 1)) {
					c->skip_data.skip = true;
					c->skip_data.high_temp_index = i;
					c->skip_data.freq = cur_freq;
					c->skip_data.cc = core_count;
					c->skip_data.final_index = i + 1;
					c->skip_data.low_temp_index = i + 1;
					c->skip_data.prev_freq =
							c->table[i-1].frequency;
					c->skip_data.prev_index = i - 1;
					c->skip_data.prev_cc = prev_cc;
				} else {
					cur_freq = CPUFREQ_ENTRY_INVALID;
					c->table[i].flags = CPUFREQ_BOOST_FREQ;
				}
			}

			/*
			 * Two of the same frequencies with the same core counts means
			 * end of table.
			 */
			if (i > 0 && c->table[i - 1].frequency ==
					c->table[i].frequency) {
				if (prev_cc == core_count) {
					struct cpufreq_frequency_table *prev =
								&c->table[i - 1];
					if (prev_freq == CPUFREQ_ENTRY_INVALID)
						prev->flags = CPUFREQ_BOOST_FREQ;
				}
				break;
			}
		}

		prev_cc = core_count;
		prev_freq = cur_freq;

		for_each_cpu(cpu, &c->related_cpus) {
			cpu_dev = get_cpu_device(cpu);
			if (!cpu_dev)
				continue;
			dev_pm_opp_add(cpu_dev, c->table[i].frequency * 1000,
							volt);

		}
	}

	c->lut_max_entries = i;
	c->table[i].frequency = CPUFREQ_TABLE_END;

	if (of_table)
			devm_kfree(dev, of_table);
	if (c->skip_data.skip) {
		pr_err("%s Skip: Index[%u], Frequency[%u], Core Count %u, Final Index %u Actual Index %u Prev_Freq[%u] Prev_Index[%u] Prev_CC[%u]\n",
				__func__, c->skip_data.high_temp_index,
				c->skip_data.freq, c->skip_data.cc,
				c->skip_data.final_index,
				c->skip_data.low_temp_index,
				c->skip_data.prev_freq,
				c->skip_data.prev_index,
				c->skip_data.prev_cc);
	}

	return 0;
	
err_of_table:
	devm_kfree(dev, of_table);
err_cpufreq_table:
	devm_kfree(dev, c->table);
	return ret;
}

static int qcom_get_related_cpus(int index, struct cpumask *m)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np)
			continue;

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
				"#freq-domain-cells", 0, &args);
		of_node_put(cpu_np);
		if (ret < 0)
			continue;

		if (index == args.args[0])
			cpumask_set_cpu(cpu, m);
	}

	return 0;
}

static int qcom_cpu_resources_init(struct platform_device *pdev,
				   unsigned int cpu, int index,
				   unsigned int max_cores,
				   unsigned long xo_rate,
				   unsigned long cpu_hw_rate)
{
	struct cpufreq_qcom *c;
	struct resource *res;
	struct device *dev = &pdev->dev;
	const u16 *offsets;
	int ret, i, cpu_r;
	void __iomem *base;

	if (qcom_freq_domain_map[cpu])
		return 0;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	offsets = of_device_get_match_data(&pdev->dev);
	if (!offsets)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = REG_ENABLE; i < REG_ARRAY_SIZE; i++)
		c->reg_bases[i] = base + offsets[i];

	if (!of_property_read_bool(dev->of_node, "qcom,skip-enable-check")) {
		/* HW should be in enabled state to proceed */
		if (!(readl_relaxed(c->reg_bases[REG_ENABLE]) & 0x1)) {
			dev_err(dev, "Domain-%d cpufreq hardware not enabled\n",
				 index);
			return -ENODEV;
		}
	}

	accumulative_counter = !of_property_read_bool(dev->of_node,
					"qcom,no-accumulative-counter");

	ret = qcom_get_related_cpus(index, &c->related_cpus);
	if (ret) {
		dev_err(dev, "Domain-%d failed to get related CPUs\n", index);
		return ret;
	}

	c->max_cores = max_cores;
	if (!c->max_cores)
		return -ENOENT;

	c->xo_rate = xo_rate;
	c->cpu_hw_rate = cpu_hw_rate;

	ret = qcom_cpufreq_hw_read_lut(pdev, c, index);
	if (ret) {
		dev_err(dev, "Domain-%d failed to read LUT\n", index);
		return ret;
	}


	for_each_cpu(cpu_r, &c->related_cpus)
		qcom_freq_domain_map[cpu_r] = c;

	return 0;
}

static int qcom_resources_init(struct platform_device *pdev)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	struct clk *clk;
	unsigned int cpu;
	unsigned long xo_rate, cpu_hw_rate;
	int ret;

	clk = devm_clk_get(&pdev->dev, "xo");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	xo_rate = clk_get_rate(clk);

	devm_clk_put(&pdev->dev, clk);

	clk = devm_clk_get(&pdev->dev, "alternate");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	cpu_hw_rate = clk_get_rate(clk) / CLK_HW_DIV;

	devm_clk_put(&pdev->dev, clk);

	of_property_read_u32(pdev->dev.of_node, "qcom,lut-row-size",
			      &lut_row_size);

	of_property_read_u32(pdev->dev.of_node, "qcom,lut-max-entries",
			      &lut_max_entries);

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_dbg(&pdev->dev, "Failed to get cpu %d device\n",
				cpu);
			continue;
		}

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
				"#freq-domain-cells", 0, &args);
		if (ret < 0)
			return ret;

		ret = qcom_cpu_resources_init(pdev, cpu, args.args[0],
					      args.args[1], xo_rate,
					      cpu_hw_rate);
		if (ret)
			return ret;
	}

	return 0;
}

static int cpufreq_hw_set_cur_state(struct thermal_cooling_device *cdev,
					unsigned long state)
{
	struct cpufreq_cooling_cdev *cpu_cdev = cdev->devdata;
	struct cpufreq_policy *policy;
	struct cpufreq_qcom *c;
	unsigned long flags;


	if (cpu_cdev->cpu_id == -1)
		return -ENODEV;

	if (state > CPUFREQ_HW_HIGH_TEMP_LEVEL)
		return -EINVAL;

	if (cpu_cdev->cpu_cooling_state == state)
		return 0;

	policy = cpufreq_cpu_get_raw(cpu_cdev->cpu_id);
	if (!policy)
		return 0;

	c = policy->driver_data;
	cpu_cdev->cpu_cooling_state = state;

	if (state == CPUFREQ_HW_HIGH_TEMP_LEVEL) {
		spin_lock_irqsave(&c->skip_data.lock, flags);
		c->skip_data.final_index = c->skip_data.high_temp_index;
		spin_unlock_irqrestore(&c->skip_data.lock, flags);
	} else {
		spin_lock_irqsave(&c->skip_data.lock, flags);
		c->skip_data.final_index = c->skip_data.low_temp_index;
		spin_unlock_irqrestore(&c->skip_data.lock, flags);
	}

	if (policy->cur != c->skip_data.freq)
		return 0;

	return qcom_cpufreq_hw_target_index(policy,
					c->skip_data.high_temp_index);
}

static int cpufreq_hw_get_cur_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	struct cpufreq_cooling_cdev *cpu_cdev = cdev->devdata;

	*state = (cpu_cdev->cpu_cooling_state) ?
			CPUFREQ_HW_HIGH_TEMP_LEVEL : CPUFREQ_HW_LOW_TEMP_LEVEL;

	return 0;
}

static int cpufreq_hw_get_max_state(struct thermal_cooling_device *cdev,
					unsigned long *state)
{
	*state = CPUFREQ_HW_HIGH_TEMP_LEVEL;
	return 0;
}

static struct thermal_cooling_device_ops cpufreq_hw_cooling_ops = {
	.get_max_state = cpufreq_hw_get_max_state,
	.get_cur_state = cpufreq_hw_get_cur_state,
	.set_cur_state = cpufreq_hw_set_cur_state,
};

static int cpufreq_hw_register_cooling_device(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node, *cpu_np, *phandle;
	struct cpufreq_cooling_cdev *cpu_cdev = NULL;
	struct device *cpu_dev;
	struct cpufreq_policy *policy;
	struct cpufreq_qcom *c;
	char cdev_name[MAX_FN_SIZE] = "";
	int cpu;

	for_each_available_child_of_node(np, cpu_np) {
		cpu_cdev = devm_kzalloc(&pdev->dev, sizeof(*cpu_cdev),
				GFP_KERNEL);
		if (!cpu_cdev)
			return -ENOMEM;
		cpu_cdev->cpu_id = -1;
		cpu_cdev->cpu_cooling_state = false;
		cpu_cdev->cdev = NULL;
		cpu_cdev->np = cpu_np;

		phandle = of_parse_phandle(cpu_np, "qcom,cooling-cpu", 0);
		for_each_possible_cpu(cpu) {
			policy = cpufreq_cpu_get_raw(cpu);
			if (!policy)
				continue;
			c = policy->driver_data;
			if (!c->skip_data.skip)
				continue;
			cpu_dev = get_cpu_device(cpu);
			if (cpu_dev && cpu_dev->of_node == phandle) {
				cpu_cdev->cpu_id = cpu;
				snprintf(cdev_name, sizeof(cdev_name),
						"cpufreq-hw-%d", cpu);
				cpu_cdev->cdev =
					thermal_of_cooling_device_register(
						cpu_cdev->np, cdev_name,
						cpu_cdev,
						&cpufreq_hw_cooling_ops);
				if (IS_ERR(cpu_cdev->cdev)) {
					pr_err("Cooling register failed for %s, ret: %ld\n",
						cdev_name,
						PTR_ERR(cpu_cdev->cdev));
					c->skip_data.final_index =
						c->skip_data.high_temp_index;
					break;
				}
				pr_info("CPUFREQ-HW cooling device %d %s\n",
						cpu, cdev_name);
				break;
			}
		}
	}

	return 0;
}

static int qcom_cpufreq_hw_driver_probe(struct platform_device *pdev)
{
	struct cpu_cycle_counter_cb cycle_counter_cb = {
		.get_cpu_cycle_counter = qcom_cpufreq_get_cpu_cycle_counter,
	};
	int rc, cpu;

	/* Get the bases of cpufreq for domains */
	rc = qcom_resources_init(pdev);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq resource init failed\n");
		return rc;
	}

	rc = cpufreq_register_driver(&cpufreq_qcom_hw_driver);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq HW driver failed to register\n");
		return rc;
	}

	for_each_possible_cpu(cpu)
		spin_lock_init(&qcom_cpufreq_counter[cpu].lock);

	rc = register_cpu_cycle_counter_cb(&cycle_counter_cb);
	if (rc) {
		dev_err(&pdev->dev, "cycle counter cb failed to register\n");
		return rc;
	}

	dev_dbg(&pdev->dev, "QCOM CPUFreq HW driver initialized\n");
	of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);

	cpufreq_hw_register_cooling_device(pdev);

	return 0;
}

static const struct of_device_id qcom_cpufreq_hw_match[] = {
	{ .compatible = "qcom,cpufreq-hw", .data = &cpufreq_qcom_std_offsets },
	{ .compatible = "qcom,cpufreq-hw-epss",
				   .data = &cpufreq_qcom_epss_std_offsets },
	{}
};

static struct platform_driver qcom_cpufreq_hw_driver = {
	.probe = qcom_cpufreq_hw_driver_probe,
	.driver = {
		.name = "qcom-cpufreq-hw",
		.of_match_table = qcom_cpufreq_hw_match,
	},
};

static int __init qcom_cpufreq_hw_init(void)
{
	return platform_driver_register(&qcom_cpufreq_hw_driver);
}
subsys_initcall(qcom_cpufreq_hw_init);

MODULE_DESCRIPTION("QCOM firmware-based CPU Frequency driver");

