What is maintain-btrfs?
=======================

`maintain-btrfs` performs regular maintenance tasks on btrfs filesystems. It
combines the following tasks into one command: scrub, stats check, defragment,
balance, and trim. Individual steps can be omitted with command-line options.
Defragmentation is recursive, including through subvolumes, so it is not
necessary to defragment each subvolume individually. Output can be verbose,
including progress indication, suitable for interactive use; or quiet, printing
only errors, suitable for launching from cron.


Advantages
==========

While `maintain-btrfs` is certainly usable from cron, there are other btrfs
maintenance packages that will do well in that environment. `maintain-btrfs` is
meant to be equally easy to use on systems where cron is not a practical option
(for example, on desktops or laptops where maintenance should happen when
nobody is using the system, nor is expected to for the duration of the
maintenance). Thus, it aims to run a full suite of maintenance tasks in a
single command, while not requiring configuration.

`maintain-btrfs` also recursively defragments all subvolumes within a mount
point, without requiring them to be listed explicitly on the command line or in
a configuration file.


Limitations
===========

`maintain-btrfs` is not meant to be a full btrfs management tool. It does not
and will not replace `btrfs-progs`. It is purely meant as a quick command to
run weekly or monthly, with any more detailed work using other software.

`maintain-btrfs` is not very customizable. It contains a number of hardcoded
assumptions and numbers, which I think are generally reasonable. It is not
possible to exclude certain files from defragmentation (though read-only
subvolumes cannot be defragmented). When recursively scanning files to
defragment, it always enters all subvolumes of the same filesystem, but never
enters other filesystems through mount points. The defragmentation extent size
limit is fixed at 32 MiB; individual extents above this size will not be moved
even if they are not contiguous. Scrubbing is done on all a filesystem’s
devices simultaneously. Balancing does not modify any profiles, and has
thresholds set to balance data chunks less than 30% full and metadata and
system chunks less than 10% full. It is not possible to work on specific byte
ranges. Patches to allow more customization are welcome though.

When defragmenting, `maintain-btrfs` will only defragment files and subvolumes
visible within the specified mount point; files hidden under further mounts
will be skipped, while bind-mounts or multiple mounts of the same filesystem
within its own tree will result in `maintain-btrfs` doing extra work to
navigate the multiple instances.

`maintain-btrfs` uses modern kernel APIs as of the time of writing. It may not
work on very old kernels, even if those kernels do support btrfs.


Installation
============

To compile `maintain-btrfs`, just run `make`. You should need nothing besides a
C compiler and the Linux kernel headers. The binary can be copied to and run
from any directory. A manual page is provided in `maintain-btrfs.8`.
