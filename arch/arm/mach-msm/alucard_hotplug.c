/*
 * Author: Alucard_24@XDA
 *
 * Copyright 2012 Alucard_24@XDA
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "acpuclock.h"

#include <linux/powersuspend.h>

static struct delayed_work alucard_hotplug_work;
static struct workqueue_struct *alucardhp_wq;

struct hotplug_cpuinfo {
	int online;
	int up_cpu;
	int up_by_cpu;
};

static DEFINE_PER_CPU(struct hotplug_cpuinfo, od_hotplug_cpuinfo);

static atomic_t suspended = ATOMIC_INIT(0);

extern unsigned int report_cpu_load(unsigned int cpu);

static struct hotplug_tuners {
	int hotplug_sampling_rate;
	atomic_t hotplug_enable;
	int cpu_up_rate;
	int cpu_down_rate;
	atomic_t maxcoreslimit;
	atomic_t maxcoreslimit_sleep;
} hotplug_tuners_ins = {
	.hotplug_sampling_rate = 60,
	.hotplug_enable = ATOMIC_INIT(0),
	.cpu_up_rate = 1,
	.cpu_down_rate = 10,
	.maxcoreslimit = ATOMIC_INIT(NR_CPUS),
	.maxcoreslimit_sleep = ATOMIC_INIT(1),
};

#define MAX_HOTPLUG_RATE	(40)
#define DOWN_INDEX		(0)
#define UP_INDEX		(1)

struct runqueue_data {
	unsigned int nr_run_avg;
	int64_t last_time;
	int64_t total_time;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

static unsigned int get_nr_run_avg(void)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time;
	unsigned int nr_run_avg;

	cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;

	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}

static int hotplugging_rate = 0;

static int hotplug_freq[4][2] = {
	{0, 1267200},
	{960000, 1497600},
	{1036800, 1728000},
	{1190400, 0}
};
static int hotplug_load[4][2] = {
	{0, 65},
	{30, 65},
	{30, 65},
	{30, 0}
};
static int hotplug_rq[4][2] = {
	{0, 300},
	{300, 300},
	{300, 300},
	{300, 0}
};

static void __ref hotplug_work_fn(struct work_struct *work)
{
	int upmaxcoreslimit = 0;
	int up_rate = hotplug_tuners_ins.cpu_up_rate;
	int down_rate = hotplug_tuners_ins.cpu_down_rate;
	unsigned int sampling_rate = hotplug_tuners_ins.hotplug_sampling_rate;
	bool check_up = false, check_down = false;
	int schedule_down_cpu = 3;
	int schedule_up_cpu = 0;
	unsigned int cpu = 0;
	int online_cpu = 0;
	int offline_cpu = 0;
	int i = 0;
	int online_cpus = 0;
	unsigned int rq_avg = 0;
	int delay;
	int cpus_off[4] = {-1, -1, -1, -1};
	int cpus_on[4] = {-1, -1, -1, -1};
	int idx_off = 0;
	int suspend = 0;
	bool hotplugged = false;

	delay = msecs_to_jiffies(sampling_rate);
	/* set hotplugging_rate used */
	++hotplugging_rate;
	check_up = (hotplugging_rate % up_rate == 0);
	check_down = (hotplugging_rate % down_rate == 0);
	rq_avg = get_nr_run_avg();

	suspend = atomic_read(&suspended);
	if (suspend)
		upmaxcoreslimit = atomic_read(&hotplug_tuners_ins.maxcoreslimit_sleep);
	else
		upmaxcoreslimit = atomic_read(&hotplug_tuners_ins.maxcoreslimit);

	get_online_cpus();
	online_cpus = num_online_cpus();
	pr_debug("ONLINE CPUS[%u], CPU[%u], \
				rq_avg[%u]\n", online_cpus, \
				cpu, rq_avg);
	for_each_cpu_not(cpu, cpu_online_mask) {
		struct hotplug_cpuinfo *pcpu_info;

		pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

		cpus_off[idx_off] = cpu;
		++idx_off;
		++schedule_up_cpu;
		--schedule_down_cpu;

		pcpu_info->online = false;
		pcpu_info->up_cpu = -1;
		pcpu_info->up_by_cpu = -1;
	}

	for_each_online_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info;
		int up_load;
		int down_load;
		unsigned int up_freq;
		unsigned int down_freq;
		unsigned int up_rq;
		unsigned int down_rq;
		int cur_load = -1;
		unsigned int cur_freq = 0;

		cur_load = report_cpu_load(cpu);

		pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

		/* if cur_load < 0, evaluate cpu load next time */
		if (cur_load >= 0) {
			/* get the cpu current frequency */
			cur_freq = acpuclk_get_rate(cpu);

			up_load = hotplug_load[cpu][UP_INDEX];
			down_load = hotplug_load[cpu][DOWN_INDEX];
			up_freq = hotplug_freq[cpu][UP_INDEX];
			down_freq = hotplug_freq[cpu][DOWN_INDEX];
			up_rq = hotplug_rq[cpu][UP_INDEX];
			down_rq = hotplug_rq[cpu][DOWN_INDEX];

			if (cpu > 0 && (online_cpus - online_cpu) >
						upmaxcoreslimit) {
				pcpu_info->online = false;
				cpus_on[online_cpu] = cpu;
				++online_cpu;
				--schedule_down_cpu;
			} else if (check_up
					&& (online_cpus + offline_cpu) <
					upmaxcoreslimit &&
					pcpu_info->up_cpu == -1
					&& schedule_up_cpu > 0
					&& cur_load >= up_load
					&& cur_freq >= up_freq
					&& rq_avg > up_rq) {
				if (offline_cpu < idx_off
						&& cpus_off[offline_cpu] > 0) {
					pcpu_info =
						&per_cpu(od_hotplug_cpuinfo,
						cpus_off[offline_cpu]);
					pcpu_info->online = true;
					pcpu_info->up_by_cpu = cpu;
					++offline_cpu;
					--schedule_up_cpu;
				}
			} else if (check_down
					&& cpu > 0
					&& schedule_down_cpu > 0
					&& cur_load >= 0) {
				if (cur_load < down_load
						|| (cur_freq <= down_freq
						&& rq_avg <= down_rq)) {
					pcpu_info->online = false;
					cpus_on[online_cpu] = cpu;
					++online_cpu;
					--schedule_down_cpu;
				}
			}
		}
	}
	put_online_cpus();

	if (offline_cpu > 0) {
		for (i = 0; i < offline_cpu; i++) {
			struct hotplug_cpuinfo *pcpu_info;
			int ret = 0;
			int refcpu = -1;

			pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpus_off[i]);

			if (pcpu_info->online == true) {
				ret = cpu_up(cpus_off[i]);
				if (!ret) {
					refcpu = pcpu_info->up_by_cpu;
					if (refcpu >= 0) {
						pcpu_info = &per_cpu(od_hotplug_cpuinfo, refcpu);
						pcpu_info->up_cpu = cpus_off[i];
					}
					hotplugged = true;
				} else {
					pr_debug("AH: Error %d online core %d\n", ret, cpus_off[i]);
					pcpu_info->online = false;
					pcpu_info->up_by_cpu = -1;
				}

			}
		}
	}

	if (online_cpu > 0) {
		for (i = 0; i < online_cpu; i++) {
			struct hotplug_cpuinfo *pcpu_info;
			int ret = 0;
			int refcpu = -1;

			pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpus_on[i]);

			if (pcpu_info->online == false) {
				ret = cpu_down(cpus_on[i]);
				if (!ret) {
					refcpu = pcpu_info->up_by_cpu;
					pcpu_info->up_cpu = -1;
					pcpu_info->up_by_cpu = -1;
					if (refcpu >= 0) {
						pcpu_info = &per_cpu(od_hotplug_cpuinfo, refcpu);
						pcpu_info->up_cpu = -1;
					}
					hotplugged = true;
				} else {
					pr_debug("AH: Error %d offline" "core %d\n", ret, cpus_on[i]);
					pcpu_info->online = true;
				}
			}
		}
	}

	get_online_cpus();
	if (num_online_cpus() == 1) {
		struct hotplug_cpuinfo *pcpu_info;
		pcpu_info = &per_cpu(od_hotplug_cpuinfo, 0);
		pcpu_info->up_cpu = -1;
	}
	put_online_cpus();

	if (hotplugging_rate >= max(up_rate, down_rate) || hotplugged == true) {
		hotplugging_rate = 0;
	}

	queue_delayed_work_on(0, alucardhp_wq, &alucard_hotplug_work, delay);
}

static void __ref alucard_hotplug_suspend(struct power_suspend *handler)
{

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0) {
		atomic_set(&suspended, 1);
	}
}

static void __ref alucard_hotplug_resume(struct power_suspend *handler)
{
	int maxcoreslimit = 0;
	int i = 0;
	int prev_online = 0;

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0) {
		/* wake up everyone */
		maxcoreslimit = atomic_read(&hotplug_tuners_ins.maxcoreslimit);
		flush_workqueue(alucardhp_wq);

		atomic_set(&suspended, 0);

		for (i = 1; i < maxcoreslimit; i++) {
			struct hotplug_cpuinfo *pcpu_info;
			int ret = 0;

			pcpu_info = &per_cpu(od_hotplug_cpuinfo, i);

			if (!cpu_online(i)) {
				ret = cpu_up(i);
				if (!ret) {
					pcpu_info->online = true;
					pcpu_info->up_by_cpu = prev_online;
					pcpu_info->up_cpu = -1;

					pcpu_info = &per_cpu(od_hotplug_cpuinfo, prev_online);
					pcpu_info->up_cpu = i;

					prev_online = i;
				}
			} else {
				prev_online = i;
			}
		}
	}
}

static struct power_suspend alucard_hotplug_power_suspend_driver = {
	.suspend = alucard_hotplug_suspend,
	.resume = alucard_hotplug_resume,
};

static int __ref hotplug_start(void)
{
	unsigned int cpu;
	int delay = msecs_to_jiffies(hotplug_tuners_ins.hotplug_sampling_rate);
	int ret = 0;

	alucardhp_wq = create_singlethread_workqueue("alucardplug");

	if (!alucardhp_wq) {
		printk(KERN_ERR "Failed to create \
				alucardhp workqueue\n");
		return -EFAULT;
	}

	ret = init_rq_avg();
	if (ret) {
		destroy_workqueue(alucardhp_wq);
		return ret;
	}

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info;

		pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

		pcpu_info->online = cpu_online(cpu);

		pcpu_info->up_cpu = -1;
		pcpu_info->up_by_cpu = -1;

		hotplugging_rate = 0;

		atomic_set(&suspended,0);

	}
	put_online_cpus();

	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;

	INIT_DELAYED_WORK(&alucard_hotplug_work, hotplug_work_fn);
	queue_delayed_work_on(0, alucardhp_wq, &alucard_hotplug_work,
						delay);

	register_power_suspend(&alucard_hotplug_power_suspend_driver);

	return 0;
}

static void hotplug_stop(void)
{
	unregister_power_suspend(&alucard_hotplug_power_suspend_driver);

	cancel_delayed_work_sync(&alucard_hotplug_work);

	kfree(rq_data);

	destroy_workqueue(alucardhp_wq);
}

#define show_atomic(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", \
			atomic_read(&hotplug_tuners_ins.object));	\
}

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", \
			hotplug_tuners_ins.object);			\
}

show_one(hotplug_sampling_rate, hotplug_sampling_rate);
show_atomic(hotplug_enable, hotplug_enable);
show_one(cpu_up_rate, cpu_up_rate);
show_one(cpu_down_rate, cpu_down_rate);
show_atomic(maxcoreslimit, maxcoreslimit);
show_atomic(maxcoreslimit_sleep, maxcoreslimit_sleep);

#define show_hotplug_param(file_name, num_core, up_down)		\
static ssize_t show_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", \
			file_name[num_core - 1][up_down]);		\
}

#define store_hotplug_param(file_name, num_core, up_down)		\
static ssize_t store_##file_name##_##num_core##_##up_down		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%d", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	if (input == file_name[num_core - 1][up_down]) {		\
		return count;						\
	}								\
	file_name[num_core - 1][up_down] = input;			\
	return count;							\
}

/* hotplug freq */
show_hotplug_param(hotplug_freq, 1, 1);
show_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_freq, 2, 1);
show_hotplug_param(hotplug_freq, 3, 0);
show_hotplug_param(hotplug_freq, 3, 1);
show_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
show_hotplug_param(hotplug_load, 1, 1);
show_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_load, 2, 1);
show_hotplug_param(hotplug_load, 3, 0);
show_hotplug_param(hotplug_load, 3, 1);
show_hotplug_param(hotplug_load, 4, 0);
#endif
/* hotplug rq */
show_hotplug_param(hotplug_rq, 1, 1);
show_hotplug_param(hotplug_rq, 2, 0);
#if NR_CPUS >= 4
show_hotplug_param(hotplug_rq, 2, 1);
show_hotplug_param(hotplug_rq, 3, 0);
show_hotplug_param(hotplug_rq, 3, 1);
show_hotplug_param(hotplug_rq, 4, 0);
#endif

/* hotplug freq */
store_hotplug_param(hotplug_freq, 1, 1);
store_hotplug_param(hotplug_freq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_freq, 2, 1);
store_hotplug_param(hotplug_freq, 3, 0);
store_hotplug_param(hotplug_freq, 3, 1);
store_hotplug_param(hotplug_freq, 4, 0);
#endif
/* hotplug load */
store_hotplug_param(hotplug_load, 1, 1);
store_hotplug_param(hotplug_load, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_load, 2, 1);
store_hotplug_param(hotplug_load, 3, 0);
store_hotplug_param(hotplug_load, 3, 1);
store_hotplug_param(hotplug_load, 4, 0);
#endif
/* hotplug rq */
store_hotplug_param(hotplug_rq, 1, 1);
store_hotplug_param(hotplug_rq, 2, 0);
#if NR_CPUS >= 4
store_hotplug_param(hotplug_rq, 2, 1);
store_hotplug_param(hotplug_rq, 3, 0);
store_hotplug_param(hotplug_rq, 3, 1);
store_hotplug_param(hotplug_rq, 4, 0);
#endif

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);
#endif

define_one_global_rw(hotplug_load_1_1);
define_one_global_rw(hotplug_load_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_load_2_1);
define_one_global_rw(hotplug_load_3_0);
define_one_global_rw(hotplug_load_3_1);
define_one_global_rw(hotplug_load_4_0);
#endif

define_one_global_rw(hotplug_rq_1_1);
define_one_global_rw(hotplug_rq_2_0);
#if NR_CPUS >= 4
define_one_global_rw(hotplug_rq_2_1);
define_one_global_rw(hotplug_rq_3_0);
define_one_global_rw(hotplug_rq_3_1);
define_one_global_rw(hotplug_rq_4_0);
#endif

static void cpus_hotplugging(int status) {
	int ret = 0;

	if (status) {
		ret = hotplug_start();
		if (ret)
			status = 0;
	} else {
		hotplug_stop();
	}

	atomic_set(&hotplug_tuners_ins.hotplug_enable, status);
}

/**
 * update_sampling_rate - update sampling rate effective immediately if needed.
 * @new_rate: new sampling rate
 *
 * If new rate is smaller than the old, simply updaing
 * hotplug_tuners_ins.hotplug_sampling_rate might not be appropriate.
 * For example:
 * if the original sampling_rate was 1 second and the requested new sampling
 * rate is 10 ms because the user needs immediate reaction from ondemand
 * governor, but not sure if higher frequency will be required or not,
 * then, the hotplugging system may change the sampling rate too late;
 * up to 1 second later.
 * Thus, if we are reducing the hotplug sampling rate, we need to make the
 * new value effective immediately.
 */
static void update_sampling_rate(unsigned int new_rate)
{
	unsigned long next_sampling, appointed_at;

	hotplug_tuners_ins.hotplug_sampling_rate = new_rate;

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0) {

		if (!delayed_work_pending(&alucard_hotplug_work)) {
			return;
		}

		next_sampling  = jiffies + msecs_to_jiffies(new_rate);
		appointed_at = alucard_hotplug_work.timer.expires;

		if (time_before(next_sampling, appointed_at)) {

			cancel_delayed_work_sync(&alucard_hotplug_work);

			queue_delayed_work_on(0, alucardhp_wq, &alucard_hotplug_work,
					msecs_to_jiffies(new_rate));
		}

	}

}

/* hotplug_sampling_rate */
static ssize_t store_hotplug_sampling_rate(struct kobject *a,
				struct attribute *b,
				const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input, 10);

	if (input == hotplug_tuners_ins.hotplug_sampling_rate)
		return count;

	update_sampling_rate(input);

	return count;
}

/* hotplug_enable */
static ssize_t store_hotplug_enable(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0;

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) == input)
		return count;

	if (input > 0)
		cpus_hotplugging(1);
	else
		cpus_hotplugging(0);

	return count;
}

/* cpu_up_rate */
static ssize_t store_cpu_up_rate(struct kobject *a, struct attribute *b,
				 const char *buf, size_t count)
{
	int input;
	int ret;
	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,MAX_HOTPLUG_RATE),1);

	if (input == hotplug_tuners_ins.cpu_up_rate)
		return count;

	hotplug_tuners_ins.cpu_up_rate = input;

	return count;
}

/* cpu_down_rate */
static ssize_t store_cpu_down_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,MAX_HOTPLUG_RATE),1);

	if (input == hotplug_tuners_ins.cpu_down_rate)
		return count;

	hotplug_tuners_ins.cpu_down_rate = input;
	return count;
}

/* maxcoreslimit */
static ssize_t store_maxcoreslimit(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1);

	if (atomic_read(&hotplug_tuners_ins.maxcoreslimit) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.maxcoreslimit, input);

	return count;
}

/* maxcoreslimit_sleep */
static ssize_t store_maxcoreslimit_sleep(struct kobject *a,
				struct attribute *b,
				const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1);

	if (atomic_read(&hotplug_tuners_ins.maxcoreslimit_sleep) == input)
		return count;

	atomic_set(&hotplug_tuners_ins.maxcoreslimit_sleep, input);

	return count;
}

define_one_global_rw(hotplug_sampling_rate);
define_one_global_rw(hotplug_enable);
define_one_global_rw(cpu_up_rate);
define_one_global_rw(cpu_down_rate);
define_one_global_rw(maxcoreslimit);
define_one_global_rw(maxcoreslimit_sleep);

static struct attribute *alucard_hotplug_attributes[] = {
	&hotplug_sampling_rate.attr,
	&hotplug_enable.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
#endif
	&hotplug_load_1_1.attr,
	&hotplug_load_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_load_2_1.attr,
	&hotplug_load_3_0.attr,
	&hotplug_load_3_1.attr,
	&hotplug_load_4_0.attr,
#endif
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
#if NR_CPUS >= 4
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
#endif
	&cpu_up_rate.attr,
	&cpu_down_rate.attr,
	&maxcoreslimit.attr,
	&maxcoreslimit_sleep.attr,
	NULL
};

static struct attribute_group alucard_hotplug_attr_group = {
	.attrs = alucard_hotplug_attributes,
	.name = "alucard_hotplug",
};

static int __init alucard_hotplug_init(void)
{
	int ret;

	ret = sysfs_create_group(kernel_kobj, &alucard_hotplug_attr_group);
	if (ret) {
		printk(KERN_ERR "failed at(%d)\n", __LINE__);
		return ret;
	}

	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0) {
		hotplug_start();
	}

	return ret;
}
EXPORT_SYMBOL_GPL(alucard_hotplug_init);

static void __exit alucard_hotplug_exit(void)
{
	if (atomic_read(&hotplug_tuners_ins.hotplug_enable) > 0) {
		hotplug_stop();
	}

	sysfs_remove_group(kernel_kobj, &alucard_hotplug_attr_group);
}
EXPORT_SYMBOL_GPL(alucard_hotplug_exit);

late_initcall(alucard_hotplug_init);
module_exit(alucard_hotplug_exit);

MODULE_AUTHOR("Alucard_24@XDA");
MODULE_DESCRIPTION("'alucard_hotplug' - A cpu hotplug driver for "
	"capable processors");
MODULE_LICENSE("GPLv2");
