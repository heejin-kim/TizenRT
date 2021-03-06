/****************************************************************************
 *
 * Copyright 2017 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * drivers/rtc.c
 *
 *   Copyright (C) 2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>

#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <tinyara/kmalloc.h>
#include <tinyara/fs/fs.h>
#include <tinyara/rtc.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

#ifdef CONFIG_RTC_ALARM
struct rtc_alarminfo_s {
	bool active;			/* True: alarm is active */
	uint8_t signo;			/* Signal number for alarm notification */
	pid_t pid;				/* Identifies task to be notified */
	union sigval sigvalue;	/* Data passed with notification */
};
#endif

struct rtc_upperhalf_s {
	FAR struct rtc_lowerhalf_s *lower;  /* Contained lower half driver */

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
	uint8_t crefs;	/* Number of open references */
	bool unlinked;	/* True if the driver has been unlinked */
#endif

#ifdef CONFIG_RTC_ALARM
	/*
	 * This is an array, indexed by the alarm ID, that provides
	 * information needed to map an alarm expiration to a signal event.
	 */

	struct rtc_alarminfo_s alarminfo[CONFIG_RTC_NALARMS];
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Internal logic */

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static void    rtc_destroy(FAR struct rtc_upperhalf_s *upper);
#endif

#ifdef CONFIG_RTC_ALARM
static void    rtc_alarm_callback(FAR void *priv, int id);
#endif

/* Character driver methods */

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int     rtc_open(FAR struct file *filep);
static int     rtc_close(FAR struct file *filep);
#endif

static ssize_t rtc_read(FAR struct file *filep, FAR char *buffer,
		size_t buflen);
static ssize_t rtc_write(FAR struct file *filep, FAR const char *buffer,
		size_t buflen);
static int     rtc_ioctl(FAR struct file *filep, int cmd, unsigned long arg);

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int     rtc_unlink(FAR struct inode *inode);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations rtc_fops = {
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
	rtc_open,	/* open */
	rtc_close,	/* close */
#else
	0,			/* open */
	0,			/* close */
#endif
	rtc_read,	/* read */
	rtc_write,	/* write */
	0,			/* seek */
	rtc_ioctl,	/* ioctl */
#ifndef CONFIG_DISABLE_POLL
	0,			/* poll */
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
	rtc_unlink	/* unlink */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rtc_destroy
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static void rtc_destroy(FAR struct rtc_upperhalf_s *upper)
{
	/*
	 * If the lower half driver provided a destroy method, then call that
	 * method now in order order to clean up resources used by the
	 * lower-half driver.
	 */

	DEBUGASSERT(upper->lower && upper->lower->ops);
	if (upper->lower->ops->destroy) {
		upper->lower->ops->destroy(upper->lower);
	}

	/* And free our container */

	kmm_free(upper);
}
#endif

/****************************************************************************
 * Name: rtc_alarm_callback
 ****************************************************************************/

#ifdef CONFIG_RTC_ALARM
static void rtc_alarm_callback(FAR void *priv, int alarmid)
{
	FAR struct rtc_upperhalf_s *upper = (FAR struct rtc_upperhalf_s *)priv;
	FAR struct rtc_alarminfo_s *alarminfo;

	DEBUGASSERT(upper != NULL && alarmid >= 0 &&
					alarmid < CONFIG_RTC_NALARMS);
	alarminfo = &upper->alarminfo[alarmid];

	/*
	 * Do we think that the alaram is active?  It might be due to some
	 * race condition between a cancellation event and the alarm
	 * expiration.
	 */

	if (alarminfo->active) {
		/* Yes.. signal the alarm expriration */

#ifdef CONFIG_CAN_PASS_STRUCTS
		(void)sigqueue(alarminfo->pid, alarminfo->signo,
				alarminfo->sigvalue);
#else
		(void)sigqueue(alarminfo->pid, alarminfo->signo,
				alarminfo->sigvalue->sival_ptr);
#endif
	}

	/* The alarm is no longer active */

	alarminfo->active = false;
}
#endif

/****************************************************************************
 * Name: rtc_open
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int rtc_open(FAR struct file *filep)
{
	FAR struct inode *inode;
	FAR struct rtc_upperhalf_s *upper;

	/*
	 * Get the reference to our internal state structure from the inode
	 * structure.
	 */

	DEBUGASSERT(filep);
	inode = filep->f_inode;
	DEBUGASSERT(inode && inode->i_private);
	upper = inode->i_private;

	/* Increment the count of open references on the RTC driver */

	upper->crefs++;
	DEBUGASSERT(upper->crefs > 0);
	return OK;
}
#endif

/****************************************************************************
 * Name: rtc_close
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int rtc_close(FAR struct file *filep)
{
	FAR struct inode *inode;
	FAR struct rtc_upperhalf_s *upper;

	/*
	 * Get the reference to our internal state structure from the inode
	 * structure.
	 */

	DEBUGASSERT(filep);
	inode = filep->f_inode;
	DEBUGASSERT(inode && inode->i_private);
	upper = inode->i_private;

	/* Decrement the count of open references on the RTC driver */

	DEBUGASSERT(upper->crefs > 0);
	upper->crefs--;

	/*
	 * If the count has decremented to zero and the driver has been
	 * unlinked, then commit Hara-Kiri now.
	 */

	if (upper->crefs == 0 && upper->unlinked) {
		rtc_destroy(upper);
	}

	return OK;
}
#endif

/****************************************************************************
 * Name: rtc_read
 ****************************************************************************/

static ssize_t rtc_read(FAR struct file *filep, FAR char *buffer, size_t len)
{
	return 0; /* Return EOF */
}

/****************************************************************************
 * Name: rtc_write
 ****************************************************************************/

static ssize_t rtc_write(FAR struct file *filep, FAR const char *buffer,
		size_t len)
{
	return len; /* Say that everything was written */
}

/****************************************************************************
 * Name: rtc_ioctl
 ****************************************************************************/

static int rtc_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
	FAR struct inode *inode;
	FAR struct rtc_upperhalf_s *upper;
	FAR const struct rtc_ops_s *ops;
	int ret = -ENOSYS;

	/*
	 * Get the reference to our internal state structure from the inode
	 * structure.
	 */

	DEBUGASSERT(filep);
	inode = filep->f_inode;
	DEBUGASSERT(inode && inode->i_private);
	upper = inode->i_private;
	DEBUGASSERT(upper->lower && upper->lower->ops);

	/*
	 * We simply forward all ioctl() commands to the lower half. The upper
	 * half is nothing more than a thin driver shell over the lower level
	 * RTC implementation.
	 */

	ops = upper->lower->ops;
	switch (cmd) {

	/*
	 * RTC_RD_TIME returns the current RTC time.
	 *
	 * Argument: A writeable reference to a struct rtc_time to
	 *           receive the RTC's time.
	 */

	case RTC_RD_TIME: {
		FAR struct rtc_time *rtctime =
				(FAR struct rtc_time *)((uintptr_t)arg);

		if (ops->rdtime) {
			ret = ops->rdtime(upper->lower, rtctime);
		}
	}
	break;

	/*
	 * RTC_SET_TIME sets the RTC's time
	 *
	 * Argument: A read-only reference to a struct rtc_time containing the
	 *           the new time to be set.
	 */

	case RTC_SET_TIME: {
		FAR const struct rtc_time *rtctime =
			(FAR const struct rtc_time *)((uintptr_t)arg);

		if (ops->settime) {
			ret = ops->settime(upper->lower, rtctime);
		}
	}
	break;

#ifdef CONFIG_RTC_ALARM
	/*
	 * RTC_SET_ALARM sets the alarm time.
	 *
	 * Argument: A read-only reference to a struct rtc_time containing the
	 *           new alarm time to be set.
	 */

	case RTC_SET_ALARM: {
		FAR const struct rtc_setalarm_s *alarminfo =
			(FAR const struct rtc_setalarm_s *)((uintptr_t)arg);
		FAR struct rtc_alarminfo_s *upperinfo;
		struct lower_setalarm_s lowerinfo;
		int alarmid;

		DEBUGASSERT(alarminfo != NULL);
		alarmid = alarminfo->id;
		DEBUGASSERT(alarmid >= 0 && alarmid < CONFIG_RTC_NALARMS);

		/* Is the alarm active? */

		upperinfo = &upper->alarminfo[alarmid];
		if (upperinfo->active) {
			/* Yes, cancel the alarm */

			if (ops->cancelalarm) {
				(void)ops->cancelalarm(upper->lower, alarmid);
			}

			upperinfo->active = false;
		}

		if (ops->setalarm) {
			pid_t pid;

			/* A PID of zero means to signal the calling task */

			pid = alarminfo->pid;
			if (pid == 0) {
				pid = getpid();
			}

			/*
			 * Save the signal info to be used to notify the
			 * caller when the alarm expires.
			 */

			upperinfo->active   = true;
			upperinfo->signo    = alarminfo->signo;
			upperinfo->pid      = pid;
			upperinfo->sigvalue = alarminfo->sigvalue;

			/*
			 * Format the alarm info needed by the lower half
			 * driver
			 */

			lowerinfo.id        = alarmid;
			lowerinfo.cb        = rtc_alarm_callback;
			lowerinfo.priv      = (FAR void *)upper;
			lowerinfo.time      = alarminfo->time;

			/* Then set the alarm */

			ret = ops->setalarm(upper->lower, &lowerinfo);
			if (ret < 0) {
				upperinfo->active = false;
			}
		}
	}
	break;

	/*
	 * RTC_SET_RELATIVE sets the alarm time relative to the current time.
	 *
	 * Argument: A read-only reference to a struct rtc_setrelative_s
	 *           containing the new relative alarm time to be set.
	 */

	case RTC_SET_RELATIVE: {
		FAR const struct rtc_setrelative_s *alarminfo =
			(FAR const struct rtc_setrelative_s *)((uintptr_t)arg);
		FAR struct rtc_alarminfo_s *upperinfo;
		struct lower_setrelative_s lowerinfo;
		int alarmid;

		DEBUGASSERT(alarminfo != NULL);
		alarmid = alarminfo->id;
		DEBUGASSERT(alarmid >= 0 && alarmid < CONFIG_RTC_NALARMS);

		/* Is the alarm active? */

		upperinfo = &upper->alarminfo[alarmid];
		if (upperinfo->active) {
			/* Yes, cancel the alarm */

			if (ops->cancelalarm) {
				(void)ops->cancelalarm(upper->lower, alarmid);
			}

			upperinfo->active = false;
		}

		if (ops->setrelative) {
			pid_t pid;

			/* A PID of zero means to signal the calling task */

			pid = alarminfo->pid;
			if (pid == 0) {
				pid = getpid();
			}

			/*
			 * Save the signal info to be used to notify the
			 * caller when the alarm expires.
			 */

			upperinfo->active   = true;
			upperinfo->signo    = alarminfo->signo;
			upperinfo->pid      = pid;
			upperinfo->sigvalue = alarminfo->sigvalue;

			/*
			 * Format the alarm info needed by the lower half
			 * driver
			 */

			lowerinfo.id        = alarmid;
			lowerinfo.cb        = rtc_alarm_callback;
			lowerinfo.priv      = (FAR void *)upper;
			lowerinfo.reltime   = alarminfo->reltime;

			/* Then set the alarm */

			ret = ops->setrelative(upper->lower, &lowerinfo);
			if (ret < 0) {
				upperinfo->active = false;
			}
		}
	}
	break;

	/*
	 * RTC_WKALRM_CANCEL cancel the alarm.
	 *
	 * Argument: An ALARM ID value that indicates which alarm should be
	 *           canceled.
	 */

	case RTC_CANCEL_ALARM: {
		int alarmid = (int)arg;

		DEBUGASSERT(alarmid >= 0 && alarmid < CONFIG_RTC_NALARMS);
		if (ops->cancelalarm) {
			ret = ops->cancelalarm(upper->lower, alarmid);
		}
	}
	break;

#endif /* CONFIG_RTC_ALARM */

	/*
	 * Forward any unrecognized IOCTLs to the lower half driver... they
	 * may represent some architecture-specific command.
	 */

	default: {
		ret = -ENOTTY;
#ifdef CONFIG_RTC_IOCTL
		if (ops->ioctl) {
			ret = ops->ioctl(upper->lower, cmd, arg);
		}
#endif
	}
	break;
	}

	return ret;
}

/****************************************************************************
 * Name: rtc_unlink
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int rtc_unlink(FAR struct inode *inode)
{
	FAR struct rtc_upperhalf_s *upper;

	/*
	 * Get the reference to our internal state structure from the inode
	 * structure.
	 */

	DEBUGASSERT(inode && inode->i_private);
	upper = inode->i_private;

	/* Indicate that the driver has been unlinked */

	upper->unlinked = true;

	/*
	 * If there are no further open references to the driver, then commit
	 * Hara-Kiri now.
	 */

	if (upper->crefs == 0) {
		rtc_destroy(upper);
	}

	return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rtc_initialize
 *
 * Description:
 *   Create an RTC driver by binding to the lower half RTC driver instance
 *   provided to this function.  The resulting RTC driver will be registered
 *   at /dev/rtcN where N is the minor number provided to this function.
 *
 * Input parameters:
 *   minor - The minor number of the RTC device.  The N in /dev/rtcN
 *   lower - The lower half driver instance.
 *
 * Returned Value:
 *   Zero (OK) on success; A negated errno value on failure.
 *
 ****************************************************************************/

int rtc_initialize(int minor, FAR struct rtc_lowerhalf_s *lower)
{
	FAR struct rtc_upperhalf_s *upper;
	char devpath[16];
	int ret;

	DEBUGASSERT(lower && lower->ops && minor >= 0 && minor < 1000);

	/* Allocate an upper half container structure */

	upper = (FAR struct rtc_upperhalf_s *)kmm_zalloc(
					sizeof(struct rtc_upperhalf_s));
	if (!upper) {
		return -ENOMEM;
	}

	/* Initialize the upper half container */

	upper->lower = lower;     /* Contain lower half driver */

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
	upper->crefs = 0;         /* No open references */
	upper->unlinked = false;  /* Driver is not  unlinked */
#endif

	/*
	 * Create the driver name.  There is space for the a minor number
	 * up to 6 characters
	 */

	snprintf(devpath, 16, "/dev/rtc%d", minor);

	/* And, finally, register the new RTC driver */

	ret = register_driver(devpath, &rtc_fops, 0666, upper);
	if (ret < 0) {
		kmm_free(upper);
		return ret;
	}

	return OK;
}
