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
 * Copyright (c) 2012, 2015 Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <linux/uio.h>

#include <linux/uaccess.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>

#include <linux/sysctl.h>
#include <linux/debugfs.h>

# define DEBUG_SUBSYSTEM S_LNET

#include <linux/libcfs/libcfs.h>
#include <asm/div64.h>

#include <linux/libcfs/libcfs_crypto.h>
#include <linux/lnet/lib-lnet.h>
#include <uapi/linux/lnet/lnet-dlc.h>
#include "tracefile.h"

static struct dentry *lnet_debugfs_root;

static DECLARE_RWSEM(ioctl_list_sem);
static LIST_HEAD(ioctl_list);

int libcfs_register_ioctl(struct libcfs_ioctl_handler *hand)
{
	int rc = 0;

	down_write(&ioctl_list_sem);
	if (!list_empty(&hand->item))
		rc = -EBUSY;
	else
		list_add_tail(&hand->item, &ioctl_list);
	up_write(&ioctl_list_sem);

	return rc;
}
EXPORT_SYMBOL(libcfs_register_ioctl);

int libcfs_deregister_ioctl(struct libcfs_ioctl_handler *hand)
{
	int rc = 0;

	down_write(&ioctl_list_sem);
	if (list_empty(&hand->item))
		rc = -ENOENT;
	else
		list_del_init(&hand->item);
	up_write(&ioctl_list_sem);

	return rc;
}
EXPORT_SYMBOL(libcfs_deregister_ioctl);

static inline size_t libcfs_ioctl_packlen(struct libcfs_ioctl_data *data)
{
	size_t len = sizeof(*data);

	len += cfs_size_round(data->ioc_inllen1);
	len += cfs_size_round(data->ioc_inllen2);
	return len;
}

static inline bool libcfs_ioctl_is_invalid(struct libcfs_ioctl_data *data)
{
	if (data->ioc_hdr.ioc_len > BIT(30)) {
		CERROR("LIBCFS ioctl: ioc_len larger than 1<<30\n");
		return true;
	}
	if (data->ioc_inllen1 > BIT(30)) {
		CERROR("LIBCFS ioctl: ioc_inllen1 larger than 1<<30\n");
		return true;
	}
	if (data->ioc_inllen2 > BIT(30)) {
		CERROR("LIBCFS ioctl: ioc_inllen2 larger than 1<<30\n");
		return true;
	}
	if (data->ioc_inlbuf1 && !data->ioc_inllen1) {
		CERROR("LIBCFS ioctl: inlbuf1 pointer but 0 length\n");
		return true;
	}
	if (data->ioc_inlbuf2 && !data->ioc_inllen2) {
		CERROR("LIBCFS ioctl: inlbuf2 pointer but 0 length\n");
		return true;
	}
	if (data->ioc_pbuf1 && !data->ioc_plen1) {
		CERROR("LIBCFS ioctl: pbuf1 pointer but 0 length\n");
		return true;
	}
	if (data->ioc_pbuf2 && !data->ioc_plen2) {
		CERROR("LIBCFS ioctl: pbuf2 pointer but 0 length\n");
		return true;
	}
	if (data->ioc_plen1 && !data->ioc_pbuf1) {
		CERROR("LIBCFS ioctl: plen1 nonzero but no pbuf1 pointer\n");
		return true;
	}
	if (data->ioc_plen2 && !data->ioc_pbuf2) {
		CERROR("LIBCFS ioctl: plen2 nonzero but no pbuf2 pointer\n");
		return true;
	}
	if ((u32)libcfs_ioctl_packlen(data) != data->ioc_hdr.ioc_len) {
		CERROR("LIBCFS ioctl: packlen != ioc_len\n");
		return true;
	}
	if (data->ioc_inllen1 &&
	    data->ioc_bulk[data->ioc_inllen1 - 1] != '\0') {
		CERROR("LIBCFS ioctl: inlbuf1 not 0 terminated\n");
		return true;
	}
	if (data->ioc_inllen2 &&
	    data->ioc_bulk[cfs_size_round(data->ioc_inllen1) +
			   data->ioc_inllen2 - 1] != '\0') {
		CERROR("LIBCFS ioctl: inlbuf2 not 0 terminated\n");
		return true;
	}
	return false;
}

static int libcfs_ioctl_data_adjust(struct libcfs_ioctl_data *data)
{
	if (libcfs_ioctl_is_invalid(data)) {
		CERROR("libcfs ioctl: parameter not correctly formatted\n");
		return -EINVAL;
	}

	if (data->ioc_inllen1)
		data->ioc_inlbuf1 = &data->ioc_bulk[0];

	if (data->ioc_inllen2)
		data->ioc_inlbuf2 = &data->ioc_bulk[0] +
			cfs_size_round(data->ioc_inllen1);

	return 0;
}

static int libcfs_ioctl_getdata(struct libcfs_ioctl_hdr **hdr_pp,
				const struct libcfs_ioctl_hdr __user *uhdr)
{
	struct libcfs_ioctl_hdr hdr;
	int err;

	if (copy_from_user(&hdr, uhdr, sizeof(hdr)))
		return -EFAULT;

	if (hdr.ioc_version != LIBCFS_IOCTL_VERSION &&
	    hdr.ioc_version != LIBCFS_IOCTL_VERSION2) {
		CERROR("libcfs ioctl: version mismatch expected %#x, got %#x\n",
		       LIBCFS_IOCTL_VERSION, hdr.ioc_version);
		return -EINVAL;
	}

	if (hdr.ioc_len < sizeof(hdr)) {
		CERROR("libcfs ioctl: user buffer too small for ioctl\n");
		return -EINVAL;
	}

	if (hdr.ioc_len > LIBCFS_IOC_DATA_MAX) {
		CERROR("libcfs ioctl: user buffer is too large %d/%d\n",
		       hdr.ioc_len, LIBCFS_IOC_DATA_MAX);
		return -EINVAL;
	}

	*hdr_pp = kvmalloc(hdr.ioc_len, GFP_KERNEL);
	if (!*hdr_pp)
		return -ENOMEM;

	if (copy_from_user(*hdr_pp, uhdr, hdr.ioc_len)) {
		err = -EFAULT;
		goto free;
	}

	if ((*hdr_pp)->ioc_version != hdr.ioc_version ||
	    (*hdr_pp)->ioc_len != hdr.ioc_len) {
		err = -EINVAL;
		goto free;
	}

	return 0;

free:
	kvfree(*hdr_pp);
	return err;
}

static int libcfs_ioctl(unsigned long cmd, void __user *uparam)
{
	struct libcfs_ioctl_data *data = NULL;
	struct libcfs_ioctl_hdr *hdr;
	int err;

	/* 'cmd' and permissions get checked in our arch-specific caller */
	err = libcfs_ioctl_getdata(&hdr, uparam);
	if (err) {
		CDEBUG_LIMIT(D_ERROR,
			     "libcfs ioctl: data header error %d\n", err);
		return err;
	}

	if (hdr->ioc_version == LIBCFS_IOCTL_VERSION) {
		/*
		 * The libcfs_ioctl_data_adjust() function performs adjustment
		 * operations on the libcfs_ioctl_data structure to make
		 * it usable by the code.  This doesn't need to be called
		 * for new data structures added.
		 */
		data = container_of(hdr, struct libcfs_ioctl_data, ioc_hdr);
		err = libcfs_ioctl_data_adjust(data);
		if (err)
			goto out;
	}

	CDEBUG(D_IOCTL, "libcfs ioctl cmd %lu\n", cmd);
	switch (cmd) {
	case IOC_LIBCFS_CLEAR_DEBUG:
		libcfs_debug_clear_buffer();
		break;

	case IOC_LIBCFS_MARK_DEBUG:
		if (!data || !data->ioc_inlbuf1 ||
		    data->ioc_inlbuf1[data->ioc_inllen1 - 1] != '\0') {
			err = -EINVAL;
			goto out;
		}
		libcfs_debug_mark_buffer(data->ioc_inlbuf1);
		break;

	default: {
		struct libcfs_ioctl_handler *hand;

		err = -EINVAL;
		down_read(&ioctl_list_sem);
		list_for_each_entry(hand, &ioctl_list, item) {
			err = hand->handle_ioctl(cmd, hdr);
			if (err == -EINVAL)
				continue;

			if (!err) {
				if (copy_to_user(uparam, hdr, hdr->ioc_len))
					err = -EFAULT;
			}
			break;
		}
		up_read(&ioctl_list_sem);
		break; }
	}
out:
	kvfree(hdr);
	return err;
}

static long
libcfs_psdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (_IOC_TYPE(cmd) != IOC_LIBCFS_TYPE ||
	    _IOC_NR(cmd) < IOC_LIBCFS_MIN_NR  ||
	    _IOC_NR(cmd) > IOC_LIBCFS_MAX_NR) {
		CDEBUG(D_IOCTL, "invalid ioctl ( type %d, nr %d, size %d )\n",
		       _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
		return -EINVAL;
	}

	return libcfs_ioctl(cmd, (void __user *)arg);
}

static const struct file_operations libcfs_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= libcfs_psdev_ioctl,
};

static struct miscdevice libcfs_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "lnet",
	.fops = &libcfs_fops,
};

int lprocfs_call_handler(void *data, int write, loff_t *ppos,
			 void __user *buffer, size_t *lenp,
			 int (*handler)(void *data, int write, loff_t pos,
					void __user *buffer, int len))
{
	int rc = handler(data, write, *ppos, buffer, *lenp);

	if (rc < 0)
		return rc;

	if (write) {
		*ppos += *lenp;
	} else {
		*lenp = rc;
		*ppos += rc;
	}
	return 0;
}
EXPORT_SYMBOL(lprocfs_call_handler);

static int __proc_dobitmasks(void *data, int write,
			     loff_t pos, void __user *buffer, int nob)
{
	const int tmpstrlen = 512;
	char *tmpstr;
	int rc;
	unsigned int *mask = data;
	int is_subsys = (mask == &libcfs_subsystem_debug) ? 1 : 0;
	int is_printk = (mask == &libcfs_printk) ? 1 : 0;

	rc = cfs_trace_allocate_string_buffer(&tmpstr, tmpstrlen);
	if (rc < 0)
		return rc;

	if (!write) {
		libcfs_debug_mask2str(tmpstr, tmpstrlen, *mask, is_subsys);
		rc = strlen(tmpstr);

		if (pos >= rc) {
			rc = 0;
		} else {
			rc = cfs_trace_copyout_string(buffer, nob,
						      tmpstr + pos, "\n");
		}
	} else {
		rc = cfs_trace_copyin_string(tmpstr, tmpstrlen, buffer, nob);
		if (rc < 0) {
			kfree(tmpstr);
			return rc;
		}

		rc = libcfs_debug_str2mask(mask, tmpstr, is_subsys);
		/* Always print LBUG/LASSERT to console, so keep this mask */
		if (is_printk)
			*mask |= D_EMERG;
	}

	kfree(tmpstr);
	return rc;
}

static int proc_dobitmasks(struct ctl_table *table, int write,
			   void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_dobitmasks);
}

static int __proc_dump_kernel(void *data, int write,
			      loff_t pos, void __user *buffer, int nob)
{
	if (!write)
		return 0;

	return cfs_trace_dump_debug_buffer_usrstr(buffer, nob);
}

static int proc_dump_kernel(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_dump_kernel);
}

static int __proc_daemon_file(void *data, int write,
			      loff_t pos, void __user *buffer, int nob)
{
	if (!write) {
		int len = strlen(cfs_tracefile);

		if (pos >= len)
			return 0;

		return cfs_trace_copyout_string(buffer, nob,
						cfs_tracefile + pos, "\n");
	}

	return cfs_trace_daemon_command_usrstr(buffer, nob);
}

static int proc_daemon_file(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_daemon_file);
}

static int libcfs_force_lbug(struct ctl_table *table, int write,
			     void __user *buffer,
			     size_t *lenp, loff_t *ppos)
{
	if (write)
		LBUG();
	return 0;
}

static int proc_fail_loc(struct ctl_table *table, int write,
			 void __user *buffer,
			 size_t *lenp, loff_t *ppos)
{
	int rc;
	long old_fail_loc = cfs_fail_loc;

	rc = proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
	if (old_fail_loc != cfs_fail_loc)
		wake_up(&cfs_race_waitq);
	return rc;
}

static int __proc_cpt_table(void *data, int write,
			    loff_t pos, void __user *buffer, int nob)
{
	char *buf = NULL;
	int len = 4096;
	int rc  = 0;

	if (write)
		return -EPERM;

	LASSERT(cfs_cpt_table);

	while (1) {
		buf = kzalloc(len, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		rc = cfs_cpt_table_print(cfs_cpt_table, buf, len);
		if (rc >= 0)
			break;

		if (rc == -EFBIG) {
			kfree(buf);
			len <<= 1;
			continue;
		}
		goto out;
	}

	if (pos >= rc) {
		rc = 0;
		goto out;
	}

	rc = cfs_trace_copyout_string(buffer, nob, buf + pos, NULL);
 out:
	kfree(buf);
	return rc;
}

static int proc_cpt_table(struct ctl_table *table, int write,
			  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_cpt_table);
}

static struct ctl_table lnet_table[] = {
	{
		.procname = "debug",
		.data     = &libcfs_debug,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dobitmasks,
	},
	{
		.procname = "subsystem_debug",
		.data     = &libcfs_subsystem_debug,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dobitmasks,
	},
	{
		.procname = "printk",
		.data     = &libcfs_printk,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dobitmasks,
	},
	{
		.procname = "cpu_partition_table",
		.maxlen   = 128,
		.mode     = 0444,
		.proc_handler = &proc_cpt_table,
	},
	{
		.procname = "debug_log_upcall",
		.data     = lnet_debug_log_upcall,
		.maxlen   = sizeof(lnet_debug_log_upcall),
		.mode     = 0644,
		.proc_handler = &proc_dostring,
	},
	{
		.procname = "catastrophe",
		.data     = &libcfs_catastrophe,
		.maxlen   = sizeof(int),
		.mode     = 0444,
		.proc_handler = &proc_dointvec,
	},
	{
		.procname = "dump_kernel",
		.maxlen   = 256,
		.mode     = 0200,
		.proc_handler = &proc_dump_kernel,
	},
	{
		.procname = "daemon_file",
		.mode     = 0644,
		.maxlen   = 256,
		.proc_handler = &proc_daemon_file,
	},
	{
		.procname = "force_lbug",
		.data     = NULL,
		.maxlen   = 0,
		.mode     = 0200,
		.proc_handler = &libcfs_force_lbug
	},
	{
		.procname = "fail_loc",
		.data     = &cfs_fail_loc,
		.maxlen   = sizeof(cfs_fail_loc),
		.mode     = 0644,
		.proc_handler = &proc_fail_loc
	},
	{
		.procname = "fail_val",
		.data     = &cfs_fail_val,
		.maxlen   = sizeof(int),
		.mode     = 0644,
		.proc_handler = &proc_dointvec
	},
	{
		.procname	= "fail_err",
		.data		= &cfs_fail_err,
		.maxlen		= sizeof(cfs_fail_err),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec,
	},
	{
	}
};

static const struct lnet_debugfs_symlink_def lnet_debugfs_symlinks[] = {
	{ "console_ratelimit",
	  "/sys/module/libcfs/parameters/libcfs_console_ratelimit"},
	{ "debug_path",
	  "/sys/module/libcfs/parameters/libcfs_debug_file_path"},
	{ "panic_on_lbug",
	  "/sys/module/libcfs/parameters/libcfs_panic_on_lbug"},
	{ "libcfs_console_backoff",
	  "/sys/module/libcfs/parameters/libcfs_console_backoff"},
	{ "debug_mb",
	  "/sys/module/libcfs/parameters/libcfs_debug_mb"},
	{ "console_min_delay_centisecs",
	  "/sys/module/libcfs/parameters/libcfs_console_min_delay"},
	{ "console_max_delay_centisecs",
	  "/sys/module/libcfs/parameters/libcfs_console_max_delay"},
	{},
};

static ssize_t lnet_debugfs_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct ctl_table *table = filp->private_data;
	int error;

	error = table->proc_handler(table, 0, (void __user *)buf, &count, ppos);
	if (!error)
		error = count;

	return error;
}

static ssize_t lnet_debugfs_write(struct file *filp, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct ctl_table *table = filp->private_data;
	int error;

	error = table->proc_handler(table, 1, (void __user *)buf, &count, ppos);
	if (!error)
		error = count;

	return error;
}

static const struct file_operations lnet_debugfs_file_operations_rw = {
	.open		= simple_open,
	.read		= lnet_debugfs_read,
	.write		= lnet_debugfs_write,
	.llseek		= default_llseek,
};

static const struct file_operations lnet_debugfs_file_operations_ro = {
	.open		= simple_open,
	.read		= lnet_debugfs_read,
	.llseek		= default_llseek,
};

static const struct file_operations lnet_debugfs_file_operations_wo = {
	.open		= simple_open,
	.write		= lnet_debugfs_write,
	.llseek		= default_llseek,
};

static const struct file_operations *lnet_debugfs_fops_select(umode_t mode)
{
	if (!(mode & 0222))
		return &lnet_debugfs_file_operations_ro;

	if (!(mode & 0444))
		return &lnet_debugfs_file_operations_wo;

	return &lnet_debugfs_file_operations_rw;
}

void lustre_insert_debugfs(struct ctl_table *table,
			   const struct lnet_debugfs_symlink_def *symlinks)
{
	if (!lnet_debugfs_root)
		lnet_debugfs_root = debugfs_create_dir("lnet", NULL);

	/* Even if we cannot create, just ignore it altogether) */
	if (IS_ERR_OR_NULL(lnet_debugfs_root))
		return;

	/* We don't save the dentry returned in next two calls, because
	 * we don't call debugfs_remove() but rather remove_recursive()
	 */
	for (; table->procname; table++)
		debugfs_create_file(table->procname, table->mode,
				    lnet_debugfs_root, table,
				    lnet_debugfs_fops_select(table->mode));

	for (; symlinks && symlinks->name; symlinks++)
		debugfs_create_symlink(symlinks->name, lnet_debugfs_root,
				       symlinks->target);
}
EXPORT_SYMBOL_GPL(lustre_insert_debugfs);

static void lustre_remove_debugfs(void)
{
	debugfs_remove_recursive(lnet_debugfs_root);

	lnet_debugfs_root = NULL;
}

static int libcfs_init(void)
{
	int rc;

	rc = libcfs_debug_init(5 * 1024 * 1024);
	if (rc < 0) {
		pr_err("LustreError: libcfs_debug_init: %d\n", rc);
		return rc;
	}

	rc = cfs_cpu_init();
	if (rc)
		goto cleanup_debug;

	rc = misc_register(&libcfs_dev);
	if (rc) {
		CERROR("misc_register: error %d\n", rc);
		goto cleanup_cpu;
	}

	cfs_rehash_wq = alloc_workqueue("cfs_rh", WQ_SYSFS, 4);
	if (!cfs_rehash_wq) {
		CERROR("Failed to start rehash workqueue.\n");
		rc = -ENOMEM;
		goto cleanup_deregister;
	}

	rc = cfs_crypto_register();
	if (rc) {
		CERROR("cfs_crypto_register: error %d\n", rc);
		goto cleanup_deregister;
	}

	lustre_insert_debugfs(lnet_table, lnet_debugfs_symlinks);

	CDEBUG(D_OTHER, "portals setup OK\n");
	return 0;
 cleanup_deregister:
	misc_deregister(&libcfs_dev);
cleanup_cpu:
	cfs_cpu_fini();
 cleanup_debug:
	libcfs_debug_cleanup();
	return rc;
}

static void libcfs_exit(void)
{
	int rc;

	lustre_remove_debugfs();

	if (cfs_rehash_wq) {
		destroy_workqueue(cfs_rehash_wq);
		cfs_rehash_wq = NULL;
	}

	cfs_crypto_unregister();

	misc_deregister(&libcfs_dev);

	cfs_cpu_fini();

	rc = libcfs_debug_cleanup();
	if (rc)
		pr_err("LustreError: libcfs_debug_cleanup: %d\n", rc);
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre helper library");
MODULE_VERSION(LIBCFS_VERSION);
MODULE_LICENSE("GPL");

module_init(libcfs_init);
module_exit(libcfs_exit);
