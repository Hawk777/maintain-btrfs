#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/btrfs.h>
#include <linux/magic.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include "ops.h"

#define CHUNK_CAPACITY 8

static const uint32_t EXTENT_THRESHOLD = 32 * 1024 * 1024;

static const unsigned int MILLISECONDS_PER_PROGRESS = 250;

struct stack_entry {
	uint32_t dev_major, dev_minor;
	uint64_t inode;
	char *name;
	DIR *dir_handle;
};

struct stack_chunk {
	struct stack_entry entries[CHUNK_CAPACITY];
	struct stack_chunk *previous;
};

struct stack {
	struct stack_chunk *top;
	size_t top_used;
	struct stack_chunk *free_chunks;
};

static void stack_init(struct stack *stack) {
	stack->top = 0;
	stack->top_used = 0;
	stack->free_chunks = 0;
}

static bool stack_empty(const struct stack *stack) {
	return !stack->top;
}

static bool stack_push(struct stack *stack, const struct stack_entry *new) {
	if(!stack->top || stack->top_used == CHUNK_CAPACITY) {
		if(!stack->free_chunks) {
			struct stack_chunk *new_chunk = malloc(sizeof(*new_chunk));
			if(!new_chunk) {
				perror("malloc");
				return false;
			}
			new_chunk->previous = 0;
			stack->free_chunks = new_chunk;
		}
		struct stack_chunk *new_chunk = stack->free_chunks;
		stack->free_chunks = new_chunk->previous;
		new_chunk->previous = stack->top;
		stack->top = new_chunk;
		stack->top_used = 0;
	}

	stack->top->entries[stack->top_used] = *new;
	++stack->top_used;
	return true;
}

static void stack_pop(struct stack *stack) {
	struct stack_entry *e = &stack->top->entries[stack->top_used - 1];
	if(e->dir_handle) {
		closedir(e->dir_handle);
	}
	free(e->name);
	--stack->top_used;
	if(!stack->top_used) {
		struct stack_chunk *empty_chunk = stack->top;
		stack->top = empty_chunk->previous;
		empty_chunk->previous = stack->free_chunks;
		stack->free_chunks = empty_chunk;
		if(stack->top) {
			stack->top_used = CHUNK_CAPACITY;
		}
	}
}

static void stack_deinit(struct stack *stack) {
	while(!stack_empty(stack)) {
		stack_pop(stack);
	}
	while(stack->free_chunks) {
		struct stack_chunk *prev = stack->free_chunks->previous;
		free(stack->free_chunks);
		stack->free_chunks = prev;
	}
}

static struct stack_entry *stack_peek(struct stack *stack) {
	return &stack->top->entries[stack->top_used - 1];
}

static void stack_foreach_down(struct stack *stack, bool (*cb)(struct stack_entry *, void *), void *cookie) {
	struct stack_chunk *chunk = stack->top;
	size_t next_index = stack->top_used - 1;
	while(chunk) {
		if(!cb(&chunk->entries[next_index], cookie)) {
			break;
		}
		if(next_index) {
			--next_index;
		} else {
			chunk = chunk->previous;
			next_index = CHUNK_CAPACITY - 1;
		}
	}
}

static void stack_foreach_up(struct stack *stack, bool (*cb)(struct stack_entry *, void *), void *cookie) {
	struct stack_chunk *chunk = stack->top;
	while(chunk && chunk->previous) {
		chunk = chunk->previous;
	}
	size_t next_index = 0;
	while(chunk) {
		if(!cb(&chunk->entries[next_index], cookie)) {
			break;
		}
		++next_index;
		if(chunk == stack->top && next_index == stack->top_used) {
			break;
		} else if(next_index == CHUNK_CAPACITY) {
			next_index = 0;
			struct stack_chunk *next_chunk = stack->top;
			while(next_chunk->previous != chunk) {
				next_chunk = next_chunk->previous;
			}
			chunk = next_chunk;
		}
	}
}

struct print_path_cookie {
	FILE *dest;
	size_t width;
};

static bool print_path_impl(struct stack_entry *e, void *cookie_raw) {
	struct print_path_cookie *cookie = cookie_raw;
	fputs(e->name, cookie->dest);
	cookie->width += strlen(e->name);
	putc('/', cookie->dest);
	++cookie->width;
	return true;
}

static void print_path(struct stack *stack, const char *final_component, size_t *width, FILE *dest) {
	struct print_path_cookie cookie = {
		.dest = dest,
		.width = width ? *width : 0,
	};
	stack_foreach_up(stack, &print_path_impl, &cookie);
	if(final_component) {
		fputs(final_component, dest);
		cookie.width += strlen(final_component);
	}
	if(width) {
		*width = cookie.width;
	}
}

static void clear_line(size_t *width) {
	for(size_t i = 0; i != *width; ++i) {
		putchar(' ');
	}
	putchar('\r');
	fflush(stdout);
	*width = 0;
}

static void show_path_error(struct stack *stack, const char *final_component, size_t *current_line_width, const char *message) {
	clear_line(current_line_width);
	print_path(stack, final_component, 0, stderr);
	fputs(": ", stderr);
	fputs(message, stderr);
	putc('\n', stderr);
}

static void show_path_errno(struct stack *stack, const char *final_component, size_t *current_line_width) {
	show_path_error(stack, final_component, current_line_width, strerror(errno));
}

struct loop_check {
	uint32_t dev_major, dev_minor;
	uint64_t inode;
	bool loop_found;
};

static bool check_loop(struct stack_entry *e, void *check_raw) {
	struct loop_check *check = check_raw;
	if(check->dev_major == e->dev_major && check->dev_minor == e->dev_minor && check->inode == e->inode) {
		check->loop_found = true;
		return false;
	}
	return true;
}

static bool process(int dir_fd, const char *name, struct stack *stack, uint8_t (*fsid)[BTRFS_FSID_SIZE], bool verbose, size_t *current_line_width, clock_t *last_progress_time) {
	// Start with an O_PATH so that we don’t provoke things like named pipes
	// and device nodes. Also use O_NOFOLLOW because we are doing a physical
	// tree traversal, so symlinks should never be followed.
	int path_fd = openat(dir_fd, name, O_RDONLY | O_PATH | O_NOFOLLOW | O_NOATIME);
	if(path_fd < 0) {
		if(errno == ENOENT) {
			// The file was deleted in between when we found it in the
			// directory scan and now. This is not an error. It is a normal
			// occurrence while navigating the filesystem. Ignore it.
			return true;
		} else {
			// Something else weirder went wrong.
			show_path_errno(stack, name, current_line_width);
			return false;
		}
	}

	// Now that we have a handle to the file which is race-proof and cannot be
	// swapped out with any other file from under us, get information about the
	// file.
	struct statx statbuf;
	if(statx(path_fd, "", AT_EMPTY_PATH, STATX_TYPE | STATX_INO, &statbuf) < 0) {
		show_path_errno(stack, name, current_line_width);
		close(path_fd);
		return false;
	}
	if((statbuf.stx_mask & (STATX_TYPE | STATX_INO)) != (STATX_TYPE | STATX_INO)) {
		show_path_error(stack, name, current_line_width, "statx returned with required information missing");
		close(path_fd);
		return false;
	}
	struct statfs statfsbuf;
	if(fstatfs(path_fd, &statfsbuf) < 0) {
		show_path_errno(stack, name, current_line_width);
		close(path_fd);
		return false;
	}

	// If this file isn’t on a btrfs filesystem, skip it. It might be a mount
	// point of some other filesystem type, an unmounted automount point, or
	// something like that. In any case, if it’s not btrfs, we can’t defragment
	// it. But it’s not an error.
	if(statfsbuf.f_type != BTRFS_SUPER_MAGIC) {
		close(path_fd);
		return true;
	}

	// If this is neither a file nor a directory (e.g. it’s a symlink, or a
	// named pipe, or a device node, or a UNIX-domain socket, or something else
	// weird), don’t touch it at all. Such things can be problematic or
	// dangerous to actually open, and anyway they can’t be defragmented.
	if(!S_ISDIR(statbuf.stx_mode) && !S_ISREG(statbuf.stx_mode)) {
		close(path_fd);
		return true;
	}

	// Now that we know it’s a regular file or directory, it’s safe to actually
	// open it. We can’t go back to the name, because someone could have
	// swapped it out after the first openat call. However, we can use
	// /proc/self/fd/foo to open a non-O_PATH copy of an O_PATH file
	// descriptor.
	//
	// Do not use O_NONBLOCK. For regular files, the only difference relates to
	// file leases. Since even an O_NONBLOCK open causes initiation of a lease
	// downgrade, using O_NONBLOCK would not reduce our impact on other
	// applications; consequently, we might as well do a blocking-open and then
	// we can actually defragment the file once we get it open.
	int file_fd;
	{
		char buffer[64];
		sprintf(buffer, "/proc/self/fd/%d", path_fd);
		file_fd = open(buffer, O_RDONLY | O_NOATIME);
	}
	if(file_fd < 0) {
		show_path_errno(stack, name, current_line_width);
		close(path_fd);
		return false;
	}

	// No need to keep the path FD around any more now that we have a real file
	// FD.
	close(path_fd);

	// Check if we have hit a loop.
	if(S_ISDIR(statbuf.stx_mode)) {
		struct loop_check check = {
			.dev_major = statbuf.stx_dev_major,
			.dev_minor = statbuf.stx_dev_minor,
			.inode = statbuf.stx_ino,
			.loop_found = false,
		};
		stack_foreach_down(stack, &check_loop, &check);
		if(check.loop_found) {
			show_path_error(stack, name, current_line_width, "filesystem loop detected");
			close(file_fd);
			return false;
		}
	}

	// If this is the top-level directory, populate fsid now that we have an
	// open descriptor.
	if(stack_empty(stack)) {
		struct btrfs_ioctl_fs_info_args args;
		if(ioctl(file_fd, BTRFS_IOC_FS_INFO, &args) < 0) {
			show_path_errno(stack, name, current_line_width);
			close(file_fd);
			return false;
		}
		memcpy(*fsid, args.fsid, BTRFS_FSID_SIZE);
	}

	// Check if we are crossing into a different filesystem (*NOT* just a
	// different subvolume; we want to recur there).
	bool new_device_number = stack_empty(stack) || statbuf.stx_dev_major != stack_peek(stack)->dev_major || statbuf.stx_dev_minor != stack_peek(stack)->dev_minor;
	if(S_ISDIR(statbuf.stx_mode) && !stack_empty(stack) && new_device_number) {
		// Device differs, so this is *either* a new subvolume *or* a new
		// filesystem.
		if(statbuf.stx_ino == 2) {
			// Inode 2 is a special thing called
			// BTRFS_EMPTY_SUBVOL_DIR_OBJECTID. It is a bit like a subvolume in
			// that it has a distinct device number, but it can never contain
			// any files. One way to get an EMPTY_SUBVOL is to snapshot a
			// subvolume that hierarchically contains a second subvolume;
			// wherever the inner subvolume appeared in the source, an
			// EMPTY_SUBVOL will appear in the snapshot.
			//
			// Almost nothing works on it in terms of btrfs ioctls, even those
			// that normally work on any file or directory. However, since it
			// doesn’t actually contain anything, we’re also totally fine
			// ignoring it.
			close(file_fd);
			return true;
		}
		struct btrfs_ioctl_fs_info_args args;
		if(ioctl(file_fd, BTRFS_IOC_FS_INFO, &args) < 0) {
			show_path_errno(stack, name, current_line_width);
			close(file_fd);
			return false;
		}
		if(memcmp(args.fsid, *fsid, BTRFS_FSID_SIZE)) {
			// We’ve crossed a mount point into a different btrfs filesystem.
			close(file_fd);
			return true;
		}
	}

	// If this is a file or the top-level directory of a subvolume (but not any
	// other directory), defragment it.
	bool ok = true;
	if(S_ISREG(statbuf.stx_mode) || new_device_number) {
		struct btrfs_ioctl_defrag_range_args args = {
			.len = (uint64_t) -1,
			.extent_thresh = EXTENT_THRESHOLD,
		};
		if(ioctl(file_fd, BTRFS_IOC_DEFRAG_RANGE, &args) < 0) {
			// Defragmentation of files in read-only subvolumes fails with
			// EROFS. We could check this ahead of time, but just letting the
			// defragment ioctl fail is harmless. Unfortunately we can’t prune
			// the entire subtree when we hit the root of a read-only
			// subvolume, because it’s possible to create a subvolume foo,
			// create a subvolume foo/bar, and then use “btrfs property” to
			// make foo read-only while leaving foo/bar read-write, in which
			// case we need to find and defragment foo/bar and its contents.
			//
			// This does mean we won’t print any error messages if you try to
			// defragment a read-only mount (i.e. one mounted with the “ro”
			// mount option), but that’s not going to be a problem in any
			// real-life scenario (either you know your mount is read-only and
			// probably aren’t running maintenance on it, or you don’t know and
			// you have other bigger problems anyway).
			if(errno != EROFS) {
				show_path_errno(stack, name, current_line_width);
				ok = false;
			}
		}
	}

	// If this is a directory, push it on the stack to scan. Otherwise, close
	// it.
	if(S_ISDIR(statbuf.stx_mode)) {
		struct stack_entry e = {
			.dev_major = statbuf.stx_dev_major,
			.dev_minor = statbuf.stx_dev_minor,
			.inode = statbuf.stx_ino,
		};
		e.name = strdup(name);
		if(e.name) {
			// Strip trailing slashes, which will only be present for the
			// top-level (potentially) so the printed path looks nicer.
			{
				size_t len = strlen(e.name);
				while(len && e.name[len - 1] == '/') {
					--len;
				}
				e.name[len] = '\0';
			}

			e.dir_handle = fdopendir(file_fd);
			if(e.dir_handle) {
				if(stack_push(stack, &e)) {
					// All good.
					if(verbose) {
						// Need to give some kind of progress indication. Just
						// print the directories as we enter them. But don’t do
						// it too often.
						clock_t now = clock();
						if(*last_progress_time == (clock_t) -1 || now == (clock_t) -1 || now - *last_progress_time >= (CLOCKS_PER_SEC / (1000 / MILLISECONDS_PER_PROGRESS))) {
							clear_line(current_line_width);
							print_path(stack, 0, current_line_width, stdout);
							putchar('\r');
							fflush(stdout);
							*last_progress_time = now;
						}
					}
				} else {
					free(e.name);
					closedir(e.dir_handle);
				}
			} else {
				show_path_errno(stack, name, current_line_width);
				ok = false;
				free(e.name);
				close(file_fd);
			}
		} else {
			perror("strdup");
			ok = false;
			close(file_fd);
		}
	} else {
		close(file_fd);
	}

	return ok;
}

bool do_defrag(const char *mountpoint, bool verbose) {
	if(verbose) {
		printf("Defragment %s:\n", mountpoint);
	}
	struct stack stack;
	stack_init(&stack);
	uint8_t fsid[BTRFS_FSID_SIZE];
	size_t current_line_width = 0;
	clock_t last_progress_time = (clock_t) -1;
	bool ok = process(AT_FDCWD, mountpoint, &stack, &fsid, verbose, &current_line_width, &last_progress_time);
	while(!stack_empty(&stack)) {
		struct stack_entry *e = stack_peek(&stack);
		errno = 0;
		struct dirent *de = readdir(e->dir_handle);
		if(de) {
			// Skip things other than files or directories. This is only an
			// optimization; process() will also do a proper race-free check.
			if(de->d_type == DT_DIR || de->d_type == DT_REG || de->d_type == DT_UNKNOWN) {
				// Skip the . and .. entries.
				if(strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
					ok &= process(dirfd(e->dir_handle), de->d_name, &stack, &fsid, verbose, &current_line_width, &last_progress_time);
				}
			}
		} else if(errno) {
			show_path_errno(&stack, 0, &current_line_width);
			stack_pop(&stack);
		} else {
			// No more entries.
			stack_pop(&stack);
		}
	}
	stack_deinit(&stack);

	// If we were displaying progress, print an empty line to avoid terminal
	// corruption.
	if(verbose) {
		putchar('\n');
	}

	return ok;
}
