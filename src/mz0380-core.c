// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380-internal.h"

MODULE_DESCRIPTION("Driver for MZ0380 based capture cards");
MODULE_AUTHOR("wolffyx");
MODULE_LICENSE("GPL");	/* GPL-2.0-or-later; see COPYING */
MODULE_VERSION("0.1.0");
