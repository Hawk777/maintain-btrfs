#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "ops.h"

#define VERSION "1.0.1"

int main(int argc, char **argv) {
	// Parse command-line parameters.
	static int scrub = 1;
	static int defrag = 1;
	static int balance = 1;
	static int trim = 1;
	static const struct option options[] = {
		{ .name = "no-scrub", .has_arg = no_argument, .flag = &scrub, .val = 0 },
		{ .name = "no-defragment", .has_arg = no_argument, .flag = &defrag, .val = 0 },
		{ .name = "no-balance", .has_arg = no_argument, .flag = &balance, .val = 0 },
		{ .name = "no-trim", .has_arg = no_argument, .flag = &trim, .val = 0 },
		{ .name = "verbose", .has_arg = no_argument, .flag = 0, .val = 'v' },
		{ .name = "help", .has_arg = no_argument, .flag = 0, .val = 'h' },
		{ .name = "version", .has_arg = no_argument, .flag = 0, .val = 'V' },
		{ .name = 0, .has_arg = 0, .flag = 0, .val = 0 },
	};
	bool verbose = false;
	{
		bool done = false;
		while(!done) {
			switch(getopt_long(argc, argv, "vhV", options, 0)) {
				case '?':
					return EXIT_FAILURE;

				case 0:
					break;

				case -1:
					done = true;
					break;

				case 'h':
					fprintf(stderr, "Usage:\n\n"
							"%s [--no-scrub] [--no-defragment] [--no-balance] [--no-trim] [--help] mountpoint ...\n\n"
							"--no-scrub: do not scrub the filesystem(s)\n"
							"--no-defragment: do not defragment the filesystem(s)\n"
							"--no-balance: do not balance the filesystem(s)\n"
							"--no-trim: do not trim unused space\n"
							"--verbose/-v: show verbose output during operations\n"
							"--help/-h: display this message\n"
							"--version/-V: show version number\n\n"
							"mountpoint ...: one or more btrfs filesystem mount points to maintain\n", argv[0]);
					return EXIT_SUCCESS;

				case 'v':
					verbose = true;
					break;

				case 'V':
					puts("maintain-btrfs version " VERSION);
					puts("License: GNU GPL version 3");
					return EXIT_SUCCESS;

				default:
					fputs("internal error: getopt returned unrecognized value\n", stderr);
					return EXIT_FAILURE;
			}
		}
	}

	// Sanity check.
	if(optind == argc) {
		fputs("At least one filesystem mount point must be specified.\nRun with -h/--help for usage information.\n", stderr);
		return EXIT_FAILURE;
	}

	// Do work.
	bool ok = true;
	if(scrub) {
		for(int i = optind; i != argc; ++i) {
			ok &= do_scrub(argv[i], verbose);
		}
	}
	for(int i = optind; i != argc; ++i) {
		ok &= do_devstats(argv[i], verbose);
	}
	if(defrag) {
		for(int i = optind; i != argc; ++i) {
			ok &= do_defrag(argv[i], verbose);
		}
	}
	if(balance) {
		for(int i = optind; i != argc; ++i) {
			ok &= do_balance(argv[i], verbose);
		}
	}
	if(trim) {
		for(int i = optind; i != argc; ++i) {
			ok &= do_trim(argv[i], verbose);
		}
	}

	// Done.
	return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
