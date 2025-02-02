// SPDX-License-Identifier: GPL-2.0
/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * libcfs/libcfs/linux/linux-debug.c
 *
 * Author: Phil Schwan <phil@clusterfs.com>
 */

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/notifier.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

# define DEBUG_SUBSYSTEM S_LNET

#include <linux/libcfs/libcfs.h>

#include "../tracefile.h"

#include <linux/kallsyms.h>

char lnet_debug_log_upcall[1024] = "/usr/lib/lustre/lnet_debug_log_upcall";

/**
 * Upcall function once a Lustre log has been dumped.
 *
 * \param file  path of the dumped log
 */
void libcfs_run_debug_log_upcall(char *file)
{
	char *argv[3];
	int rc;
	static const char * const envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		NULL
	};

	argv[0] = lnet_debug_log_upcall;

	LASSERTF(file, "called on a null filename\n");
	argv[1] = file; /* only need to pass the path of the file */

	argv[2] = NULL;

	rc = call_usermodehelper(argv[0], argv, (char **)envp, 1);
	if (rc < 0 && rc != -ENOENT) {
		CERROR("Error %d invoking LNET debug log upcall %s %s; check /sys/kernel/debug/lnet/debug_log_upcall\n",
		       rc, argv[0], argv[1]);
	} else {
		CDEBUG(D_HA, "Invoked LNET debug log upcall %s %s\n",
		       argv[0], argv[1]);
	}
}

/* coverity[+kill] */
void __noreturn lbug_with_loc(struct libcfs_debug_msg_data *msgdata)
{
	libcfs_catastrophe = 1;
	libcfs_debug_msg(msgdata, "LBUG\n");

	if (in_interrupt()) {
		panic("LBUG in interrupt.\n");
		/* not reached */
	}

	dump_stack();
	if (!libcfs_panic_on_lbug)
		libcfs_debug_dumplog();
	if (libcfs_panic_on_lbug)
		panic("LBUG");
	set_current_state(TASK_UNINTERRUPTIBLE);
	while (1)
		schedule();
}
EXPORT_SYMBOL(lbug_with_loc);

static int panic_notifier(struct notifier_block *self, unsigned long unused1,
			  void *unused2)
{
	if (libcfs_panic_in_progress)
		return 0;

	libcfs_panic_in_progress = 1;
	mb();

	return 0;
}

static struct notifier_block libcfs_panic_notifier = {
	.notifier_call	= panic_notifier,
	.next		= NULL,
	.priority	= 10000,
};

void libcfs_register_panic_notifier(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &libcfs_panic_notifier);
}

void libcfs_unregister_panic_notifier(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &libcfs_panic_notifier);
}
