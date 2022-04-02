#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <linux/btrfs.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "ops.h"
#include "util.h"

static const int PROGRESS_INTERVAL = 5000;

struct thread_info {
	int fd;
	int efd;
	uint64_t bytes_used;
	struct btrfs_ioctl_scrub_args args;
	atomic_bool done;
	int ioctl_ret;
	int ioctl_errno;
	thrd_t thread;
};

struct cookie {
	int fd;
	int efd;
	struct thread_info *threads;
	size_t thread_count;
	size_t threads_started;
};

static int thread_proc(void *ti_raw) {
	struct thread_info *ti = ti_raw;
	ti->ioctl_ret = ioctl(ti->fd, BTRFS_IOC_SCRUB, &ti->args);
	ti->ioctl_errno = errno;
	atomic_store_explicit(&ti->done, true, memory_order_release);
	if(eventfd_write(ti->efd, 1) < 0) {
		perror("eventfd_write");
		abort();
	}
	return 0;
}

static bool start_thread(const struct btrfs_ioctl_fs_info_args *fs_info, const struct btrfs_ioctl_dev_info_args *dev_info, void *cookie_raw) {
	struct cookie *cookie = cookie_raw;

	if(!cookie->threads) {
		if(fs_info->num_devices > SIZE_MAX) {
			fprintf(stderr, "num_devices (%" PRIu64 ") > SIZE_MAX (%" PRIuMAX ")\n", (uint64_t) fs_info->num_devices, (uintmax_t) SIZE_MAX);
			return false;
		}
		cookie->thread_count = (size_t) fs_info->num_devices;
		cookie->threads = calloc(cookie->thread_count, sizeof(*cookie->threads));
		if(!cookie->threads) {
			perror("calloc");
			return false;
		}
	}

	if(cookie->threads_started == cookie->thread_count) {
		fprintf(stderr, "expected to find %zu devices but found another one\n", cookie->thread_count);
		return false;
	}

	struct thread_info *ti = &cookie->threads[cookie->threads_started];
	ti->fd = cookie->fd;
	ti->efd = cookie->efd;
	ti->bytes_used = dev_info->bytes_used;
	ti->args.devid = dev_info->devid;
	ti->args.end = (uint64_t) -1;
	atomic_init(&ti->done, false);
	int rc = thrd_create(&ti->thread, &thread_proc, ti);
	if(rc == thrd_nomem) {
		fprintf(stderr, "thrd_create: %s\n", strerror(ENOMEM));
		return false;
	} else if(rc == thrd_error) {
		fputs("thrd_create: failed\n", stderr);
		return false;
	} else if(rc != thrd_success) {
		fputs("thrd_create: unknown error\n", stderr);
		return false;
	}

	++cookie->threads_started;
	return true;
}

static bool do_scrub_fd_auxfds(const char *mountpoint, bool verbose, int fd, int sigfd, int efd) {
	struct cookie cookie = { .fd = fd, .efd = efd, };
	if(!for_each_device(mountpoint, fd, &start_thread, &cookie)) {
		// Forking a thread or allocating memory failed. A scrub might or might
		// not have gotten started. If one did, issue a cancel, join any
		// threads that were successfully forked, and free the array.
		if(cookie.threads) {
			ioctl(fd, BTRFS_IOC_SCRUB_CANCEL, (void *) 0);
			for(size_t i = 0; i != cookie.threads_started; ++i) {
				// Ignore errors; this is a best-effort cleanup attempt
				// when something else has already gone badly wrong.
				thrd_join(cookie.threads[i].thread, 0);
			}
			free(cookie.threads);
		}
		return false;
	}

	assert(cookie.threads_started == cookie.thread_count);

	// The threads are started. Loop until they’re all finished.
	size_t remaining = cookie.thread_count;
	bool cancelled = false;
	while(remaining) {
		struct pollfd pfds[] = {
			{ .fd = sigfd, .events = POLLIN, .revents = 0 },
			{ .fd = efd, .events = POLLIN, .revents = 0 },
		};
		if(poll(pfds, sizeof(pfds) / sizeof(*pfds), verbose ? PROGRESS_INTERVAL : -1) < 0) {
			perror("poll");
			break;
		}
		if(pfds[0].revents & POLLIN) {
			// A signal was received. Get out.
			cancelled = true;
			break;
		}
		if(pfds[1].revents & POLLIN) {
			// One or more threads notified us of completion.
			eventfd_t count;
			if(eventfd_read(efd, &count) < 0) {
				perror("eventfd_read");
				break;
			}
			assert(count <= remaining);
			remaining -= count;
		}
		if(verbose) {
			for(size_t i = 0; i != cookie.thread_count; ++i) {
				const struct thread_info *ti = &cookie.threads[i];
				if(i) {
					fputs("  ", stdout);
				}
				printf("[%" PRIu64 "]: ", (uint64_t) ti->args.devid);
				struct btrfs_ioctl_scrub_args args = { .devid = ti->args.devid, };
				const struct btrfs_ioctl_scrub_args *report_args;
				if(atomic_load_explicit(&ti->done, memory_order_acquire)) {
					report_args = &ti->args;
				} else {
					if(ioctl(fd, BTRFS_IOC_SCRUB_PROGRESS, &args) >= 0) {
						report_args = &args;
					} else {
						// This can happen if the scrub is finished but the
						// thread has not updated “done” yet.
						report_args = 0;
					}
				}

				if(report_args) {
					unsigned int permille;
					uint64_t bytes_scrubbed = report_args->progress.data_bytes_scrubbed + report_args->progress.tree_bytes_scrubbed;
					if(!ti->bytes_used) {
						permille = 500;
					} else if(bytes_scrubbed > ti->bytes_used) {
						permille = 1000;
					} else {
						permille = bytes_scrubbed * 1000 / ti->bytes_used;
					}

					uint64_t errors = report_args->progress.read_errors + report_args->progress.csum_errors + report_args->progress.verify_errors + report_args->progress.super_errors + report_args->progress.malloc_errors + report_args->progress.uncorrectable_errors + report_args->progress.corrected_errors + report_args->progress.unverified_errors;
					printf("%3u.%u%%: [%" PRIu64 " error(s)]", permille / 10, permille % 10, errors);
				} else {
					fputs("???                ", stdout);
				}
			}
			putchar('\r');
			fflush(stdout);
		}
	}

	// If any threads didn’t finish on their own, cancel the scrub.
	if(remaining) {
		ioctl(fd, BTRFS_IOC_SCRUB_CANCEL, (void *) 0);
	}

	// If we were displaying progress, print an empty line to avoid terminal
	// corruption.
	if(verbose) {
		putchar('\n');
	}

	// Join all the threads and present the results.
	bool ok = true;
	for(size_t i = 0; i != cookie.thread_count; ++i) {
		if(thrd_join(cookie.threads[i].thread, 0) == thrd_error) {
			fputs("thrd_join: error\n", stderr);
			abort();
		}

		if(cookie.threads[i].ioctl_ret >= 0) {
#define CHECK_ERROR(field_name, error_name) \
			do { \
				if(verbose || cookie.threads[i].args.progress.field_name) { \
					fprintf(cookie.threads[i].args.progress.field_name ? stderr : stdout, "%s: device ID %" PRIu64 ": scrub detected %" PRIu64 " " error_name " error(s)\n", mountpoint, (uint64_t) cookie.threads[i].args.devid, (uint64_t) cookie.threads[i].args.progress.field_name); \
					if(cookie.threads[i].args.progress.field_name) { \
						ok = false; \
					} \
				} \
			} while(0)
			CHECK_ERROR(read_errors, "read");
			CHECK_ERROR(csum_errors, "checksum");
			CHECK_ERROR(verify_errors, "verify");
			CHECK_ERROR(super_errors, "superblock");
			CHECK_ERROR(malloc_errors, "malloc");
			CHECK_ERROR(uncorrectable_errors, "uncorrectable");
			CHECK_ERROR(corrected_errors, "corrected");
			CHECK_ERROR(unverified_errors, "unverified");
#undef CHECK_ERROR
			if(verbose) {
				if(cookie.threads[i].args.progress.no_csum) {
					printf("%s: device ID %" PRIu64 ": scrub skipped %" PRIu64 " blocks without checksum\n", mountpoint, (uint64_t) cookie.threads[i].args.devid, (uint64_t) cookie.threads[i].args.progress.no_csum);
				}
				if(cookie.threads[i].args.progress.csum_discards) {
					printf("%s: device ID %" PRIu64 ": scrub ignored %" PRIu64 " checksums without data\n", mountpoint, (uint64_t) cookie.threads[i].args.devid, (uint64_t) cookie.threads[i].args.progress.csum_discards);
				}
			}
		} else if(!(cancelled && cookie.threads[i].ioctl_errno == ECANCELED)) {
			fprintf(stderr, "%s: device ID %" PRIu64 ": scrub failed: %s\n", mountpoint, (uint64_t) cookie.threads[i].args.devid, strerror(cookie.threads[i].ioctl_errno));
			ok = false;
		}
	}

	// Clean up.
	free(cookie.threads);
	return ok;
}

static bool do_scrub_fd(const char *mountpoint, bool verbose, int fd) {
	// The scrub ioctl is blocking and uninterruptible (in the traditional
	// signal-delivery sense) so just running it straight makes the process
	// unkillable (even with kill -9). However, BTRFS_IOC_SCRUB_CANCEL is
	// available, so make a signalfd to detect SIGINT, SIGQUIT, and SIGTERM and
	// cancel the in-progress scrubs. In order to accomplish that, the scrubs
	// run in separate threads and the main thread handles termination signals,
	// thread reaping, and progress display.
	//
	// A signalfd is used to take the termination signals. An eventfd is used
	// by the scrub threads to notify the main thread that they are finished.
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGQUIT);
	sigaddset(&sigs, SIGTERM);
	if(sigprocmask(SIG_BLOCK, &sigs, 0) < 0) {
		perror("sigprocmask");
		return false;
	}
	bool ret = true;
	int sigfd = signalfd(-1, &sigs, 0);
	if(sigfd >= 0) {
		int efd = eventfd(0, 0);
		if(efd >= 0) {
			ret = do_scrub_fd_auxfds(mountpoint, verbose, fd, sigfd, efd);
		} else {
			perror("eventfd");
			ret = false;
		}
	} else {
		perror("signalfd");
		ret = false;
	}
	if(sigprocmask(SIG_UNBLOCK, &sigs, 0) < 0) {
		perror("sigprocmask");
		abort();
	}
	return ret;
}

bool do_scrub(const char *mountpoint, bool verbose) {
	if(verbose) {
		printf("Scrub %s:\n", mountpoint);
	}
	int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);
	bool ret;
	if(fd >= 0) {
		ret = do_scrub_fd(mountpoint, verbose, fd);
		close(fd);
	} else {
		perror(mountpoint);
		ret = false;
	}
	return ret;
}
