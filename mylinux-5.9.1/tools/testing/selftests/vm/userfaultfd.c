// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stress userfaultfd syscall.
 *
 *  Copyright (C) 2015  Red Hat, Inc.
 *
 * This test allocates two virtual areas and bounces the physical
 * memory across the two virtual areas (from area_src to area_dst)
 * using userfaultfd.
 *
 * There are three threads running per CPU:
 *
 * 1) one per-CPU thread takes a per-page pthread_mutex in a random
 *    page of the area_dst (while the physical page may still be in
 *    area_src), and increments a per-page counter in the same page,
 *    and checks its value against a verification region.
 *
 * 2) another per-CPU thread handles the userfaults generated by
 *    thread 1 above. userfaultfd blocking reads or poll() modes are
 *    exercised interleaved.
 *
 * 3) one last per-CPU thread transfers the memory in the background
 *    at maximum bandwidth (if not already transferred by thread
 *    2). Each cpu thread takes cares of transferring a portion of the
 *    area.
 *
 * When all threads of type 3 completed the transfer, one bounce is
 * complete. area_src and area_dst are then swapped. All threads are
 * respawned and so the bounce is immediately restarted in the
 * opposite direction.
 *
 * per-CPU threads 1 by triggering userfaults inside
 * pthread_mutex_lock will also verify the atomicity of the memory
 * transfer (UFFDIO_COPY).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <pthread.h>
#include <linux/userfaultfd.h>
#include <setjmp.h>
#include <stdbool.h>
#include <assert.h>

#include "../kselftest.h"

#ifdef __NR_userfaultfd

static unsigned long nr_cpus, nr_pages, nr_pages_per_cpu, page_size;

#define BOUNCE_RANDOM		(1<<0)
#define BOUNCE_RACINGFAULTS	(1<<1)
#define BOUNCE_VERIFY		(1<<2)
#define BOUNCE_POLL		(1<<3)
static int bounces;

#define TEST_ANON	1
#define TEST_HUGETLB	2
#define TEST_SHMEM	3
static int test_type;

/* exercise the test_uffdio_*_eexist every ALARM_INTERVAL_SECS */
#define ALARM_INTERVAL_SECS 10
static volatile bool test_uffdio_copy_eexist = true;
static volatile bool test_uffdio_zeropage_eexist = true;
/* Whether to test uffd write-protection */
static bool test_uffdio_wp = false;

static bool map_shared;
static int huge_fd;
static char *huge_fd_off0;
static unsigned long long *count_verify;
static int uffd, uffd_flags, finished, *pipefd;
static char *area_src, *area_src_alias, *area_dst, *area_dst_alias;
static char *zeropage;
pthread_attr_t attr;

/* Userfaultfd test statistics */
struct uffd_stats {
	int cpu;
	unsigned long missing_faults;
	unsigned long wp_faults;
};

/* pthread_mutex_t starts at page offset 0 */
#define area_mutex(___area, ___nr)					\
	((pthread_mutex_t *) ((___area) + (___nr)*page_size))
/*
 * count is placed in the page after pthread_mutex_t naturally aligned
 * to avoid non alignment faults on non-x86 archs.
 */
#define area_count(___area, ___nr)					\
	((volatile unsigned long long *) ((unsigned long)		\
				 ((___area) + (___nr)*page_size +	\
				  sizeof(pthread_mutex_t) +		\
				  sizeof(unsigned long long) - 1) &	\
				 ~(unsigned long)(sizeof(unsigned long long) \
						  -  1)))

const char *examples =
    "# Run anonymous memory test on 100MiB region with 99999 bounces:\n"
    "./userfaultfd anon 100 99999\n\n"
    "# Run share memory test on 1GiB region with 99 bounces:\n"
    "./userfaultfd shmem 1000 99\n\n"
    "# Run hugetlb memory test on 256MiB region with 50 bounces (using /dev/hugepages/hugefile):\n"
    "./userfaultfd hugetlb 256 50 /dev/hugepages/hugefile\n\n"
    "# Run the same hugetlb test but using shmem:\n"
    "./userfaultfd hugetlb_shared 256 50 /dev/hugepages/hugefile\n\n"
    "# 10MiB-~6GiB 999 bounces anonymous test, "
    "continue forever unless an error triggers\n"
    "while ./userfaultfd anon $[RANDOM % 6000 + 10] 999; do true; done\n\n";

static void usage(void)
{
	fprintf(stderr, "\nUsage: ./userfaultfd <test type> <MiB> <bounces> "
		"[hugetlbfs_file]\n\n");
	fprintf(stderr, "Supported <test type>: anon, hugetlb, "
		"hugetlb_shared, shmem\n\n");
	fprintf(stderr, "Examples:\n\n");
	fprintf(stderr, "%s", examples);
	exit(1);
}

static void uffd_stats_reset(struct uffd_stats *uffd_stats,
			     unsigned long n_cpus)
{
	int i;

	for (i = 0; i < n_cpus; i++) {
		uffd_stats[i].cpu = i;
		uffd_stats[i].missing_faults = 0;
		uffd_stats[i].wp_faults = 0;
	}
}

static void uffd_stats_report(struct uffd_stats *stats, int n_cpus)
{
	int i;
	unsigned long long miss_total = 0, wp_total = 0;

	for (i = 0; i < n_cpus; i++) {
		miss_total += stats[i].missing_faults;
		wp_total += stats[i].wp_faults;
	}

	printf("userfaults: %llu missing (", miss_total);
	for (i = 0; i < n_cpus; i++)
		printf("%lu+", stats[i].missing_faults);
	printf("\b), %llu wp (", wp_total);
	for (i = 0; i < n_cpus; i++)
		printf("%lu+", stats[i].wp_faults);
	printf("\b)\n");
}

static int anon_release_pages(char *rel_area)
{
	int ret = 0;

	if (madvise(rel_area, nr_pages * page_size, MADV_DONTNEED)) {
		perror("madvise");
		ret = 1;
	}

	return ret;
}

static void anon_allocate_area(void **alloc_area)
{
	if (posix_memalign(alloc_area, page_size, nr_pages * page_size)) {
		fprintf(stderr, "out of memory\n");
		*alloc_area = NULL;
	}
}

static void noop_alias_mapping(__u64 *start, size_t len, unsigned long offset)
{
}

/* HugeTLB memory */
static int hugetlb_release_pages(char *rel_area)
{
	int ret = 0;

	if (fallocate(huge_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				rel_area == huge_fd_off0 ? 0 :
				nr_pages * page_size,
				nr_pages * page_size)) {
		perror("fallocate");
		ret = 1;
	}

	return ret;
}


static void hugetlb_allocate_area(void **alloc_area)
{
	void *area_alias = NULL;
	char **alloc_area_alias;
	*alloc_area = mmap(NULL, nr_pages * page_size, PROT_READ | PROT_WRITE,
			   (map_shared ? MAP_SHARED : MAP_PRIVATE) |
			   MAP_HUGETLB,
			   huge_fd, *alloc_area == area_src ? 0 :
			   nr_pages * page_size);
	if (*alloc_area == MAP_FAILED) {
		fprintf(stderr, "mmap of hugetlbfs file failed\n");
		*alloc_area = NULL;
	}

	if (map_shared) {
		area_alias = mmap(NULL, nr_pages * page_size, PROT_READ | PROT_WRITE,
				  MAP_SHARED | MAP_HUGETLB,
				  huge_fd, *alloc_area == area_src ? 0 :
				  nr_pages * page_size);
		if (area_alias == MAP_FAILED) {
			if (munmap(*alloc_area, nr_pages * page_size) < 0)
				perror("hugetlb munmap"), exit(1);
			*alloc_area = NULL;
			return;
		}
	}
	if (*alloc_area == area_src) {
		huge_fd_off0 = *alloc_area;
		alloc_area_alias = &area_src_alias;
	} else {
		alloc_area_alias = &area_dst_alias;
	}
	if (area_alias)
		*alloc_area_alias = area_alias;
}

static void hugetlb_alias_mapping(__u64 *start, size_t len, unsigned long offset)
{
	if (!map_shared)
		return;
	/*
	 * We can't zap just the pagetable with hugetlbfs because
	 * MADV_DONTEED won't work. So exercise -EEXIST on a alias
	 * mapping where the pagetables are not established initially,
	 * this way we'll exercise the -EEXEC at the fs level.
	 */
	*start = (unsigned long) area_dst_alias + offset;
}

/* Shared memory */
static int shmem_release_pages(char *rel_area)
{
	int ret = 0;

	if (madvise(rel_area, nr_pages * page_size, MADV_REMOVE)) {
		perror("madvise");
		ret = 1;
	}

	return ret;
}

static void shmem_allocate_area(void **alloc_area)
{
	*alloc_area = mmap(NULL, nr_pages * page_size, PROT_READ | PROT_WRITE,
			   MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (*alloc_area == MAP_FAILED) {
		fprintf(stderr, "shared memory mmap failed\n");
		*alloc_area = NULL;
	}
}

struct uffd_test_ops {
	unsigned long expected_ioctls;
	void (*allocate_area)(void **alloc_area);
	int (*release_pages)(char *rel_area);
	void (*alias_mapping)(__u64 *start, size_t len, unsigned long offset);
};

#define SHMEM_EXPECTED_IOCTLS		((1 << _UFFDIO_WAKE) | \
					 (1 << _UFFDIO_COPY) | \
					 (1 << _UFFDIO_ZEROPAGE))

#define ANON_EXPECTED_IOCTLS		((1 << _UFFDIO_WAKE) | \
					 (1 << _UFFDIO_COPY) | \
					 (1 << _UFFDIO_ZEROPAGE) | \
					 (1 << _UFFDIO_WRITEPROTECT))

static struct uffd_test_ops anon_uffd_test_ops = {
	.expected_ioctls = ANON_EXPECTED_IOCTLS,
	.allocate_area	= anon_allocate_area,
	.release_pages	= anon_release_pages,
	.alias_mapping = noop_alias_mapping,
};

static struct uffd_test_ops shmem_uffd_test_ops = {
	.expected_ioctls = SHMEM_EXPECTED_IOCTLS,
	.allocate_area	= shmem_allocate_area,
	.release_pages	= shmem_release_pages,
	.alias_mapping = noop_alias_mapping,
};

static struct uffd_test_ops hugetlb_uffd_test_ops = {
	.expected_ioctls = UFFD_API_RANGE_IOCTLS_BASIC,
	.allocate_area	= hugetlb_allocate_area,
	.release_pages	= hugetlb_release_pages,
	.alias_mapping = hugetlb_alias_mapping,
};

static struct uffd_test_ops *uffd_test_ops;

static int my_bcmp(char *str1, char *str2, size_t n)
{
	unsigned long i;
	for (i = 0; i < n; i++)
		if (str1[i] != str2[i])
			return 1;
	return 0;
}

static void wp_range(int ufd, __u64 start, __u64 len, bool wp)
{
	struct uffdio_writeprotect prms = { 0 };

	/* Write protection page faults */
	prms.range.start = start;
	prms.range.len = len;
	/* Undo write-protect, do wakeup after that */
	prms.mode = wp ? UFFDIO_WRITEPROTECT_MODE_WP : 0;

	if (ioctl(ufd, UFFDIO_WRITEPROTECT, &prms))
		fprintf(stderr, "clear WP failed for address 0x%Lx\n",
			start), exit(1);
}

static void *locking_thread(void *arg)
{
	unsigned long cpu = (unsigned long) arg;
	struct random_data rand;
	unsigned long page_nr = *(&(page_nr)); /* uninitialized warning */
	int32_t rand_nr;
	unsigned long long count;
	char randstate[64];
	unsigned int seed;
	time_t start;

	if (bounces & BOUNCE_RANDOM) {
		seed = (unsigned int) time(NULL) - bounces;
		if (!(bounces & BOUNCE_RACINGFAULTS))
			seed += cpu;
		bzero(&rand, sizeof(rand));
		bzero(&randstate, sizeof(randstate));
		if (initstate_r(seed, randstate, sizeof(randstate), &rand))
			fprintf(stderr, "srandom_r error\n"), exit(1);
	} else {
		page_nr = -bounces;
		if (!(bounces & BOUNCE_RACINGFAULTS))
			page_nr += cpu * nr_pages_per_cpu;
	}

	while (!finished) {
		if (bounces & BOUNCE_RANDOM) {
			if (random_r(&rand, &rand_nr))
				fprintf(stderr, "random_r 1 error\n"), exit(1);
			page_nr = rand_nr;
			if (sizeof(page_nr) > sizeof(rand_nr)) {
				if (random_r(&rand, &rand_nr))
					fprintf(stderr, "random_r 2 error\n"), exit(1);
				page_nr |= (((unsigned long) rand_nr) << 16) <<
					   16;
			}
		} else
			page_nr += 1;
		page_nr %= nr_pages;

		start = time(NULL);
		if (bounces & BOUNCE_VERIFY) {
			count = *area_count(area_dst, page_nr);
			if (!count)
				fprintf(stderr,
					"page_nr %lu wrong count %Lu %Lu\n",
					page_nr, count,
					count_verify[page_nr]), exit(1);


			/*
			 * We can't use bcmp (or memcmp) because that
			 * returns 0 erroneously if the memory is
			 * changing under it (even if the end of the
			 * page is never changing and always
			 * different).
			 */
#if 1
			if (!my_bcmp(area_dst + page_nr * page_size, zeropage,
				     page_size))
				fprintf(stderr,
					"my_bcmp page_nr %lu wrong count %Lu %Lu\n",
					page_nr, count,
					count_verify[page_nr]), exit(1);
#else
			unsigned long loops;

			loops = 0;
			/* uncomment the below line to test with mutex */
			/* pthread_mutex_lock(area_mutex(area_dst, page_nr)); */
			while (!bcmp(area_dst + page_nr * page_size, zeropage,
				     page_size)) {
				loops += 1;
				if (loops > 10)
					break;
			}
			/* uncomment below line to test with mutex */
			/* pthread_mutex_unlock(area_mutex(area_dst, page_nr)); */
			if (loops) {
				fprintf(stderr,
					"page_nr %lu all zero thread %lu %p %lu\n",
					page_nr, cpu, area_dst + page_nr * page_size,
					loops);
				if (loops > 10)
					exit(1);
			}
#endif
		}

		pthread_mutex_lock(area_mutex(area_dst, page_nr));
		count = *area_count(area_dst, page_nr);
		if (count != count_verify[page_nr]) {
			fprintf(stderr,
				"page_nr %lu memory corruption %Lu %Lu\n",
				page_nr, count,
				count_verify[page_nr]), exit(1);
		}
		count++;
		*area_count(area_dst, page_nr) = count_verify[page_nr] = count;
		pthread_mutex_unlock(area_mutex(area_dst, page_nr));

		if (time(NULL) - start > 1)
			fprintf(stderr,
				"userfault too slow %ld "
				"possible false positive with overcommit\n",
				time(NULL) - start);
	}

	return NULL;
}

static void retry_copy_page(int ufd, struct uffdio_copy *uffdio_copy,
			    unsigned long offset)
{
	uffd_test_ops->alias_mapping(&uffdio_copy->dst,
				     uffdio_copy->len,
				     offset);
	if (ioctl(ufd, UFFDIO_COPY, uffdio_copy)) {
		/* real retval in ufdio_copy.copy */
		if (uffdio_copy->copy != -EEXIST)
			fprintf(stderr, "UFFDIO_COPY retry error %Ld\n",
				uffdio_copy->copy), exit(1);
	} else {
		fprintf(stderr,	"UFFDIO_COPY retry unexpected %Ld\n",
			uffdio_copy->copy), exit(1);
	}
}

static int __copy_page(int ufd, unsigned long offset, bool retry)
{
	struct uffdio_copy uffdio_copy;

	if (offset >= nr_pages * page_size)
		fprintf(stderr, "unexpected offset %lu\n",
			offset), exit(1);
	uffdio_copy.dst = (unsigned long) area_dst + offset;
	uffdio_copy.src = (unsigned long) area_src + offset;
	uffdio_copy.len = page_size;
	if (test_uffdio_wp)
		uffdio_copy.mode = UFFDIO_COPY_MODE_WP;
	else
		uffdio_copy.mode = 0;
	uffdio_copy.copy = 0;
	if (ioctl(ufd, UFFDIO_COPY, &uffdio_copy)) {
		/* real retval in ufdio_copy.copy */
		if (uffdio_copy.copy != -EEXIST)
			fprintf(stderr, "UFFDIO_COPY error %Ld\n",
				uffdio_copy.copy), exit(1);
	} else if (uffdio_copy.copy != page_size) {
		fprintf(stderr, "UFFDIO_COPY unexpected copy %Ld\n",
			uffdio_copy.copy), exit(1);
	} else {
		if (test_uffdio_copy_eexist && retry) {
			test_uffdio_copy_eexist = false;
			retry_copy_page(ufd, &uffdio_copy, offset);
		}
		return 1;
	}
	return 0;
}

static int copy_page_retry(int ufd, unsigned long offset)
{
	return __copy_page(ufd, offset, true);
}

static int copy_page(int ufd, unsigned long offset)
{
	return __copy_page(ufd, offset, false);
}

static int uffd_read_msg(int ufd, struct uffd_msg *msg)
{
	int ret = read(uffd, msg, sizeof(*msg));

	if (ret != sizeof(*msg)) {
		if (ret < 0) {
			if (errno == EAGAIN)
				return 1;
			else
				perror("blocking read error"), exit(1);
		} else {
			fprintf(stderr, "short read\n"), exit(1);
		}
	}

	return 0;
}

static void uffd_handle_page_fault(struct uffd_msg *msg,
				   struct uffd_stats *stats)
{
	unsigned long offset;

	if (msg->event != UFFD_EVENT_PAGEFAULT)
		fprintf(stderr, "unexpected msg event %u\n",
			msg->event), exit(1);

	if (msg->arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
		wp_range(uffd, msg->arg.pagefault.address, page_size, false);
		stats->wp_faults++;
	} else {
		/* Missing page faults */
		if (bounces & BOUNCE_VERIFY &&
		    msg->arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE)
			fprintf(stderr, "unexpected write fault\n"), exit(1);

		offset = (char *)(unsigned long)msg->arg.pagefault.address - area_dst;
		offset &= ~(page_size-1);

		if (copy_page(uffd, offset))
			stats->missing_faults++;
	}
}

static void *uffd_poll_thread(void *arg)
{
	struct uffd_stats *stats = (struct uffd_stats *)arg;
	unsigned long cpu = stats->cpu;
	struct pollfd pollfd[2];
	struct uffd_msg msg;
	struct uffdio_register uffd_reg;
	int ret;
	char tmp_chr;

	pollfd[0].fd = uffd;
	pollfd[0].events = POLLIN;
	pollfd[1].fd = pipefd[cpu*2];
	pollfd[1].events = POLLIN;

	for (;;) {
		ret = poll(pollfd, 2, -1);
		if (!ret)
			fprintf(stderr, "poll error %d\n", ret), exit(1);
		if (ret < 0)
			perror("poll"), exit(1);
		if (pollfd[1].revents & POLLIN) {
			if (read(pollfd[1].fd, &tmp_chr, 1) != 1)
				fprintf(stderr, "read pipefd error\n"),
					exit(1);
			break;
		}
		if (!(pollfd[0].revents & POLLIN))
			fprintf(stderr, "pollfd[0].revents %d\n",
				pollfd[0].revents), exit(1);
		if (uffd_read_msg(uffd, &msg))
			continue;
		switch (msg.event) {
		default:
			fprintf(stderr, "unexpected msg event %u\n",
				msg.event), exit(1);
			break;
		case UFFD_EVENT_PAGEFAULT:
			uffd_handle_page_fault(&msg, stats);
			break;
		case UFFD_EVENT_FORK:
			close(uffd);
			uffd = msg.arg.fork.ufd;
			pollfd[0].fd = uffd;
			break;
		case UFFD_EVENT_REMOVE:
			uffd_reg.range.start = msg.arg.remove.start;
			uffd_reg.range.len = msg.arg.remove.end -
				msg.arg.remove.start;
			if (ioctl(uffd, UFFDIO_UNREGISTER, &uffd_reg.range))
				fprintf(stderr, "remove failure\n"), exit(1);
			break;
		case UFFD_EVENT_REMAP:
			area_dst = (char *)(unsigned long)msg.arg.remap.to;
			break;
		}
	}

	return NULL;
}

pthread_mutex_t uffd_read_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *uffd_read_thread(void *arg)
{
	struct uffd_stats *stats = (struct uffd_stats *)arg;
	struct uffd_msg msg;

	pthread_mutex_unlock(&uffd_read_mutex);
	/* from here cancellation is ok */

	for (;;) {
		if (uffd_read_msg(uffd, &msg))
			continue;
		uffd_handle_page_fault(&msg, stats);
	}

	return NULL;
}

static void *background_thread(void *arg)
{
	unsigned long cpu = (unsigned long) arg;
	unsigned long page_nr, start_nr, mid_nr, end_nr;

	start_nr = cpu * nr_pages_per_cpu;
	end_nr = (cpu+1) * nr_pages_per_cpu;
	mid_nr = (start_nr + end_nr) / 2;

	/* Copy the first half of the pages */
	for (page_nr = start_nr; page_nr < mid_nr; page_nr++)
		copy_page_retry(uffd, page_nr * page_size);

	/*
	 * If we need to test uffd-wp, set it up now.  Then we'll have
	 * at least the first half of the pages mapped already which
	 * can be write-protected for testing
	 */
	if (test_uffdio_wp)
		wp_range(uffd, (unsigned long)area_dst + start_nr * page_size,
			nr_pages_per_cpu * page_size, true);

	/*
	 * Continue the 2nd half of the page copying, handling write
	 * protection faults if any
	 */
	for (page_nr = mid_nr; page_nr < end_nr; page_nr++)
		copy_page_retry(uffd, page_nr * page_size);

	return NULL;
}

static int stress(struct uffd_stats *uffd_stats)
{
	unsigned long cpu;
	pthread_t locking_threads[nr_cpus];
	pthread_t uffd_threads[nr_cpus];
	pthread_t background_threads[nr_cpus];

	finished = 0;
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		if (pthread_create(&locking_threads[cpu], &attr,
				   locking_thread, (void *)cpu))
			return 1;
		if (bounces & BOUNCE_POLL) {
			if (pthread_create(&uffd_threads[cpu], &attr,
					   uffd_poll_thread,
					   (void *)&uffd_stats[cpu]))
				return 1;
		} else {
			if (pthread_create(&uffd_threads[cpu], &attr,
					   uffd_read_thread,
					   (void *)&uffd_stats[cpu]))
				return 1;
			pthread_mutex_lock(&uffd_read_mutex);
		}
		if (pthread_create(&background_threads[cpu], &attr,
				   background_thread, (void *)cpu))
			return 1;
	}
	for (cpu = 0; cpu < nr_cpus; cpu++)
		if (pthread_join(background_threads[cpu], NULL))
			return 1;

	/*
	 * Be strict and immediately zap area_src, the whole area has
	 * been transferred already by the background treads. The
	 * area_src could then be faulted in in a racy way by still
	 * running uffdio_threads reading zeropages after we zapped
	 * area_src (but they're guaranteed to get -EEXIST from
	 * UFFDIO_COPY without writing zero pages into area_dst
	 * because the background threads already completed).
	 */
	if (uffd_test_ops->release_pages(area_src))
		return 1;


	finished = 1;
	for (cpu = 0; cpu < nr_cpus; cpu++)
		if (pthread_join(locking_threads[cpu], NULL))
			return 1;

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		char c;
		if (bounces & BOUNCE_POLL) {
			if (write(pipefd[cpu*2+1], &c, 1) != 1) {
				fprintf(stderr, "pipefd write error\n");
				return 1;
			}
			if (pthread_join(uffd_threads[cpu],
					 (void *)&uffd_stats[cpu]))
				return 1;
		} else {
			if (pthread_cancel(uffd_threads[cpu]))
				return 1;
			if (pthread_join(uffd_threads[cpu], NULL))
				return 1;
		}
	}

	return 0;
}

static int userfaultfd_open(int features)
{
	struct uffdio_api uffdio_api;

	uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
	if (uffd < 0) {
		fprintf(stderr,
			"userfaultfd syscall not available in this kernel\n");
		return 1;
	}
	uffd_flags = fcntl(uffd, F_GETFD, NULL);

	uffdio_api.api = UFFD_API;
	uffdio_api.features = features;
	if (ioctl(uffd, UFFDIO_API, &uffdio_api)) {
		fprintf(stderr, "UFFDIO_API\n");
		return 1;
	}
	if (uffdio_api.api != UFFD_API) {
		fprintf(stderr, "UFFDIO_API error %Lu\n", uffdio_api.api);
		return 1;
	}

	return 0;
}

sigjmp_buf jbuf, *sigbuf;

static void sighndl(int sig, siginfo_t *siginfo, void *ptr)
{
	if (sig == SIGBUS) {
		if (sigbuf)
			siglongjmp(*sigbuf, 1);
		abort();
	}
}

/*
 * For non-cooperative userfaultfd test we fork() a process that will
 * generate pagefaults, will mremap the area monitored by the
 * userfaultfd and at last this process will release the monitored
 * area.
 * For the anonymous and shared memory the area is divided into two
 * parts, the first part is accessed before mremap, and the second
 * part is accessed after mremap. Since hugetlbfs does not support
 * mremap, the entire monitored area is accessed in a single pass for
 * HUGETLB_TEST.
 * The release of the pages currently generates event for shmem and
 * anonymous memory (UFFD_EVENT_REMOVE), hence it is not checked
 * for hugetlb.
 * For signal test(UFFD_FEATURE_SIGBUS), signal_test = 1, we register
 * monitored area, generate pagefaults and test that signal is delivered.
 * Use UFFDIO_COPY to allocate missing page and retry. For signal_test = 2
 * test robustness use case - we release monitored area, fork a process
 * that will generate pagefaults and verify signal is generated.
 * This also tests UFFD_FEATURE_EVENT_FORK event along with the signal
 * feature. Using monitor thread, verify no userfault events are generated.
 */
static int faulting_process(int signal_test)
{
	unsigned long nr;
	unsigned long long count;
	unsigned long split_nr_pages;
	unsigned long lastnr;
	struct sigaction act;
	unsigned long signalled = 0;

	if (test_type != TEST_HUGETLB)
		split_nr_pages = (nr_pages + 1) / 2;
	else
		split_nr_pages = nr_pages;

	if (signal_test) {
		sigbuf = &jbuf;
		memset(&act, 0, sizeof(act));
		act.sa_sigaction = sighndl;
		act.sa_flags = SA_SIGINFO;
		if (sigaction(SIGBUS, &act, 0)) {
			perror("sigaction");
			return 1;
		}
		lastnr = (unsigned long)-1;
	}

	for (nr = 0; nr < split_nr_pages; nr++) {
		int steps = 1;
		unsigned long offset = nr * page_size;

		if (signal_test) {
			if (sigsetjmp(*sigbuf, 1) != 0) {
				if (steps == 1 && nr == lastnr) {
					fprintf(stderr, "Signal repeated\n");
					return 1;
				}

				lastnr = nr;
				if (signal_test == 1) {
					if (steps == 1) {
						/* This is a MISSING request */
						steps++;
						if (copy_page(uffd, offset))
							signalled++;
					} else {
						/* This is a WP request */
						assert(steps == 2);
						wp_range(uffd,
							 (__u64)area_dst +
							 offset,
							 page_size, false);
					}
				} else {
					signalled++;
					continue;
				}
			}
		}

		count = *area_count(area_dst, nr);
		if (count != count_verify[nr]) {
			fprintf(stderr,
				"nr %lu memory corruption %Lu %Lu\n",
				nr, count,
				count_verify[nr]);
	        }
		/*
		 * Trigger write protection if there is by writting
		 * the same value back.
		 */
		*area_count(area_dst, nr) = count;
	}

	if (signal_test)
		return signalled != split_nr_pages;

	if (test_type == TEST_HUGETLB)
		return 0;

	area_dst = mremap(area_dst, nr_pages * page_size,  nr_pages * page_size,
			  MREMAP_MAYMOVE | MREMAP_FIXED, area_src);
	if (area_dst == MAP_FAILED)
		perror("mremap"), exit(1);

	for (; nr < nr_pages; nr++) {
		count = *area_count(area_dst, nr);
		if (count != count_verify[nr]) {
			fprintf(stderr,
				"nr %lu memory corruption %Lu %Lu\n",
				nr, count,
				count_verify[nr]), exit(1);
		}
		/*
		 * Trigger write protection if there is by writting
		 * the same value back.
		 */
		*area_count(area_dst, nr) = count;
	}

	if (uffd_test_ops->release_pages(area_dst))
		return 1;

	for (nr = 0; nr < nr_pages; nr++) {
		if (my_bcmp(area_dst + nr * page_size, zeropage, page_size))
			fprintf(stderr, "nr %lu is not zero\n", nr), exit(1);
	}

	return 0;
}

static void retry_uffdio_zeropage(int ufd,
				  struct uffdio_zeropage *uffdio_zeropage,
				  unsigned long offset)
{
	uffd_test_ops->alias_mapping(&uffdio_zeropage->range.start,
				     uffdio_zeropage->range.len,
				     offset);
	if (ioctl(ufd, UFFDIO_ZEROPAGE, uffdio_zeropage)) {
		if (uffdio_zeropage->zeropage != -EEXIST)
			fprintf(stderr, "UFFDIO_ZEROPAGE retry error %Ld\n",
				uffdio_zeropage->zeropage), exit(1);
	} else {
		fprintf(stderr, "UFFDIO_ZEROPAGE retry unexpected %Ld\n",
			uffdio_zeropage->zeropage), exit(1);
	}
}

static int __uffdio_zeropage(int ufd, unsigned long offset, bool retry)
{
	struct uffdio_zeropage uffdio_zeropage;
	int ret;
	unsigned long has_zeropage;

	has_zeropage = uffd_test_ops->expected_ioctls & (1 << _UFFDIO_ZEROPAGE);

	if (offset >= nr_pages * page_size)
		fprintf(stderr, "unexpected offset %lu\n",
			offset), exit(1);
	uffdio_zeropage.range.start = (unsigned long) area_dst + offset;
	uffdio_zeropage.range.len = page_size;
	uffdio_zeropage.mode = 0;
	ret = ioctl(ufd, UFFDIO_ZEROPAGE, &uffdio_zeropage);
	if (ret) {
		/* real retval in ufdio_zeropage.zeropage */
		if (has_zeropage) {
			if (uffdio_zeropage.zeropage == -EEXIST)
				fprintf(stderr, "UFFDIO_ZEROPAGE -EEXIST\n"),
					exit(1);
			else
				fprintf(stderr, "UFFDIO_ZEROPAGE error %Ld\n",
					uffdio_zeropage.zeropage), exit(1);
		} else {
			if (uffdio_zeropage.zeropage != -EINVAL)
				fprintf(stderr,
					"UFFDIO_ZEROPAGE not -EINVAL %Ld\n",
					uffdio_zeropage.zeropage), exit(1);
		}
	} else if (has_zeropage) {
		if (uffdio_zeropage.zeropage != page_size) {
			fprintf(stderr, "UFFDIO_ZEROPAGE unexpected %Ld\n",
				uffdio_zeropage.zeropage), exit(1);
		} else {
			if (test_uffdio_zeropage_eexist && retry) {
				test_uffdio_zeropage_eexist = false;
				retry_uffdio_zeropage(ufd, &uffdio_zeropage,
						      offset);
			}
			return 1;
		}
	} else {
		fprintf(stderr,
			"UFFDIO_ZEROPAGE succeeded %Ld\n",
			uffdio_zeropage.zeropage), exit(1);
	}

	return 0;
}

static int uffdio_zeropage(int ufd, unsigned long offset)
{
	return __uffdio_zeropage(ufd, offset, false);
}

/* exercise UFFDIO_ZEROPAGE */
static int userfaultfd_zeropage_test(void)
{
	struct uffdio_register uffdio_register;
	unsigned long expected_ioctls;

	printf("testing UFFDIO_ZEROPAGE: ");
	fflush(stdout);

	if (uffd_test_ops->release_pages(area_dst))
		return 1;

	if (userfaultfd_open(0) < 0)
		return 1;
	uffdio_register.range.start = (unsigned long) area_dst;
	uffdio_register.range.len = nr_pages * page_size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (test_uffdio_wp)
		uffdio_register.mode |= UFFDIO_REGISTER_MODE_WP;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register))
		fprintf(stderr, "register failure\n"), exit(1);

	expected_ioctls = uffd_test_ops->expected_ioctls;
	if ((uffdio_register.ioctls & expected_ioctls) !=
	    expected_ioctls)
		fprintf(stderr,
			"unexpected missing ioctl for anon memory\n"),
			exit(1);

	if (uffdio_zeropage(uffd, 0)) {
		if (my_bcmp(area_dst, zeropage, page_size))
			fprintf(stderr, "zeropage is not zero\n"), exit(1);
	}

	close(uffd);
	printf("done.\n");
	return 0;
}

static int userfaultfd_events_test(void)
{
	struct uffdio_register uffdio_register;
	unsigned long expected_ioctls;
	pthread_t uffd_mon;
	int err, features;
	pid_t pid;
	char c;
	struct uffd_stats stats = { 0 };

	printf("testing events (fork, remap, remove): ");
	fflush(stdout);

	if (uffd_test_ops->release_pages(area_dst))
		return 1;

	features = UFFD_FEATURE_EVENT_FORK | UFFD_FEATURE_EVENT_REMAP |
		UFFD_FEATURE_EVENT_REMOVE;
	if (userfaultfd_open(features) < 0)
		return 1;
	fcntl(uffd, F_SETFL, uffd_flags | O_NONBLOCK);

	uffdio_register.range.start = (unsigned long) area_dst;
	uffdio_register.range.len = nr_pages * page_size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (test_uffdio_wp)
		uffdio_register.mode |= UFFDIO_REGISTER_MODE_WP;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register))
		fprintf(stderr, "register failure\n"), exit(1);

	expected_ioctls = uffd_test_ops->expected_ioctls;
	if ((uffdio_register.ioctls & expected_ioctls) !=
	    expected_ioctls)
		fprintf(stderr,
			"unexpected missing ioctl for anon memory\n"),
			exit(1);

	if (pthread_create(&uffd_mon, &attr, uffd_poll_thread, &stats))
		perror("uffd_poll_thread create"), exit(1);

	pid = fork();
	if (pid < 0)
		perror("fork"), exit(1);

	if (!pid)
		return faulting_process(0);

	waitpid(pid, &err, 0);
	if (err)
		fprintf(stderr, "faulting process failed\n"), exit(1);

	if (write(pipefd[1], &c, sizeof(c)) != sizeof(c))
		perror("pipe write"), exit(1);
	if (pthread_join(uffd_mon, NULL))
		return 1;

	close(uffd);

	uffd_stats_report(&stats, 1);

	return stats.missing_faults != nr_pages;
}

static int userfaultfd_sig_test(void)
{
	struct uffdio_register uffdio_register;
	unsigned long expected_ioctls;
	unsigned long userfaults;
	pthread_t uffd_mon;
	int err, features;
	pid_t pid;
	char c;
	struct uffd_stats stats = { 0 };

	printf("testing signal delivery: ");
	fflush(stdout);

	if (uffd_test_ops->release_pages(area_dst))
		return 1;

	features = UFFD_FEATURE_EVENT_FORK|UFFD_FEATURE_SIGBUS;
	if (userfaultfd_open(features) < 0)
		return 1;
	fcntl(uffd, F_SETFL, uffd_flags | O_NONBLOCK);

	uffdio_register.range.start = (unsigned long) area_dst;
	uffdio_register.range.len = nr_pages * page_size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
	if (test_uffdio_wp)
		uffdio_register.mode |= UFFDIO_REGISTER_MODE_WP;
	if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register))
		fprintf(stderr, "register failure\n"), exit(1);

	expected_ioctls = uffd_test_ops->expected_ioctls;
	if ((uffdio_register.ioctls & expected_ioctls) !=
	    expected_ioctls)
		fprintf(stderr,
			"unexpected missing ioctl for anon memory\n"),
			exit(1);

	if (faulting_process(1))
		fprintf(stderr, "faulting process failed\n"), exit(1);

	if (uffd_test_ops->release_pages(area_dst))
		return 1;

	if (pthread_create(&uffd_mon, &attr, uffd_poll_thread, &stats))
		perror("uffd_poll_thread create"), exit(1);

	pid = fork();
	if (pid < 0)
		perror("fork"), exit(1);

	if (!pid)
		exit(faulting_process(2));

	waitpid(pid, &err, 0);
	if (err)
		fprintf(stderr, "faulting process failed\n"), exit(1);

	if (write(pipefd[1], &c, sizeof(c)) != sizeof(c))
		perror("pipe write"), exit(1);
	if (pthread_join(uffd_mon, (void **)&userfaults))
		return 1;

	printf("done.\n");
	if (userfaults)
		fprintf(stderr, "Signal test failed, userfaults: %ld\n",
			userfaults);
	close(uffd);
	return userfaults != 0;
}

static int userfaultfd_stress(void)
{
	void *area;
	char *tmp_area;
	unsigned long nr;
	struct uffdio_register uffdio_register;
	unsigned long cpu;
	int err;
	struct uffd_stats uffd_stats[nr_cpus];

	uffd_test_ops->allocate_area((void **)&area_src);
	if (!area_src)
		return 1;
	uffd_test_ops->allocate_area((void **)&area_dst);
	if (!area_dst)
		return 1;

	if (userfaultfd_open(0) < 0)
		return 1;

	count_verify = malloc(nr_pages * sizeof(unsigned long long));
	if (!count_verify) {
		perror("count_verify");
		return 1;
	}

	for (nr = 0; nr < nr_pages; nr++) {
		*area_mutex(area_src, nr) = (pthread_mutex_t)
			PTHREAD_MUTEX_INITIALIZER;
		count_verify[nr] = *area_count(area_src, nr) = 1;
		/*
		 * In the transition between 255 to 256, powerpc will
		 * read out of order in my_bcmp and see both bytes as
		 * zero, so leave a placeholder below always non-zero
		 * after the count, to avoid my_bcmp to trigger false
		 * positives.
		 */
		*(area_count(area_src, nr) + 1) = 1;
	}

	pipefd = malloc(sizeof(int) * nr_cpus * 2);
	if (!pipefd) {
		perror("pipefd");
		return 1;
	}
	for (cpu = 0; cpu < nr_cpus; cpu++) {
		if (pipe2(&pipefd[cpu*2], O_CLOEXEC | O_NONBLOCK)) {
			perror("pipe");
			return 1;
		}
	}

	if (posix_memalign(&area, page_size, page_size)) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	zeropage = area;
	bzero(zeropage, page_size);

	pthread_mutex_lock(&uffd_read_mutex);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 16*1024*1024);

	err = 0;
	while (bounces--) {
		unsigned long expected_ioctls;

		printf("bounces: %d, mode:", bounces);
		if (bounces & BOUNCE_RANDOM)
			printf(" rnd");
		if (bounces & BOUNCE_RACINGFAULTS)
			printf(" racing");
		if (bounces & BOUNCE_VERIFY)
			printf(" ver");
		if (bounces & BOUNCE_POLL)
			printf(" poll");
		printf(", ");
		fflush(stdout);

		if (bounces & BOUNCE_POLL)
			fcntl(uffd, F_SETFL, uffd_flags | O_NONBLOCK);
		else
			fcntl(uffd, F_SETFL, uffd_flags & ~O_NONBLOCK);

		/* register */
		uffdio_register.range.start = (unsigned long) area_dst;
		uffdio_register.range.len = nr_pages * page_size;
		uffdio_register.mode = UFFDIO_REGISTER_MODE_MISSING;
		if (test_uffdio_wp)
			uffdio_register.mode |= UFFDIO_REGISTER_MODE_WP;
		if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register)) {
			fprintf(stderr, "register failure\n");
			return 1;
		}
		expected_ioctls = uffd_test_ops->expected_ioctls;
		if ((uffdio_register.ioctls & expected_ioctls) !=
		    expected_ioctls) {
			fprintf(stderr,
				"unexpected missing ioctl for anon memory\n");
			return 1;
		}

		if (area_dst_alias) {
			uffdio_register.range.start = (unsigned long)
				area_dst_alias;
			if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register)) {
				fprintf(stderr, "register failure alias\n");
				return 1;
			}
		}

		/*
		 * The madvise done previously isn't enough: some
		 * uffd_thread could have read userfaults (one of
		 * those already resolved by the background thread)
		 * and it may be in the process of calling
		 * UFFDIO_COPY. UFFDIO_COPY will read the zapped
		 * area_src and it would map a zero page in it (of
		 * course such a UFFDIO_COPY is perfectly safe as it'd
		 * return -EEXIST). The problem comes at the next
		 * bounce though: that racing UFFDIO_COPY would
		 * generate zeropages in the area_src, so invalidating
		 * the previous MADV_DONTNEED. Without this additional
		 * MADV_DONTNEED those zeropages leftovers in the
		 * area_src would lead to -EEXIST failure during the
		 * next bounce, effectively leaving a zeropage in the
		 * area_dst.
		 *
		 * Try to comment this out madvise to see the memory
		 * corruption being caught pretty quick.
		 *
		 * khugepaged is also inhibited to collapse THP after
		 * MADV_DONTNEED only after the UFFDIO_REGISTER, so it's
		 * required to MADV_DONTNEED here.
		 */
		if (uffd_test_ops->release_pages(area_dst))
			return 1;

		uffd_stats_reset(uffd_stats, nr_cpus);

		/* bounce pass */
		if (stress(uffd_stats))
			return 1;

		/* Clear all the write protections if there is any */
		if (test_uffdio_wp)
			wp_range(uffd, (unsigned long)area_dst,
				 nr_pages * page_size, false);

		/* unregister */
		if (ioctl(uffd, UFFDIO_UNREGISTER, &uffdio_register.range)) {
			fprintf(stderr, "unregister failure\n");
			return 1;
		}
		if (area_dst_alias) {
			uffdio_register.range.start = (unsigned long) area_dst;
			if (ioctl(uffd, UFFDIO_UNREGISTER,
				  &uffdio_register.range)) {
				fprintf(stderr, "unregister failure alias\n");
				return 1;
			}
		}

		/* verification */
		if (bounces & BOUNCE_VERIFY) {
			for (nr = 0; nr < nr_pages; nr++) {
				if (*area_count(area_dst, nr) != count_verify[nr]) {
					fprintf(stderr,
						"error area_count %Lu %Lu %lu\n",
						*area_count(area_src, nr),
						count_verify[nr],
						nr);
					err = 1;
					bounces = 0;
				}
			}
		}

		/* prepare next bounce */
		tmp_area = area_src;
		area_src = area_dst;
		area_dst = tmp_area;

		tmp_area = area_src_alias;
		area_src_alias = area_dst_alias;
		area_dst_alias = tmp_area;

		uffd_stats_report(uffd_stats, nr_cpus);
	}

	if (err)
		return err;

	close(uffd);
	return userfaultfd_zeropage_test() || userfaultfd_sig_test()
		|| userfaultfd_events_test();
}

/*
 * Copied from mlock2-tests.c
 */
unsigned long default_huge_page_size(void)
{
	unsigned long hps = 0;
	char *line = NULL;
	size_t linelen = 0;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		return 0;
	while (getline(&line, &linelen, f) > 0) {
		if (sscanf(line, "Hugepagesize:       %lu kB", &hps) == 1) {
			hps <<= 10;
			break;
		}
	}

	free(line);
	fclose(f);
	return hps;
}

static void set_test_type(const char *type)
{
	if (!strcmp(type, "anon")) {
		test_type = TEST_ANON;
		uffd_test_ops = &anon_uffd_test_ops;
		/* Only enable write-protect test for anonymous test */
		test_uffdio_wp = true;
	} else if (!strcmp(type, "hugetlb")) {
		test_type = TEST_HUGETLB;
		uffd_test_ops = &hugetlb_uffd_test_ops;
	} else if (!strcmp(type, "hugetlb_shared")) {
		map_shared = true;
		test_type = TEST_HUGETLB;
		uffd_test_ops = &hugetlb_uffd_test_ops;
	} else if (!strcmp(type, "shmem")) {
		map_shared = true;
		test_type = TEST_SHMEM;
		uffd_test_ops = &shmem_uffd_test_ops;
	} else {
		fprintf(stderr, "Unknown test type: %s\n", type), exit(1);
	}

	if (test_type == TEST_HUGETLB)
		page_size = default_huge_page_size();
	else
		page_size = sysconf(_SC_PAGE_SIZE);

	if (!page_size)
		fprintf(stderr, "Unable to determine page size\n"),
				exit(2);
	if ((unsigned long) area_count(NULL, 0) + sizeof(unsigned long long) * 2
	    > page_size)
		fprintf(stderr, "Impossible to run this test\n"), exit(2);
}

static void sigalrm(int sig)
{
	if (sig != SIGALRM)
		abort();
	test_uffdio_copy_eexist = true;
	test_uffdio_zeropage_eexist = true;
	alarm(ALARM_INTERVAL_SECS);
}

int main(int argc, char **argv)
{
	if (argc < 4)
		usage();

	if (signal(SIGALRM, sigalrm) == SIG_ERR)
		fprintf(stderr, "failed to arm SIGALRM"), exit(1);
	alarm(ALARM_INTERVAL_SECS);

	set_test_type(argv[1]);

	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	nr_pages_per_cpu = atol(argv[2]) * 1024*1024 / page_size /
		nr_cpus;
	if (!nr_pages_per_cpu) {
		fprintf(stderr, "invalid MiB\n");
		usage();
	}

	bounces = atoi(argv[3]);
	if (bounces <= 0) {
		fprintf(stderr, "invalid bounces\n");
		usage();
	}
	nr_pages = nr_pages_per_cpu * nr_cpus;

	if (test_type == TEST_HUGETLB) {
		if (argc < 5)
			usage();
		huge_fd = open(argv[4], O_CREAT | O_RDWR, 0755);
		if (huge_fd < 0) {
			fprintf(stderr, "Open of %s failed", argv[3]);
			perror("open");
			exit(1);
		}
		if (ftruncate(huge_fd, 0)) {
			fprintf(stderr, "ftruncate %s to size 0 failed", argv[3]);
			perror("ftruncate");
			exit(1);
		}
	}
	printf("nr_pages: %lu, nr_pages_per_cpu: %lu\n",
	       nr_pages, nr_pages_per_cpu);
	return userfaultfd_stress();
}

#else /* __NR_userfaultfd */

#warning "missing __NR_userfaultfd definition"

int main(void)
{
	printf("skip: Skipping userfaultfd test (missing __NR_userfaultfd)\n");
	return KSFT_SKIP;
}

#endif /* __NR_userfaultfd */
