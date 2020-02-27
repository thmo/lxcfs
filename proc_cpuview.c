/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 26
#endif

#define _FILE_OFFSET_BITS 64

#define __STDC_FORMAT_MACROS
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wait.h>
#include <linux/magic.h>
#include <linux/sched.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>

#include "bindings.h"
#include "config.h"
#include "cgroup_fuse.h"
#include "cpuset_parse.h"
#include "cgroups/cgroup.h"
#include "cgroups/cgroup_utils.h"
#include "memory_utils.h"
#include "proc_loadavg.h"
#include "utils.h"

/* Data for CPU view */
struct cg_proc_stat {
	char *cg;
	struct cpuacct_usage *usage; // Real usage as read from the host's /proc/stat
	struct cpuacct_usage *view; // Usage stats reported to the container
	int cpu_count;
	pthread_mutex_t lock; // For node manipulation
	struct cg_proc_stat *next;
};

struct cg_proc_stat_head {
	struct cg_proc_stat *next;
	time_t lastcheck;

	/*
	 * For access to the list. Reading can be parallel, pruning is exclusive.
	 */
	pthread_rwlock_t lock;
};

#define CPUVIEW_HASH_SIZE 100
static struct cg_proc_stat_head *proc_stat_history[CPUVIEW_HASH_SIZE];

static void reset_proc_stat_node(struct cg_proc_stat *node, struct cpuacct_usage *usage, int cpu_count)
{
	int i;

	lxcfs_debug("Resetting stat node for %s\n", node->cg);
	memcpy(node->usage, usage, sizeof(struct cpuacct_usage) * cpu_count);

	for (i = 0; i < cpu_count; i++) {
		node->view[i].user = 0;
		node->view[i].system = 0;
		node->view[i].idle = 0;
	}

	node->cpu_count = cpu_count;
}

static bool expand_proc_stat_node(struct cg_proc_stat *node, int cpu_count)
{
	__do_free struct cpuacct_usage *new_usage = NULL, *new_view = NULL;

	/* Allocate new memory */
	new_usage = malloc(sizeof(struct cpuacct_usage) * cpu_count);
	if (!new_usage)
		return false;

	new_view = malloc(sizeof(struct cpuacct_usage) * cpu_count);
	if (!new_view)
		return false;

	/* Copy existing data & initialize new elements */
	for (int i = 0; i < cpu_count; i++) {
		if (i < node->cpu_count) {
			new_usage[i].user = node->usage[i].user;
			new_usage[i].system = node->usage[i].system;
			new_usage[i].idle = node->usage[i].idle;

			new_view[i].user = node->view[i].user;
			new_view[i].system = node->view[i].system;
			new_view[i].idle = node->view[i].idle;
		} else {
			new_usage[i].user = 0;
			new_usage[i].system = 0;
			new_usage[i].idle = 0;

			new_view[i].user = 0;
			new_view[i].system = 0;
			new_view[i].idle = 0;
		}
	}

	free(node->usage);
	node->usage = move_ptr(new_usage);

	free(node->view);
	node->view = move_ptr(new_view);
	node->cpu_count = cpu_count;

	return true;
}

static void free_proc_stat_node(struct cg_proc_stat *node)
{
	pthread_mutex_destroy(&node->lock);
	free_disarm(node->cg);
	free_disarm(node->usage);
	free_disarm(node->view);
	free_disarm(node);
}

static struct cg_proc_stat *add_proc_stat_node(struct cg_proc_stat *new_node)
{
	int hash = calc_hash(new_node->cg) % CPUVIEW_HASH_SIZE;
	struct cg_proc_stat_head *head = proc_stat_history[hash];
	struct cg_proc_stat *node, *rv = new_node;

	pthread_rwlock_wrlock(&head->lock);

	if (!head->next) {
		head->next = new_node;
		goto out;
	}

	node = head->next;

	for (;;) {
		if (strcmp(node->cg, new_node->cg) == 0) {
			/* The node is already present, return it */
			free_proc_stat_node(new_node);
			rv = node;
			goto out;
		}

		if (node->next) {
			node = node->next;
			continue;
		}

		node->next = new_node;
		goto out;
	}

out:
	pthread_rwlock_unlock(&head->lock);
	return rv;
}

static struct cg_proc_stat *new_proc_stat_node(struct cpuacct_usage *usage, int cpu_count, const char *cg)
{
	struct cg_proc_stat *node;
	int i;

	node = malloc(sizeof(struct cg_proc_stat));
	if (!node)
		goto err;

	node->cg = NULL;
	node->usage = NULL;
	node->view = NULL;

	node->cg = malloc(strlen(cg) + 1);
	if (!node->cg)
		goto err;

	strcpy(node->cg, cg);

	node->usage = malloc(sizeof(struct cpuacct_usage) * cpu_count);
	if (!node->usage)
		goto err;

	memcpy(node->usage, usage, sizeof(struct cpuacct_usage) * cpu_count);

	node->view = malloc(sizeof(struct cpuacct_usage) * cpu_count);
	if (!node->view)
		goto err;

	node->cpu_count = cpu_count;
	node->next = NULL;

	if (pthread_mutex_init(&node->lock, NULL) != 0) {
		lxcfs_error("%s\n", "Failed to initialize node lock");
		goto err;
	}

	for (i = 0; i < cpu_count; i++) {
		node->view[i].user = 0;
		node->view[i].system = 0;
		node->view[i].idle = 0;
	}

	return node;

err:
	if (node && node->cg)
		free(node->cg);
	if (node && node->usage)
		free(node->usage);
	if (node && node->view)
		free(node->view);
	if (node)
		free(node);

	return NULL;
}

static bool cgfs_param_exist(const char *controller, const char *cgroup,
			     const char *file)
{
	int ret, cfd;
	size_t len;
	char *fnam;

	cfd = get_cgroup_fd(controller);
	if (cfd < 0)
		return false;

	/* Make sure we pass a relative path to *at() family of functions.
	 * . + /cgroup + / + file + \0
	 */
	len = strlen(cgroup) + strlen(file) + 3;
	fnam = alloca(len);
	ret = snprintf(fnam, len, "%s%s/%s", dot_or_empty(cgroup), cgroup, file);
	if (ret < 0 || (size_t)ret >= len)
		return false;

	return (faccessat(cfd, fnam, F_OK, 0) == 0);
}

static struct cg_proc_stat *prune_proc_stat_list(struct cg_proc_stat *node)
{
	struct cg_proc_stat *first = NULL, *prev, *tmp;

	for (prev = NULL; node; ) {
		if (!cgfs_param_exist("cpu", node->cg, "cpu.shares")) {
			tmp = node;
			lxcfs_debug("Removing stat node for %s\n", node->cg);

			if (prev)
				prev->next = node->next;
			else
				first = node->next;

			node = node->next;
			free_proc_stat_node(tmp);
		} else {
			if (!first)
				first = node;
			prev = node;
			node = node->next;
		}
	}

	return first;
}

#define PROC_STAT_PRUNE_INTERVAL 10
static void prune_proc_stat_history(void)
{
	int i;
	time_t now = time(NULL);

	for (i = 0; i < CPUVIEW_HASH_SIZE; i++) {
		pthread_rwlock_wrlock(&proc_stat_history[i]->lock);

		if ((proc_stat_history[i]->lastcheck + PROC_STAT_PRUNE_INTERVAL) > now) {
			pthread_rwlock_unlock(&proc_stat_history[i]->lock);
			return;
		}

		if (proc_stat_history[i]->next) {
			proc_stat_history[i]->next = prune_proc_stat_list(proc_stat_history[i]->next);
			proc_stat_history[i]->lastcheck = now;
		}

		pthread_rwlock_unlock(&proc_stat_history[i]->lock);
	}
}

static struct cg_proc_stat *find_proc_stat_node(struct cg_proc_stat_head *head,
						const char *cg)
{
	struct cg_proc_stat *node;

	pthread_rwlock_rdlock(&head->lock);

	if (!head->next) {
		pthread_rwlock_unlock(&head->lock);
		return NULL;
	}

	node = head->next;

	do {
		if (strcmp(cg, node->cg) == 0)
			goto out;
	} while ((node = node->next));

	node = NULL;

out:
	pthread_rwlock_unlock(&head->lock);
	prune_proc_stat_history();
	return node;
}

static struct cg_proc_stat *find_or_create_proc_stat_node(struct cpuacct_usage *usage, int cpu_count, const char *cg)
{
	int hash = calc_hash(cg) % CPUVIEW_HASH_SIZE;
	struct cg_proc_stat_head *head = proc_stat_history[hash];
	struct cg_proc_stat *node;

	node = find_proc_stat_node(head, cg);

	if (!node) {
		node = new_proc_stat_node(usage, cpu_count, cg);
		if (!node)
			return NULL;

		node = add_proc_stat_node(node);
		lxcfs_debug("New stat node (%d) for %s\n", cpu_count, cg);
	}

	pthread_mutex_lock(&node->lock);

	/* If additional CPUs on the host have been enabled, CPU usage counter
	 * arrays have to be expanded */
	if (node->cpu_count < cpu_count) {
		lxcfs_debug("Expanding stat node %d->%d for %s\n",
				node->cpu_count, cpu_count, cg);

		if (!expand_proc_stat_node(node, cpu_count)) {
			pthread_mutex_unlock(&node->lock);
			lxcfs_debug("Unable to expand stat node %d->%d for %s\n",
					node->cpu_count, cpu_count, cg);
			return NULL;
		}
	}

	return node;
}

static void add_cpu_usage(unsigned long *surplus, struct cpuacct_usage *usage,
			  unsigned long *counter, unsigned long threshold)
{
	unsigned long free_space, to_add;

	free_space = threshold - usage->user - usage->system;

	if (free_space > usage->idle)
		free_space = usage->idle;

	to_add = free_space > *surplus ? *surplus : free_space;

	*counter += to_add;
	usage->idle -= to_add;
	*surplus -= to_add;
}

static unsigned long diff_cpu_usage(struct cpuacct_usage *older,
				    struct cpuacct_usage *newer,
				    struct cpuacct_usage *diff, int cpu_count)
{
	int i;
	unsigned long sum = 0;

	for (i = 0; i < cpu_count; i++) {
		if (!newer[i].online)
			continue;

		/* When cpuset is changed on the fly, the CPUs might get reordered.
		 * We could either reset all counters, or check that the substractions
		 * below will return expected results.
		 */
		if (newer[i].user > older[i].user)
			diff[i].user = newer[i].user - older[i].user;
		else
			diff[i].user = 0;

		if (newer[i].system > older[i].system)
			diff[i].system = newer[i].system - older[i].system;
		else
			diff[i].system = 0;

		if (newer[i].idle > older[i].idle)
			diff[i].idle = newer[i].idle - older[i].idle;
		else
			diff[i].idle = 0;

		sum += diff[i].user;
		sum += diff[i].system;
		sum += diff[i].idle;
	}

	return sum;
}

/*
 * Read cgroup CPU quota parameters from `cpu.cfs_quota_us` or `cpu.cfs_period_us`,
 * depending on `param`. Parameter value is returned throuh `value`.
 */
static bool read_cpu_cfs_param(const char *cg, const char *param, int64_t *value)
{
	__do_free char *str = NULL;
	char file[11 + 6 + 1]; /* cpu.cfs__us + quota/period + \0 */

	snprintf(file, sizeof(file), "cpu.cfs_%s_us", param);

	if (!cgroup_ops->get(cgroup_ops, "cpu", cg, file, &str))
		return false;

	if (sscanf(str, "%ld", value) != 1)
		return false;

	return true;
}

/*
 * Return the exact number of visible CPUs based on CPU quotas.
 * If there is no quota set, zero is returned.
 */
static double exact_cpu_count(const char *cg)
{
	double rv;
	int nprocs;
	int64_t cfs_quota, cfs_period;

	if (!read_cpu_cfs_param(cg, "quota", &cfs_quota))
		return 0;

	if (!read_cpu_cfs_param(cg, "period", &cfs_period))
		return 0;

	if (cfs_quota <= 0 || cfs_period <= 0)
		return 0;

	rv = (double)cfs_quota / (double)cfs_period;

	nprocs = get_nprocs();

	if (rv > nprocs)
		rv = nprocs;

	return rv;
}

/*
 * Return the maximum number of visible CPUs based on CPU quotas.
 * If there is no quota set, zero is returned.
 */
int max_cpu_count(const char *cg)
{
	int rv, nprocs;
	int64_t cfs_quota, cfs_period;
	int nr_cpus_in_cpuset = 0;
	char *cpuset = NULL;

	if (!read_cpu_cfs_param(cg, "quota", &cfs_quota))
		return 0;

	if (!read_cpu_cfs_param(cg, "period", &cfs_period))
		return 0;

	cpuset = get_cpuset(cg);
	if (cpuset)
		nr_cpus_in_cpuset = cpu_number_in_cpuset(cpuset);

	if (cfs_quota <= 0 || cfs_period <= 0){
		if (nr_cpus_in_cpuset > 0)
			return nr_cpus_in_cpuset;

		return 0;
	}

	rv = cfs_quota / cfs_period;

	/* In case quota/period does not yield a whole number, add one CPU for
	 * the remainder.
	 */
	if ((cfs_quota % cfs_period) > 0)
		rv += 1;

	nprocs = get_nprocs();

	if (rv > nprocs)
		rv = nprocs;

	/* use min value in cpu quota and cpuset */
	if (nr_cpus_in_cpuset > 0 && nr_cpus_in_cpuset < rv)
		rv = nr_cpus_in_cpuset;

	return rv;
}

int cpuview_proc_stat(const char *cg, const char *cpuset,
		      struct cpuacct_usage *cg_cpu_usage, int cg_cpu_usage_size,
		      FILE *f, char *buf, size_t buf_size)
{
	__do_free char *line = NULL;
	__do_free struct cpuacct_usage *diff = NULL;
	size_t linelen = 0, total_len = 0, l;
	int curcpu = -1; /* cpu numbering starts at 0 */
	int physcpu, i;
	int max_cpus = max_cpu_count(cg), cpu_cnt = 0;
	unsigned long user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
		      irq = 0, softirq = 0, steal = 0, guest = 0, guest_nice = 0;
	unsigned long user_sum = 0, system_sum = 0, idle_sum = 0;
	unsigned long user_surplus = 0, system_surplus = 0;
	unsigned long total_sum, threshold;
	struct cg_proc_stat *stat_node;
	int nprocs = get_nprocs_conf();

	if (cg_cpu_usage_size < nprocs)
		nprocs = cg_cpu_usage_size;

	/* Read all CPU stats and stop when we've encountered other lines */
	while (getline(&line, &linelen, f) != -1) {
		int ret;
		char cpu_char[10]; /* That's a lot of cores */
		uint64_t all_used, cg_used;

		if (strlen(line) == 0)
			continue;

		/* not a ^cpuN line containing a number N */
		if (sscanf(line, "cpu%9[^ ]", cpu_char) != 1)
			break;

		if (sscanf(cpu_char, "%d", &physcpu) != 1)
			continue;

		if (physcpu >= cg_cpu_usage_size)
			continue;

		curcpu ++;
		cpu_cnt ++;

		if (!cpu_in_cpuset(physcpu, cpuset)) {
			for (i = curcpu; i <= physcpu; i++)
				cg_cpu_usage[i].online = false;
			continue;
		}

		if (curcpu < physcpu) {
			/* Some CPUs may be disabled */
			for (i = curcpu; i < physcpu; i++)
				cg_cpu_usage[i].online = false;

			curcpu = physcpu;
		}

		cg_cpu_usage[curcpu].online = true;

		ret = sscanf(line, "%*s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
			   &user,
			   &nice,
			   &system,
			   &idle,
			   &iowait,
			   &irq,
			   &softirq,
			   &steal,
			   &guest,
			   &guest_nice);

		if (ret != 10)
			continue;

		all_used = user + nice + system + iowait + irq + softirq + steal + guest + guest_nice;
		cg_used = cg_cpu_usage[curcpu].user + cg_cpu_usage[curcpu].system;

		if (all_used >= cg_used) {
			cg_cpu_usage[curcpu].idle = idle + (all_used - cg_used);

		} else {
			lxcfs_error("cpu%d from %s has unexpected cpu time: %lu in /proc/stat, "
					"%lu in cpuacct.usage_all; unable to determine idle time\n",
					curcpu, cg, all_used, cg_used);
			cg_cpu_usage[curcpu].idle = idle;
		}
	}

	/* Cannot use more CPUs than is available due to cpuset */
	if (max_cpus > cpu_cnt)
		max_cpus = cpu_cnt;

	stat_node = find_or_create_proc_stat_node(cg_cpu_usage, nprocs, cg);

	if (!stat_node) {
		lxcfs_error("unable to find/create stat node for %s\n", cg);
		return 0;
	}

	diff = malloc(sizeof(struct cpuacct_usage) * nprocs);
	if (!diff) {
		return 0;
	}

	/*
	 * If the new values are LOWER than values stored in memory, it means
	 * the cgroup has been reset/recreated and we should reset too.
	 */
	for (curcpu = 0; curcpu < nprocs; curcpu++) {
		if (!cg_cpu_usage[curcpu].online)
			continue;

		if (cg_cpu_usage[curcpu].user < stat_node->usage[curcpu].user)
			reset_proc_stat_node(stat_node, cg_cpu_usage, nprocs);

		break;
	}

	total_sum = diff_cpu_usage(stat_node->usage, cg_cpu_usage, diff, nprocs);

	for (curcpu = 0, i = -1; curcpu < nprocs; curcpu++) {
		stat_node->usage[curcpu].online = cg_cpu_usage[curcpu].online;

		if (!stat_node->usage[curcpu].online)
			continue;

		i++;

		stat_node->usage[curcpu].user += diff[curcpu].user;
		stat_node->usage[curcpu].system += diff[curcpu].system;
		stat_node->usage[curcpu].idle += diff[curcpu].idle;

		if (max_cpus > 0 && i >= max_cpus) {
			user_surplus += diff[curcpu].user;
			system_surplus += diff[curcpu].system;
		}
	}

	/* Calculate usage counters of visible CPUs */
	if (max_cpus > 0) {
		unsigned long diff_user = 0;
		unsigned long diff_system = 0;
		unsigned long diff_idle = 0;
		unsigned long max_diff_idle = 0;
		unsigned long max_diff_idle_index = 0;
		double exact_cpus;

		/* threshold = maximum usage per cpu, including idle */
		threshold = total_sum / cpu_cnt * max_cpus;

		for (curcpu = 0, i = -1; curcpu < nprocs; curcpu++) {
			if (!stat_node->usage[curcpu].online)
				continue;

			i++;

			if (i == max_cpus)
				break;

			if (diff[curcpu].user + diff[curcpu].system >= threshold)
				continue;

			/* Add user */
			add_cpu_usage(&user_surplus, &diff[curcpu],
				      &diff[curcpu].user, threshold);

			if (diff[curcpu].user + diff[curcpu].system >= threshold)
				continue;

			/* If there is still room, add system */
			add_cpu_usage(&system_surplus, &diff[curcpu],
				      &diff[curcpu].system, threshold);
		}

		if (user_surplus > 0)
			lxcfs_debug("leftover user: %lu for %s\n", user_surplus, cg);
		if (system_surplus > 0)
			lxcfs_debug("leftover system: %lu for %s\n", system_surplus, cg);

		for (curcpu = 0, i = -1; curcpu < nprocs; curcpu++) {
			if (!stat_node->usage[curcpu].online)
				continue;

			i++;

			if (i == max_cpus)
				break;

			stat_node->view[curcpu].user += diff[curcpu].user;
			stat_node->view[curcpu].system += diff[curcpu].system;
			stat_node->view[curcpu].idle += diff[curcpu].idle;

			user_sum += stat_node->view[curcpu].user;
			system_sum += stat_node->view[curcpu].system;
			idle_sum += stat_node->view[curcpu].idle;

			diff_user += diff[curcpu].user;
			diff_system += diff[curcpu].system;
			diff_idle += diff[curcpu].idle;
			if (diff[curcpu].idle > max_diff_idle) {
				max_diff_idle = diff[curcpu].idle;
				max_diff_idle_index = curcpu;
			}

			lxcfs_v("curcpu: %d, diff_user: %lu, diff_system: %lu, diff_idle: %lu\n", curcpu, diff[curcpu].user, diff[curcpu].system, diff[curcpu].idle);
		}
		lxcfs_v("total. diff_user: %lu, diff_system: %lu, diff_idle: %lu\n", diff_user, diff_system, diff_idle);

		/* revise cpu usage view to support partial cpu case. */
		exact_cpus = exact_cpu_count(cg);
		if (exact_cpus < (double)max_cpus){
			unsigned long delta = (unsigned long)((double)(diff_user + diff_system + diff_idle) * (1 - exact_cpus / (double)max_cpus));

			lxcfs_v("revising cpu usage view to match the exact cpu count [%f]\n", exact_cpus);
			lxcfs_v("delta: %lu\n", delta);
			lxcfs_v("idle_sum before: %lu\n", idle_sum);
			idle_sum = idle_sum > delta ? idle_sum - delta : 0;
			lxcfs_v("idle_sum after: %lu\n", idle_sum);

			curcpu = max_diff_idle_index;
			lxcfs_v("curcpu: %d, idle before: %lu\n", curcpu, stat_node->view[curcpu].idle);
			stat_node->view[curcpu].idle = stat_node->view[curcpu].idle > delta ? stat_node->view[curcpu].idle - delta : 0;
			lxcfs_v("curcpu: %d, idle after: %lu\n", curcpu, stat_node->view[curcpu].idle);
		}
	} else {
		for (curcpu = 0; curcpu < nprocs; curcpu++) {
			if (!stat_node->usage[curcpu].online)
				continue;

			stat_node->view[curcpu].user = stat_node->usage[curcpu].user;
			stat_node->view[curcpu].system = stat_node->usage[curcpu].system;
			stat_node->view[curcpu].idle = stat_node->usage[curcpu].idle;

			user_sum += stat_node->view[curcpu].user;
			system_sum += stat_node->view[curcpu].system;
			idle_sum += stat_node->view[curcpu].idle;
		}
	}

	/* Render the file */
	/* cpu-all */
	l = snprintf(buf, buf_size, "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n",
			user_sum,
			system_sum,
			idle_sum);
	lxcfs_v("cpu-all: %s\n", buf);

	if (l < 0) {
		perror("Error writing to cache");
		return 0;
	}
	if (l >= buf_size) {
		lxcfs_error("%s\n", "Internal error: truncated write to cache.");
		return 0;
	}

	buf += l;
	buf_size -= l;
	total_len += l;

	/* Render visible CPUs */
	for (curcpu = 0, i = -1; curcpu < nprocs; curcpu++) {
		if (!stat_node->usage[curcpu].online)
			continue;

		i++;

		if (max_cpus > 0 && i == max_cpus)
			break;

		l = snprintf(buf, buf_size, "cpu%d %lu 0 %lu %lu 0 0 0 0 0 0\n",
				i,
				stat_node->view[curcpu].user,
				stat_node->view[curcpu].system,
				stat_node->view[curcpu].idle);
		lxcfs_v("cpu: %s\n", buf);

		if (l < 0) {
			perror("Error writing to cache");
			return 0;

		}
		if (l >= buf_size) {
			lxcfs_error("%s\n", "Internal error: truncated write to cache.");
			return 0;
		}

		buf += l;
		buf_size -= l;
		total_len += l;
	}

	/* Pass the rest of /proc/stat, start with the last line read */
	l = snprintf(buf, buf_size, "%s", line);

	if (l < 0) {
		perror("Error writing to cache");
		return 0;

	}
	if (l >= buf_size) {
		lxcfs_error("%s\n", "Internal error: truncated write to cache.");
		return 0;
	}

	buf += l;
	buf_size -= l;
	total_len += l;

	/* Pass the rest of the host's /proc/stat */
	while (getline(&line, &linelen, f) != -1) {
		l = snprintf(buf, buf_size, "%s", line);
		if (l < 0) {
			perror("Error writing to cache");
			return 0;
		}
		if (l >= buf_size) {
			lxcfs_error("%s\n", "Internal error: truncated write to cache.");
			return 0;
		}
		buf += l;
		buf_size -= l;
		total_len += l;
	}

	if (stat_node)
		pthread_mutex_unlock(&stat_node->lock);
	return total_len;
}

/*
 * check whether this is a '^processor" line in /proc/cpuinfo
 */
static bool is_processor_line(const char *line)
{
	int cpu;

	if (sscanf(line, "processor       : %d", &cpu) == 1)
		return true;
	return false;
}

static bool cpuline_in_cpuset(const char *line, const char *cpuset)
{
	int cpu;

	if (sscanf(line, "processor       : %d", &cpu) != 1)
		return false;
	return cpu_in_cpuset(cpu, cpuset);
}

int proc_cpuinfo_read(char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	__do_free char *cg = NULL, *cpuset = NULL, *line = NULL;
	__do_fclose FILE *f = NULL;
	struct fuse_context *fc = fuse_get_context();
	struct file_info *d = INTTYPE_TO_PTR(fi->fh);
	size_t linelen = 0, total_len = 0;
	bool am_printing = false, firstline = true, is_s390x = false;
	int curcpu = -1, cpu, max_cpus = 0;
	bool use_view;
	char *cache = d->buf;
	size_t cache_size = d->buflen;

	if (offset){
		int left;

		if (offset > d->size)
			return -EINVAL;

		if (!d->cached)
			return 0;

		left = d->size - offset;
		total_len = left > size ? size: left;
		memcpy(buf, cache + offset, total_len);

		return total_len;
	}

	pid_t initpid = lookup_initpid_in_store(fc->pid);
	if (initpid <= 1 || is_shared_pidns(initpid))
		initpid = fc->pid;
	cg = get_pid_cgroup(initpid, "cpuset");
	if (!cg)
		return read_file_fuse("proc/cpuinfo", buf, size, d);
	prune_init_slice(cg);

	cpuset = get_cpuset(cg);
	if (!cpuset)
		return 0;

	use_view = cgroup_ops->can_use_cpuview(cgroup_ops);
	if (use_view)
		max_cpus = max_cpu_count(cg);

	f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return 0;

	while (getline(&line, &linelen, f) != -1) {
		ssize_t l;
		if (firstline) {
			firstline = false;
			if (strstr(line, "IBM/S390") != NULL) {
				is_s390x = true;
				am_printing = true;
				continue;
			}
		}
		if (strncmp(line, "# processors:", 12) == 0)
			continue;
		if (is_processor_line(line)) {
			if (use_view && max_cpus > 0 && (curcpu+1) == max_cpus)
				break;
			am_printing = cpuline_in_cpuset(line, cpuset);
			if (am_printing) {
				curcpu ++;
				l = snprintf(cache, cache_size, "processor	: %d\n", curcpu);
				if (l < 0) {
					perror("Error writing to cache");
					return 0;
				}
				if (l >= cache_size) {
					lxcfs_error("%s\n", "Internal error: truncated write to cache.");
					return 0;
				}
				cache += l;
				cache_size -= l;
				total_len += l;
			}
			continue;
		} else if (is_s390x && sscanf(line, "processor %d:", &cpu) == 1) {
			char *p;
			if (use_view && max_cpus > 0 && (curcpu+1) == max_cpus)
				break;
			if (!cpu_in_cpuset(cpu, cpuset))
				continue;
			curcpu ++;
			p = strchr(line, ':');
			if (!p || !*p)
				return 0;
			p++;
			l = snprintf(cache, cache_size, "processor %d:%s", curcpu, p);
			if (l < 0) {
				perror("Error writing to cache");
				return 0;
			}
			if (l >= cache_size) {
				lxcfs_error("%s\n", "Internal error: truncated write to cache.");
				return 0;
			}
			cache += l;
			cache_size -= l;
			total_len += l;
			continue;

		}
		if (am_printing) {
			l = snprintf(cache, cache_size, "%s", line);
			if (l < 0) {
				perror("Error writing to cache");
				return 0;
			}
			if (l >= cache_size) {
				lxcfs_error("%s\n", "Internal error: truncated write to cache.");
				return 0;
			}
			cache += l;
			cache_size -= l;
			total_len += l;
		}
	}

	if (is_s390x) {
		__do_free char *origcache = d->buf;
		ssize_t l;

		d->buf = malloc(d->buflen);
		if (!d->buf) {
			d->buf = move_ptr(origcache);
			return 0;
		}

		cache = d->buf;
		cache_size = d->buflen;
		total_len = 0;
		l = snprintf(cache, cache_size, "vendor_id       : IBM/S390\n");
		if (l < 0 || l >= cache_size)
			return 0;

		cache_size -= l;
		cache += l;
		total_len += l;
		l = snprintf(cache, cache_size, "# processors    : %d\n", curcpu + 1);
		if (l < 0 || l >= cache_size)
			return 0;

		cache_size -= l;
		cache += l;
		total_len += l;
		l = snprintf(cache, cache_size, "%s", origcache);
		if (l < 0 || l >= cache_size)
			return 0;
		total_len += l;
	}

	d->cached = 1;
	d->size = total_len;
	if (total_len > size ) total_len = size;

	/* read from off 0 */
	memcpy(buf, d->buf, total_len);
	return total_len;
}

/*
 * Returns 0 on success.
 * It is the caller's responsibility to free `return_usage`, unless this
 * function returns an error.
 */
int read_cpuacct_usage_all(char *cg, char *cpuset,
			   struct cpuacct_usage **return_usage, int *size)
{
	__do_free char *usage_str = NULL;
	__do_free struct cpuacct_usage *cpu_usage = NULL;
	int cpucount = get_nprocs_conf();
	int i = 0, j = 0, read_pos = 0, read_cnt = 0;
	int ret;
	int cg_cpu;
	uint64_t cg_user, cg_system;
	int64_t ticks_per_sec;

	ticks_per_sec = sysconf(_SC_CLK_TCK);

	if (ticks_per_sec < 0 && errno == EINVAL) {
		lxcfs_v(
			"%s\n",
			"read_cpuacct_usage_all failed to determine number of clock ticks "
			"in a second");
		return -1;
	}

	cpu_usage = malloc(sizeof(struct cpuacct_usage) * cpucount);
	if (!cpu_usage)
		return -ENOMEM;

	memset(cpu_usage, 0, sizeof(struct cpuacct_usage) * cpucount);
	if (!cgroup_ops->get(cgroup_ops, "cpuacct", cg, "cpuacct.usage_all", &usage_str)) {
		char *data = NULL;
		size_t sz = 0, asz = 0;

		/* read cpuacct.usage_percpu instead. */
		lxcfs_v("failed to read cpuacct.usage_all. reading cpuacct.usage_percpu instead\n%s", "");
		if (!cgroup_ops->get(cgroup_ops, "cpuacct", cg, "cpuacct.usage_percpu", &usage_str))
			return -1;
		lxcfs_v("usage_str: %s\n", usage_str);

		/* convert cpuacct.usage_percpu into cpuacct.usage_all. */
		lxcfs_v("converting cpuacct.usage_percpu into cpuacct.usage_all\n%s", "");

		must_strcat(&data, &sz, &asz, "cpu user system\n");

		while (sscanf(usage_str + read_pos, "%lu %n", &cg_user, &read_cnt) > 0) {
			lxcfs_debug("i: %d, cg_user: %lu, read_pos: %d, read_cnt: %d\n", i, cg_user, read_pos, read_cnt);
			must_strcat(&data, &sz, &asz, "%d %lu 0\n", i, cg_user);
			i++;
			read_pos += read_cnt;
		}

		usage_str = data;

		lxcfs_v("usage_str: %s\n", usage_str);
	}

	if (sscanf(usage_str, "cpu user system\n%n", &read_cnt) != 0) {
		lxcfs_error("read_cpuacct_usage_all reading first line from "
				"%s/cpuacct.usage_all failed.\n", cg);
		return -1;
	}

	read_pos += read_cnt;

	for (i = 0, j = 0; i < cpucount; i++) {
		ret = sscanf(usage_str + read_pos, "%d %lu %lu\n%n", &cg_cpu, &cg_user,
				&cg_system, &read_cnt);

		if (ret == EOF)
			break;

		if (ret != 3) {
			lxcfs_error("read_cpuacct_usage_all reading from %s/cpuacct.usage_all "
					"failed.\n", cg);
			return -1;
		}

		read_pos += read_cnt;

		/* Convert the time from nanoseconds to USER_HZ */
		cpu_usage[j].user = cg_user / 1000.0 / 1000 / 1000 * ticks_per_sec;
		cpu_usage[j].system = cg_system / 1000.0 / 1000 / 1000 * ticks_per_sec;
		j++;
	}

	*return_usage = move_ptr(cpu_usage);
	*size = cpucount;
	return 0;
}

static bool cpuview_init_head(struct cg_proc_stat_head **head)
{
	*head = malloc(sizeof(struct cg_proc_stat_head));
	if (!(*head)) {
		lxcfs_error("%s\n", strerror(errno));
		return false;
	}

	(*head)->lastcheck = time(NULL);
	(*head)->next = NULL;

	if (pthread_rwlock_init(&(*head)->lock, NULL) != 0) {
		lxcfs_error("%s\n", "Failed to initialize list lock");
		free_disarm(*head);
		return false;
	}

	return true;
}

bool init_cpuview(void)
{
	int i;

	for (i = 0; i < CPUVIEW_HASH_SIZE; i++)
		proc_stat_history[i] = NULL;

	for (i = 0; i < CPUVIEW_HASH_SIZE; i++) {
		if (!cpuview_init_head(&proc_stat_history[i]))
			goto err;
	}

	return true;

err:
	for (i = 0; i < CPUVIEW_HASH_SIZE; i++) {
		if (proc_stat_history[i])
			free_disarm(proc_stat_history[i]);
	}

	return false;
}

static void cpuview_free_head(struct cg_proc_stat_head *head)
{
	struct cg_proc_stat *node, *tmp;

	if (head->next) {
		node = head->next;

		for (;;) {
			tmp = node;
			node = node->next;
			free_proc_stat_node(tmp);

			if (!node)
				break;
		}
	}

	pthread_rwlock_destroy(&head->lock);
	free_disarm(head);
}

void free_cpuview(void)
{
	for (int i = 0; i < CPUVIEW_HASH_SIZE; i++)
		if (proc_stat_history[i])
			cpuview_free_head(proc_stat_history[i]);
}
