/*
  drbd_int.h

  This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

  Copyright (C) 2001-2008, LINBIT Information Technologies GmbH.
  Copyright (C) 1999-2008, Philipp Reisner <philipp.reisner@linbit.com>.
  Copyright (C) 2002-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

  drbd is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  drbd is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with drbd; see the file COPYING.  If not, write to
  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef _DRBD_INT_H
#define _DRBD_INT_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <linux/ratelimit.h>
#include <linux/tcp.h>
#include <linux/mutex.h>
#include <linux/genhd.h>
#include <linux/idr.h>
#include <net/tcp.h>
#include <linux/lru_cache.h>
#include <linux/prefetch.h>
#include <linux/drbd_genl_api.h>
#include <linux/drbd.h>
#include <linux/drbd_config.h>

#include "drbd_strings.h"
#include "compat.h"
#include "drbd_state.h"
#include "drbd_protocol.h"

#ifdef __CHECKER__
# define __protected_by(x)       __attribute__((require_context(x,1,999,"rdwr")))
# define __protected_read_by(x)  __attribute__((require_context(x,1,999,"read")))
# define __protected_write_by(x) __attribute__((require_context(x,1,999,"write")))
# define __must_hold(x)       __attribute__((context(x,1,1), require_context(x,1,999,"call")))
#else
# define __protected_by(x)
# define __protected_read_by(x)
# define __protected_write_by(x)
# define __must_hold(x)
#endif

#define __no_warn(lock, stmt) do { __acquire(lock); stmt; __release(lock); } while (0)

/* Compatibility for older kernels */
#undef __cond_lock
#ifdef __CHECKER__
# ifndef __acquires
#  define __acquires(x)	__attribute__((context(x,0,1)))
#  define __releases(x)	__attribute__((context(x,1,0)))
#  define __acquire(x)	__context__(x,1)
#  define __release(x)	__context__(x,-1)
# endif
# define __cond_lock(x,c)	((c) ? ({ __acquire(x); 1; }) : 0)
#else
# ifndef __acquires
#  define __acquires(x)
#  define __releases(x)
#  define __acquire(x)	(void)0
#  define __release(x)	(void)0
# endif
# define __cond_lock(x,c) (c)
#endif

/* module parameter, defined in drbd_main.c */
extern unsigned int minor_count;
extern bool disable_sendpage;
extern bool allow_oos;

#ifdef DRBD_ENABLE_FAULTS
extern int enable_faults;
extern int fault_rate;
extern int fault_devs;
#endif

extern char usermode_helper[];

#include <linux/major.h>
#ifndef DRBD_MAJOR
# define DRBD_MAJOR 147
#endif

#include <linux/blkdev.h>
#include <linux/bio.h>

/* I don't remember why XCPU ...
 * This is used to wake the asender,
 * and to interrupt sending the sending task
 * on disconnect.
 */
#define DRBD_SIG SIGXCPU

/* This is used to stop/restart our threads.
 * Cannot use SIGTERM nor SIGKILL, since these
 * are sent out by init on runlevel changes
 * I choose SIGHUP for now.
 *
 * FIXME btw, we should register some reboot notifier.
 */
#define DRBD_SIGKILL SIGHUP

#define ID_IN_SYNC      (4711ULL)
#define ID_OUT_OF_SYNC  (4712ULL)
#define ID_SYNCER (-1ULL)

#define UUID_NEW_BM_OFFSET ((u64)0x0001000000000000ULL)

struct drbd_device;
struct drbd_connection;

#ifdef DBG_ALL_SYMBOLS
# define STATIC
#else
# define STATIC static
#endif

/*
 * dev_printk() and dev_dbg() exist since "forever".
 * dynamic_dev_dbg() and disk_to_dev() exists since v2.6.28-rc1.
 */
#if defined(disk_to_dev)
#define __drbd_printk_device(level, device, fmt, args...) \
	dev_printk(level, disk_to_dev((device)->vdisk), fmt, ## args)
#define __drbd_printk_peer_device(level, peer_device, fmt, args...) \
	dev_printk(level, disk_to_dev((peer_device)->device->vdisk), "%s: " fmt, \
		   rcu_dereference((peer_device)->connection->net_conf)->name, ## args)
#else
#define __drbd_printk_device(level, device, fmt, args...) \
	printk(level "block drbd%u: " fmt, (device)->minor, ## args)
#define __drbd_printk_peer_device(level, peer_device, fmt, args...) \
	printk(level "block drbd%u %s: " fmt, (peer_device)->device->minor, \
	       rcu_dereference((peer_device)->connection->net_conf)->name, ## args)
#endif

#define __drbd_printk_resource(level, resource, fmt, args...) \
	printk(level "drbd %s: " fmt, (resource)->name, ## args)

#define __drbd_printk_connection(level, connection, fmt, args...) \
	printk(level "drbd %s %s: " fmt, (connection)->resource->name,  \
	       rcu_dereference((connection)->net_conf)->name, ## args)

void drbd_printk_with_wrong_object_type(void);

#define __drbd_printk_if_same_type(obj, type, func, level, fmt, args...) \
	(__builtin_types_compatible_p(typeof(obj), type) || \
	 __builtin_types_compatible_p(typeof(obj), const type)), \
	func(level, (const type)(obj), fmt, ## args)

#define drbd_printk(level, obj, fmt, args...) \
	__builtin_choose_expr( \
	  __drbd_printk_if_same_type(obj, struct drbd_device *, \
			     __drbd_printk_device, level, fmt, ## args), \
	  __builtin_choose_expr( \
	    __drbd_printk_if_same_type(obj, struct drbd_resource *, \
			       __drbd_printk_resource, level, fmt, ## args), \
	    __builtin_choose_expr( \
	      __drbd_printk_if_same_type(obj, struct drbd_connection *, \
				 __drbd_printk_connection, level, fmt, ## args), \
	      __builtin_choose_expr( \
		__drbd_printk_if_same_type(obj, struct drbd_peer_device *, \
				 __drbd_printk_peer_device, level, fmt, ## args), \
	        drbd_printk_with_wrong_object_type()))))

#if defined(disk_to_dev)
#define drbd_dbg(device, fmt, args...) \
	dev_dbg(disk_to_dev(device->vdisk), fmt, ## args)
#elif defined(DEBUG)
#define drbd_dbg(device, fmt, args...) \
	drbd_printk(KERN_DEBUG, device, fmt, ## args)
#else
#define drbd_dbg(device, fmt, args...) \
	do { if (0) drbd_printk(KERN_DEBUG, device, fmt, ## args); } while (0)
#endif

#if defined(dynamic_dev_dbg) && defined(disk_to_dev)
#define dynamic_drbd_dbg(device, fmt, args...) \
	dynamic_dev_dbg(disk_to_dev(device->vdisk), fmt, ## args)
#else
#define dynamic_drbd_dbg(device, fmt, args...) \
	drbd_dbg(device, fmt, ## args)
#endif

#define drbd_emerg(device, fmt, args...) \
	drbd_printk(KERN_EMERG, device, fmt, ## args)
#define drbd_alert(device, fmt, args...) \
	drbd_printk(KERN_ALERT, device, fmt, ## args)
#define drbd_err(device, fmt, args...) \
	drbd_printk(KERN_ERR, device, fmt, ## args)
#define drbd_warn(device, fmt, args...) \
	drbd_printk(KERN_WARNING, device, fmt, ## args)
#define drbd_info(device, fmt, args...) \
	drbd_printk(KERN_INFO, device, fmt, ## args)

extern struct ratelimit_state drbd_ratelimit_state;

static inline int drbd_ratelimit(void)
{
	return __ratelimit(&drbd_ratelimit_state);
}

# define D_ASSERT(device, exp)	if (!(exp)) \
	 drbd_err(device, "ASSERT( " #exp " ) in %s:%d\n", __FILE__, __LINE__)

/**
 * expect  -  Make an assertion
 *
 * Unlike the assert macro, this macro returns a boolean result.
 */
#define expect(x, exp) ({								\
		bool _bool = (exp);						\
		if (!_bool)							\
			drbd_err(x, "ASSERTION %s FAILED in %s\n",		\
			        #exp, __func__);				\
		_bool;								\
		})

/* Defines to control fault insertion */
enum {
	DRBD_FAULT_MD_WR = 0,	/* meta data write */
	DRBD_FAULT_MD_RD = 1,	/*           read  */
	DRBD_FAULT_RS_WR = 2,	/* resync          */
	DRBD_FAULT_RS_RD = 3,
	DRBD_FAULT_DT_WR = 4,	/* data            */
	DRBD_FAULT_DT_RD = 5,
	DRBD_FAULT_DT_RA = 6,	/* data read ahead */
	DRBD_FAULT_BM_ALLOC = 7,	/* bitmap allocation */
	DRBD_FAULT_AL_EE = 8,	/* alloc ee */
	DRBD_FAULT_RECEIVE = 9, /* Changes some bytes upon receiving a [rs]data block */

	DRBD_FAULT_MAX,
};

extern unsigned int
_drbd_insert_fault(struct drbd_device *device, unsigned int type);

static inline int
drbd_insert_fault(struct drbd_device *device, unsigned int type) {
#ifdef DRBD_ENABLE_FAULTS
	return fault_rate &&
		(enable_faults & (1<<type)) &&
		_drbd_insert_fault(device, type);
#else
	return 0;
#endif
}

/*
 * our structs
 *************************/

#define SET_MDEV_MAGIC(x) \
	({ typecheck(struct drbd_device*, x); \
	  (x)->magic = (long)(x) ^ DRBD_MAGIC; })
#define IS_VALID_MDEV(x)  \
	(typecheck(struct drbd_device*, x) && \
	  ((x) ? (((x)->magic ^ DRBD_MAGIC) == (long)(x)) : 0))

extern struct idr drbd_devices; /* RCU, updates: genl_lock() */
extern struct list_head drbd_resources; /* RCU, updates: genl_lock() */

extern const char *cmdname(enum drbd_packet cmd);

/* for sending/receiving the bitmap,
 * possibly in some encoding scheme */
struct bm_xfer_ctx {
	/* "const"
	 * stores total bits and long words
	 * of the bitmap, so we don't need to
	 * call the accessor functions over and again. */
	unsigned long bm_bits;
	unsigned long bm_words;
	/* during xfer, current position within the bitmap */
	unsigned long bit_offset;
	unsigned long word_offset;

	/* statistics; index: (h->command == P_BITMAP) */
	unsigned packets[2];
	unsigned bytes[2];
};

extern void INFO_bm_xfer_stats(struct drbd_peer_device *, const char *, struct bm_xfer_ctx *);

static inline void bm_xfer_ctx_bit_to_word_offset(struct bm_xfer_ctx *c)
{
	/* word_offset counts "native long words" (32 or 64 bit),
	 * aligned at 64 bit.
	 * Encoded packet may end at an unaligned bit offset.
	 * In case a fallback clear text packet is transmitted in
	 * between, we adjust this offset back to the last 64bit
	 * aligned "native long word", which makes coding and decoding
	 * the plain text bitmap much more convenient.  */
#if BITS_PER_LONG == 64
	c->word_offset = c->bit_offset >> 6;
#elif BITS_PER_LONG == 32
	c->word_offset = c->bit_offset >> 5;
	c->word_offset &= ~(1UL);
#else
# error "unsupported BITS_PER_LONG"
#endif
}

extern unsigned int drbd_header_size(struct drbd_connection *connection);

/**********************************************************************/
enum drbd_thread_state {
	NONE,
	RUNNING,
	EXITING,
	RESTARTING
};

struct drbd_thread {
	spinlock_t t_lock;
	struct task_struct *task;
	struct completion startstop;
	enum drbd_thread_state t_state;
	int (*function) (struct drbd_thread *);
	struct drbd_resource *resource;
	struct drbd_connection *connection;
	int reset_cpu_mask;
	const char *name;
};

static inline enum drbd_thread_state get_t_state(struct drbd_thread *thi)
{
	/* THINK testing the t_state seems to be uncritical in all cases
	 * (but thread_{start,stop}), so we can read it *without* the lock.
	 *	--lge */

	smp_rmb();
	return thi->t_state;
}

struct drbd_work {
	struct list_head list;
	int (*cb)(struct drbd_work *, int cancel);
};

struct drbd_peer_device_work {
	struct drbd_work w;
	struct drbd_peer_device *peer_device;
};

#include "drbd_interval.h"

extern int drbd_wait_misc(struct drbd_device *, struct drbd_peer_device *, struct drbd_interval *);
extern bool idr_is_empty(struct idr *idr);

extern void lock_all_resources(void);
extern void unlock_all_resources(void);

extern enum drbd_disk_state disk_state_from_md(struct drbd_device *);

/* sequence arithmetic for dagtag (data generation tag) sector numbers.
 * dagtag_newer_eq: true, if a is newer than b */
#define dagtag_newer_eq(a,b)      \
	(typecheck(u64, a) && \
	 typecheck(u64, b) && \
	((s64)(a) - (s64)(b) >= 0))

struct drbd_request {
	struct drbd_device *device;

	/* if local IO is not allowed, will be NULL.
	 * if local IO _is_ allowed, holds the locally submitted bio clone,
	 * or, after local IO completion, the ERR_PTR(error).
	 * see drbd_request_endio(). */
	struct bio *private_bio;

	struct drbd_interval i;

	/* epoch: used to check on "completion" whether this req was in
	 * the current epoch, and we therefore have to close it,
	 * causing a p_barrier packet to be send, starting a new epoch.
	 *
	 * This corresponds to "barrier" in struct p_barrier[_ack],
	 * and to "barrier_nr" in struct drbd_epoch (and various
	 * comments/function parameters/local variable names).
	 */
	unsigned int epoch;

	/* Position of this request in the serialized per-resource change
	 * stream. Can be used to serialize with other events when
	 * communicating the change stream via multiple connections.
	 * Assigned from device->resource->dagtag_sector.
	 *
	 * Given that some IO backends write several GB per second meanwhile,
	 * lets just use a 64bit sequence space. */
	u64 dagtag_sector;

	struct list_head tl_requests; /* ring list in the transfer log */
	struct bio *master_bio;       /* master bio pointer */
	unsigned long start_time;

	/* once it hits 0, we may complete the master_bio */
	atomic_t completion_ref;
	/* once it hits 0, we may destroy this drbd_request object */
	struct kref kref;

	/* rq_state[0] is for local disk,
	 * rest is indexed by peer_device->bitmap_index + 1 */
	unsigned rq_state[1 + MAX_PEERS];
};

struct drbd_epoch {
	struct drbd_connection *connection;
	struct list_head list;
	unsigned int barrier_nr;
	atomic_t epoch_size; /* increased on every request added. */
	atomic_t active;     /* increased on every req. added, and dec on every finished. */
	unsigned long flags;
};

/* drbd_epoch flag bits */
enum {
	DE_BARRIER_IN_NEXT_EPOCH_ISSUED,
	DE_BARRIER_IN_NEXT_EPOCH_DONE,
	DE_CONTAINS_A_BARRIER,
	DE_HAVE_BARRIER_NUMBER,
	DE_IS_FINISHING,
};

enum epoch_event {
	EV_PUT,
	EV_GOT_BARRIER_NR,
	EV_BARRIER_DONE,
	EV_BECAME_LAST,
	EV_CLEANUP = 32, /* used as flag */
};

struct digest_info {
	int digest_size;
	void *digest;
};

struct drbd_peer_request {
	struct drbd_work w;
	struct drbd_peer_device *peer_device;
	struct list_head recv_order; /* writes only */
	struct drbd_epoch *epoch; /* for writes */
	struct page *pages;
	atomic_t pending_bios;
	struct drbd_interval i;
	/* see comments on ee flag bits below */
	unsigned long flags;
	union {
		u64 block_id;
		struct digest_info *digest;
	};
	u64 dagtag_sector;
};

/* ee flag bits.
 * While corresponding bios are in flight, the only modification will be
 * set_bit WAS_ERROR, which has to be atomic.
 * If no bios are in flight yet, or all have been completed,
 * non-atomic modification to ee->flags is ok.
 */
enum {
	__EE_CALL_AL_COMPLETE_IO,
	__EE_MAY_SET_IN_SYNC,

	/* This peer request closes an epoch using a barrier.
	 * On successful completion, the epoch is released,
	 * and the P_BARRIER_ACK send. */
	__EE_IS_BARRIER,

	/* In case a barrier failed,
	 * we need to resubmit without the barrier flag. */
	__EE_RESUBMITTED,

	/* we may have several bios per peer request.
	 * if any of those fail, we set this flag atomically
	 * from the endio callback */
	__EE_WAS_ERROR,

	/* This ee has a pointer to a digest instead of a block id */
	__EE_HAS_DIGEST,

	/* Conflicting local requests need to be restarted after this request */
	__EE_RESTART_REQUESTS,

	/* The peer wants a write ACK for this (wire proto C) */
	__EE_SEND_WRITE_ACK,

	/* Is set when net_conf had two_primaries set while creating this peer_req */
	__EE_IN_INTERVAL_TREE,
};
#define EE_CALL_AL_COMPLETE_IO (1<<__EE_CALL_AL_COMPLETE_IO)
#define EE_MAY_SET_IN_SYNC     (1<<__EE_MAY_SET_IN_SYNC)
#define EE_IS_BARRIER          (1<<__EE_IS_BARRIER)
#define	EE_RESUBMITTED         (1<<__EE_RESUBMITTED)
#define EE_WAS_ERROR           (1<<__EE_WAS_ERROR)
#define EE_HAS_DIGEST          (1<<__EE_HAS_DIGEST)
#define EE_RESTART_REQUESTS	(1<<__EE_RESTART_REQUESTS)
#define EE_SEND_WRITE_ACK	(1<<__EE_SEND_WRITE_ACK)
#define EE_IN_INTERVAL_TREE	(1<<__EE_IN_INTERVAL_TREE)

/* flag bits per device */
enum {
	UNPLUG_QUEUED,		/* only relevant with kernel 2.4 */
	UNPLUG_REMOTE,		/* sending a "UnplugRemote" could help */
	MD_DIRTY,		/* current uuids and flags not yet on disk */
	CRASHED_PRIMARY,	/* This node was a crashed primary.
				 * Gets cleared when the state.conn
				 * goes into L_ESTABLISHED state. */
	MD_NO_BARRIER,		/* meta data device does not support barriers,
				   so don't even try */
	SUSPEND_IO,		/* suspend application io */
	GO_DISKLESS,		/* Disk is being detached, on io-error or admin request. */
	WAS_IO_ERROR,		/* Local disk failed, returned IO error */
	WAS_READ_ERROR,		/* Local disk READ failed (set additionally to the above) */
	FORCE_DETACH,		/* Force-detach from local disk, aborting any pending local IO */
	NEW_CUR_UUID,		/* Create new current UUID when thawing IO */
	AL_SUSPENDED,		/* Activity logging is currently suspended. */
	AHEAD_TO_SYNC_SOURCE,   /* Ahead -> SyncSource queued */
};

/* flag bits per peer device */
enum {
	CONSIDER_RESYNC,
	RESYNC_AFTER_NEG,       /* Resync after online grow after the attach&negotiate finished. */
	RESIZE_PENDING,		/* Size change detected locally, waiting for the response from
				 * the peer, if it changed there as well. */
	B_RS_H_DONE,		/* Before resync handler done (already executed) */
	DISCARD_MY_DATA,	/* discard_my_data flag per volume */
	USE_DEGR_WFC_T,		/* degr-wfc-timeout instead of wfc-timeout. */
	READ_BALANCE_RR,
	INITIAL_STATE_SENT,
	INITIAL_STATE_RECEIVED,
	RECONCILIATION_RESYNC,
};

/* definition of bits in bm_flags to be used in drbd_bm_lock
 * and drbd_bitmap_io and friends. */
enum bm_flag {
	BM_P_VMALLOCED = 0x10000,  /* do we need to kfree or vfree bm_pages? */

	/*
	 * The bitmap can be locked to prevent others from clearing, setting,
	 * and/or testing bits.  The following combinations of lock flags make
	 * sense:
	 *
	 *   BM_LOCK_CLEAR,
	 *   BM_LOCK_SET, | BM_LOCK_CLEAR,
	 *   BM_LOCK_TEST | BM_LOCK_SET | BM_LOCK_CLEAR.
	 */

	BM_LOCK_TEST = 0x1,
	BM_LOCK_SET = 0x2,
	BM_LOCK_CLEAR = 0x4,
	BM_LOCK_BULK = 0x8, /* locked for bulk operation, allow all non-bulk operations */

	BM_LOCK_ALL = BM_LOCK_TEST | BM_LOCK_SET | BM_LOCK_CLEAR | BM_LOCK_BULK,

	BM_LOCK_SINGLE_SLOT = 0x10,
};

struct drbd_bitmap {
	struct page **bm_pages;
	spinlock_t bm_lock;

	unsigned long bm_set[MAX_PEERS]; /* number of bits set */
	unsigned long bm_bits;  /* bits per peer */
	size_t   bm_words;
	size_t   bm_number_of_pages;
	sector_t bm_dev_capacity;
	struct mutex bm_change; /* serializes resize operations */

	wait_queue_head_t bm_io_wait; /* used to serialize IO of single pages */

	enum bm_flag bm_flags;
	unsigned int bm_max_peers;

	/* debugging aid, in case we are still racy somewhere */
	char          *bm_why;
	struct task_struct *bm_task;
	struct drbd_peer_device *bm_locked_peer;
};

struct drbd_work_queue {
	struct list_head q;
	spinlock_t q_lock;  /* to protect the list. */
	wait_queue_head_t q_wait;
};

struct drbd_socket {
	struct mutex mutex;
	struct socket    *socket;
	/* this way we get our
	 * send/receive buffers off the stack */
	void *sbuf;
	void *rbuf;
};

struct drbd_peer_md {
	u64 bitmap_uuid;
	u32 flags;
	u32 node_id;
};

struct drbd_md {
	u64 md_offset;		/* sector offset to 'super' block */

	u64 effective_size;	/* last agreed size (sectors) */
	spinlock_t uuid_lock;
	u64 current_uuid;
	u64 device_uuid;
	u32 flags;
	u32 node_id;
	u32 md_size_sect;

	s32 al_offset;	/* signed relative sector offset to activity log */
	s32 bm_offset;	/* signed relative sector offset to bitmap */

	struct drbd_peer_md *peers;
	u64 history_uuids[HISTORY_UUIDS];

	/* cached value of bdev->disk_conf->meta_dev_idx */
	s32 meta_dev_idx;

	/* see al_tr_number_to_on_disk_sector() */
	u32 al_stripes;
	u32 al_stripe_size_4k;
	u32 al_size_4k; /* cached product of the above */
};

struct drbd_backing_dev {
	struct kobject kobject;
	struct block_device *backing_bdev;
	struct block_device *md_bdev;
	struct drbd_md md;
	struct disk_conf *disk_conf; /* RCU, for updates: resource->conf_update */
	sector_t known_size; /* last known size of that backing device */
	char id_to_bit[MAX_PEERS];
};

struct drbd_md_io {
	unsigned int done;
	int error;
};

struct bm_io_work {
	struct drbd_work w;
	struct drbd_device *device;
	struct drbd_peer_device *peer_device;
	char *why;
	enum bm_flag flags;
	int (*io_fn)(struct drbd_device *, struct drbd_peer_device *);
	void (*done)(struct drbd_device *device, struct drbd_peer_device *, int rv);
};

struct fifo_buffer {
	/* singly linked list to accumulate multiple such struct fifo_buffers,
	 * to be freed after a single syncronize_rcu(),
	 * outside a critical section. */
	struct fifo_buffer *next;
	unsigned int head_index;
	unsigned int size;
	int total; /* sum of all values */
	int values[0];
};
extern struct fifo_buffer *fifo_alloc(int fifo_size);

/* flag bits per connection */
enum {
	NET_CONGESTED,		/* The data socket is congested */
	RESOLVE_CONFLICTS,	/* Set on one node, cleared on the peer! */
	SEND_PING,		/* whether asender should send a ping asap */
	SIGNAL_ASENDER,		/* whether asender wants to be interrupted */
	GOT_PING_ACK,		/* set when we receive a ping_ack packet, ping_wait gets woken */
	CONN_WD_ST_CHG_REQ,
	CONN_WD_ST_CHG_OKAY,
	CONN_WD_ST_CHG_FAIL,
	CONN_DRY_RUN,		/* Expect disconnect after resync handshake. */
	CREATE_BARRIER,		/* next P_DATA is preceded by a P_BARRIER */
	DISCONNECT_SENT,
};

/* flag bits per resource */
enum {
	EXPLICIT_PRIMARY,
	CALLBACK_PENDING,	/* Whether we have a call_usermodehelper(, UMH_WAIT_PROC)
				 * pending, from drbd worker context.
				 * If set, bdi_write_congested() returns true,
				 * so shrink_page_list() would not recurse into,
				 * and potentially deadlock on, this drbd worker.
				 */
};

enum which_state { NOW, OLD = NOW, NEW };

struct drbd_resource {
	char *name;
	struct kref kref;
	struct idr devices;		/* volume number to device mapping */
	struct list_head connections;
	struct list_head resources;
	struct res_opts res_opts;
	int max_node_id;
	/* conf_update protects the devices, connections, peer devices, net_conf, disk_conf */
	struct mutex conf_update;
	int open_rw_cnt, open_ro_cnt;
	spinlock_t req_lock;
	u64 dagtag_sector;		/* Protected by req_lock.
					 * See also dagtag_sector in
					 * &drbd_request */
	unsigned long flags;

	struct list_head transfer_log;	/* all requests not yet fully processed */

	struct list_head peer_ack_list;  /* requests to send peer acks for */
	u64 last_peer_acked_dagtag;  /* dagtag of last PEER_ACK'ed request */
	struct drbd_request *peer_ack_req;  /* last request not yet PEER_ACK'ed */

	struct mutex state_mutex;
	wait_queue_head_t state_wait;  /* upon each state change. */
	enum chg_state_flags state_change_flags;
	bool remote_state_change;  /* remote state change in progress */
	struct drbd_connection *remote_state_change_prepared;  /* prepared on behalf of peer */

	enum drbd_role role[2];
	bool susp[2];			/* IO suspended by user */
	bool susp_nod[2];		/* IO suspended because no data */
	bool susp_fen[2];		/* IO suspended because fence peer handler runs */

	enum write_ordering_e write_ordering;
	atomic_t current_tle_nr;	/* transfer log epoch number */
	unsigned current_tle_writes;	/* writes seen within this tl epoch */


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30) && !defined(cpumask_bits)
	cpumask_t cpu_mask[1];
#else
	cpumask_var_t cpu_mask;
#endif

	struct drbd_work_queue work;
	struct drbd_thread worker;

	struct list_head listeners;
	spinlock_t listeners_lock;
};

struct drbd_connection {			/* is a resource from the config file */
	struct list_head connections;
	struct drbd_resource *resource;
	struct kref kref;
	struct idr peer_devices;	/* volume number to peer device mapping */
	enum drbd_conn_state cstate[2];
	enum drbd_role peer_role[2];

	unsigned long flags;
	struct net_conf *net_conf;	/* content protected by rcu */
	enum drbd_fencing_policy fencing_policy;
	wait_queue_head_t ping_wait;	/* Woken upon reception of a ping, and a state change */

	struct sockaddr_storage my_addr;
	int my_addr_len;
	struct sockaddr_storage peer_addr;
	int peer_addr_len;

	struct drbd_socket data;	/* data/barrier/cstate/parameter packets */
	struct drbd_socket meta;	/* ping/ack (metadata) packets */
	int agreed_pro_version;		/* actually used protocol version */
	unsigned long last_received;	/* in jiffies, either socket */
	unsigned int ko_count;

	struct crypto_hash *cram_hmac_tfm;
	struct crypto_hash *integrity_tfm;  /* checksums we compute, updates protected by connection->data->mutex */
	struct crypto_hash *peer_integrity_tfm;  /* checksums we verify, only accessed from receiver thread  */
	struct crypto_hash *csums_tfm;
	struct crypto_hash *verify_tfm;
	void *int_dig_in;
	void *int_dig_vv;

	/* receiver side */
	struct drbd_epoch *current_epoch;
	spinlock_t epoch_lock;
	unsigned int epochs;

	unsigned long last_reconnect_jif;
	struct drbd_thread receiver;
	struct drbd_thread sender;
	struct drbd_thread asender;
	struct list_head peer_requests; /* All peer requests in the order we received them.. */
	u64 last_dagtag_sector;

	/* sender side */
	struct drbd_work_queue sender_work;

	/* For limiting logging verbosity of the asender thread */
	enum drbd_state_rv last_remote_state_error;
	unsigned long last_remote_state_error_jiffies;

	struct sender_todo {
		struct list_head work_list;

#ifdef blk_queue_plugged
		/* For older kernels that do and need explicit unplug,
		 * we store here the resource->dagtag_sector of unplug events
		 * when they occur.
		 *
		 * If upper layers trigger an unplug on this side, we want to
		 * send and unplug hint over to the peer.  Sending it too
		 * early, or missing it completely, causes a potential latency
		 * penalty (requests idling too long in the remote queue).
		 * There is no harm done if we occasionally send one too many
		 * such unplug hints.
		 *
		 * We have two slots, which are used in an alternating fashion:
		 * If a new unplug event happens while the current pending one
		 * has not even been processed yet, we overwrite the next
		 * pending slot: there is not much point in unplugging on the
		 * remote side, if we have a full request queue to be send on
		 * this side still, and not even reached the position in the
		 * change stream when the previous local unplug happened.
		 */
		u64 unplug_dagtag_sector[2];
		unsigned int unplug_slot; /* 0 or 1 */
#endif

		/* the currently (or last) processed request,
		 * see process_sender_todo() */
		struct drbd_request *req;

		/* Points to the next request on the resource->transfer_log,
		 * which is RQ_NET_QUEUED for this connection, and so can
		 * safely be used as next starting point for the list walk
		 * in tl_next_request_for_connection().
		 *
		 * If it is NULL (we walked off the tail last time), it will be
		 * set by __req_mod( QUEUE_FOR.* ), so fast connections don't
		 * need to walk the full transfer_log list every time, even if
		 * the list is kept long by some slow connections.
		 *
		 * There is also a special value to reliably re-start
		 * the transfer log walk after having scheduled the requests
		 * for RESEND. */
#define TL_NEXT_REQUEST_RESEND	((void*)1)
		struct drbd_request *req_next;
	} todo;

	struct {
		/* whether this sender thread
		 * has processed a single write yet. */
		bool seen_any_write_yet;

		/* Which barrier number to send with the next P_BARRIER */
		int current_epoch_nr;

		/* how many write requests have been sent
		 * with req->epoch == current_epoch_nr.
		 * If none, no P_BARRIER will be sent. */
		unsigned current_epoch_writes;

		/* position in change stream */
		u64 current_dagtag_sector;
	} send;
};

struct drbd_peer_device {
	struct list_head peer_devices;
	struct drbd_device *device;
	struct drbd_connection *connection;
	enum drbd_disk_state disk_state[2];
	enum drbd_repl_state repl_state[2];
	bool resync_susp_user[2];
	bool resync_susp_peer[2];
	bool resync_susp_dependency[2];
	bool resync_susp_other_c[2];
	unsigned int send_cnt;
	unsigned int recv_cnt;
	atomic_t packet_seq;
	unsigned int peer_seq;
	spinlock_t peer_seq_lock;
	unsigned int max_bio_size;
	sector_t max_size;  /* maximum disk size allowed by peer */
	int bitmap_index;
	int node_id;

	unsigned long flags;

	struct drbd_work start_resync_work;
	struct timer_list start_resync_timer;
	struct drbd_work resync_work;
	struct timer_list resync_timer;

	/* Used to track operations of resync... */
	struct lru_cache *resync_lru;
	/* Number of locked elements in resync LRU */
	unsigned int resync_locked;
	/* resync extent number waiting for application requests */
	unsigned int resync_wenr;

	atomic_t ap_pending_cnt; /* AP data packets on the wire, ack expected */
	atomic_t unacked_cnt;	 /* Need to send replies for */
	atomic_t rs_pending_cnt; /* RS request/data packets on the wire */

	/* blocks to resync in this run [unit BM_BLOCK_SIZE] */
	unsigned long rs_total;
	/* number of resync blocks that failed in this run */
	unsigned long rs_failed;
	/* Syncer's start time [unit jiffies] */
	unsigned long rs_start;
	/* cumulated time in PausedSyncX state [unit jiffies] */
	unsigned long rs_paused;
	/* skipped because csum was equal [unit BM_BLOCK_SIZE] */
	unsigned long rs_same_csum;
#define DRBD_SYNC_MARKS 8
#define DRBD_SYNC_MARK_STEP (3*HZ)
	/* block not up-to-date at mark [unit BM_BLOCK_SIZE] */
	unsigned long rs_mark_left[DRBD_SYNC_MARKS];
	/* marks's time [unit jiffies] */
	unsigned long rs_mark_time[DRBD_SYNC_MARKS];
	/* current index into rs_mark_{left,time} */
	int rs_last_mark;

	/* where does the admin want us to start? (sector) */
	sector_t ov_start_sector;
	sector_t ov_stop_sector;
	/* where are we now? (sector) */
	sector_t ov_position;
	/* Start sector of out of sync range (to merge printk reporting). */
	sector_t ov_last_oos_start;
	/* size of out-of-sync range in sectors. */
	sector_t ov_last_oos_size;
	int c_sync_rate; /* current resync rate after syncer throttle magic */
	struct fifo_buffer *rs_plan_s; /* correction values of resync planer (RCU, connection->conn_update) */
	atomic_t rs_sect_in; /* for incoming resync data rate, SyncTarget */
	int rs_last_sect_ev; /* counter to compare with */
	int rs_last_events;  /* counter of read or write "events" (unit sectors)
			      * on the lower level device when we last looked. */
	int rs_in_flight; /* resync sectors in flight (to proxy, in proxy and from proxy) */
	unsigned long ov_left; /* in bits */

	u64 current_uuid;
	u64 bitmap_uuid;
	u64 history_uuids[HISTORY_UUIDS];
	u64 dirty_bits;
	u64 uuid_flags;
	bool uuids_received;

	unsigned long comm_bm_set; /* communicated number of set bits. */
};

struct submit_worker {
	struct workqueue_struct *wq;
	struct work_struct worker;

	spinlock_t lock;
	struct list_head writes;
};

struct drbd_device {
#ifdef PARANOIA
	long magic;
#endif
	struct drbd_resource *resource;
	struct list_head peer_devices;
	int vnr;			/* volume number within the connection */
	struct kobject kobj;

	/* things that are stored as / read from meta data on disk */
	unsigned long flags;

	/* configured by drbdsetup */
	struct drbd_backing_dev *ldev __protected_by(local);

	struct request_queue *rq_queue;
	struct block_device *this_bdev;
	struct gendisk	    *vdisk;

	unsigned long last_reattach_jif;
	struct drbd_work go_diskless;
	struct drbd_work md_sync_work;

	struct timer_list md_sync_timer;
	struct timer_list request_timer;
#ifdef DRBD_DEBUG_MD_SYNC
	struct {
		unsigned int line;
		const char* func;
	} last_md_mark_dirty;
#endif

	enum drbd_disk_state disk_state[2];
	wait_queue_head_t misc_wait;
	unsigned int read_cnt;
	unsigned int writ_cnt;
	unsigned int al_writ_cnt;
	unsigned int bm_writ_cnt;
	atomic_t ap_bio_cnt;	 /* Requests we need to complete */
	atomic_t local_cnt;	 /* Waiting for local completion */

	/* Interval trees of pending local requests */
	struct rb_root read_requests;
	struct rb_root write_requests;

	struct drbd_bitmap *bitmap;
	unsigned long bm_resync_fo; /* bit offset for drbd_bm_find_next */

	/* FIXME clean comments, restructure so it is more obvious which
	 * members are protected by what */

	struct list_head active_ee; /* IO in progress (P_DATA gets written to disk) */
	struct list_head sync_ee;   /* IO in progress (P_RS_DATA_REPLY gets written to disk) */
	struct list_head done_ee;   /* need to send P_WRITE_ACK */
	struct list_head read_ee;   /* [RS]P_DATA_REQUEST being read */
	struct list_head net_ee;    /* zero-copy network send in progress */

	int next_barrier_nr;
	atomic_t pp_in_use;		/* allocated from page pool */
	atomic_t pp_in_use_by_net;	/* sendpage()d, still referenced by tcp */
	wait_queue_head_t ee_wait;
	struct page *md_io_page;	/* one page buffer for md_io */
	struct drbd_md_io md_io;
	atomic_t md_io_in_use;		/* protects the md_io, md_io_page and md_io_tmpp */
	spinlock_t al_lock;
	wait_queue_head_t al_wait;
	struct lru_cache *act_log;	/* activity log */
	unsigned int al_tr_number;
	int al_tr_cycle;
	wait_queue_head_t seq_wait;
	unsigned int minor;
	u64 exposed_data_uuid; /* UUID of the exposed data */
	atomic_t rs_sect_ev; /* for submitted resync data rate, both */
	atomic_t ap_in_flight; /* App sectors in flight (waiting for ack) */
	struct list_head pending_bitmap_work;
	struct device_conf device_conf;

	/* any requests that would block in drbd_make_request()
	 * are deferred to this single-threaded work queue */
	struct submit_worker submit;
};

static inline struct drbd_device *minor_to_mdev(unsigned int minor)
{
	return (struct drbd_device *)idr_find(&drbd_devices, minor);
}


static inline struct drbd_peer_device *
conn_peer_device(struct drbd_connection *connection, int volume_number)
{
	return idr_find(&connection->peer_devices, volume_number);
}

static inline unsigned drbd_req_state_by_peer_device(struct drbd_request *req,
		struct drbd_peer_device *peer_device)
{
	int idx = peer_device->node_id;
	if (idx < 0 || idx >= MAX_PEERS) {
		drbd_warn(peer_device, "FIXME: bitmap_index: %d\n", idx);
		/* WARN(1, "bitmap_index: %d", idx); */
		return 0;
	}
	return req->rq_state[1 + idx];
}

#define for_each_resource(resource, _resources) \
	list_for_each_entry(resource, _resources, resources)

#define for_each_resource_rcu(resource, _resources) \
	list_for_each_entry_rcu(resource, _resources, resources)

#define for_each_resource_safe(resource, tmp, _resources) \
	list_for_each_entry_safe(resource, tmp, _resources, resources)

#define for_each_connection(connection, resource) \
	list_for_each_entry(connection, &resource->connections, connections)

#define for_each_connection_rcu(connection, resource) \
	list_for_each_entry_rcu(connection, &resource->connections, connections)

#define for_each_connection_safe(connection, tmp, resource) \
	list_for_each_entry_safe(connection, tmp, &resource->connections, connections)

#define for_each_peer_device(peer_device, device) \
	list_for_each_entry(peer_device, &device->peer_devices, peer_devices)

#define for_each_peer_device_rcu(peer_device, device) \
	list_for_each_entry_rcu(peer_device, &device->peer_devices, peer_devices)

#define for_each_peer_device_safe(peer_device, tmp, device) \
	list_for_each_entry_safe(peer_device, tmp, &device->peer_devices, peer_devices)

static inline unsigned int device_to_minor(struct drbd_device *device)
{
	return device->minor;
}

/*
 * function declarations
 *************************/

/* drbd_main.c */

enum dds_flags {
	DDSF_FORCED    = 1,
	DDSF_NO_RESYNC = 2, /* Do not run a resync for the new space */
};

extern int  drbd_thread_start(struct drbd_thread *thi);
extern void _drbd_thread_stop(struct drbd_thread *thi, int restart, int wait);
#ifdef CONFIG_SMP
extern void drbd_thread_current_set_cpu(struct drbd_thread *thi);
#else
#define drbd_thread_current_set_cpu(A) ({})
#endif
extern void tl_release(struct drbd_connection *, unsigned int barrier_nr,
		       unsigned int set_size);
extern void tl_clear(struct drbd_connection *);
extern void drbd_free_sock(struct drbd_connection *connection);
extern int drbd_send(struct drbd_connection *connection, struct socket *sock,
		     void *buf, size_t size, unsigned msg_flags);
extern int drbd_send_all(struct drbd_connection *, struct socket *, void *, size_t,
			 unsigned);

extern int __drbd_send_protocol(struct drbd_connection *connection, enum drbd_packet cmd);
extern int drbd_send_protocol(struct drbd_connection *connection);
extern int drbd_send_uuids(struct drbd_peer_device *, u64 uuid_flags, u64 mask);
extern void drbd_gen_and_send_sync_uuid(struct drbd_peer_device *);
extern int drbd_attach_peer_device(struct drbd_peer_device *);
extern int drbd_send_sizes(struct drbd_peer_device *, int trigger_reply, enum dds_flags flags);
extern int drbd_send_state(struct drbd_peer_device *, union drbd_state);
extern int conn_send_state(struct drbd_connection *, union drbd_state);
extern int drbd_send_current_state(struct drbd_peer_device *);
extern int conn_send_current_state(struct drbd_connection *, bool);
extern int drbd_send_sync_param(struct drbd_peer_device *);
extern void drbd_send_b_ack(struct drbd_connection *connection, u32 barrier_nr, u32 set_size);
extern int drbd_send_ack(struct drbd_peer_device *, enum drbd_packet,
			 struct drbd_peer_request *);
extern void drbd_send_ack_rp(struct drbd_peer_device *, enum drbd_packet,
			     struct p_block_req *rp);
extern void drbd_send_ack_dp(struct drbd_peer_device *, enum drbd_packet,
			     struct p_data *dp, int data_size);
extern int drbd_send_ack_ex(struct drbd_peer_device *, enum drbd_packet,
			    sector_t sector, int blksize, u64 block_id);
extern int drbd_send_out_of_sync(struct drbd_peer_device *, struct drbd_request *);
extern int drbd_send_block(struct drbd_peer_device *, enum drbd_packet,
			   struct drbd_peer_request *);
extern int drbd_send_dblock(struct drbd_peer_device *, struct drbd_request *req);
extern int drbd_send_drequest(struct drbd_peer_device *, int cmd,
			      sector_t sector, int size, u64 block_id);
extern int drbd_send_drequest_csum(struct drbd_peer_device *, sector_t sector,
				   int size, void *digest, int digest_size,
				   enum drbd_packet cmd);
extern int drbd_send_ov_request(struct drbd_peer_device *, sector_t sector, int size);

extern int drbd_send_bitmap(struct drbd_device *, struct drbd_peer_device *);
extern int drbd_send_dagtag(struct drbd_connection *connection, u64 dagtag);
extern void drbd_send_sr_reply(struct drbd_peer_device *, enum drbd_state_rv retcode);
extern void conn_send_sr_reply(struct drbd_connection *connection, enum drbd_state_rv retcode);
extern void drbd_send_peers_in_sync(struct drbd_peer_device *, u64, sector_t, int);
extern int drbd_send_peer_dagtag(struct drbd_connection *connection, struct drbd_connection *lost_peer);
extern void drbd_send_current_uuid(struct drbd_peer_device *peer_device, u64 current_uuid);
extern void drbd_free_bc(struct drbd_backing_dev *ldev);
extern void drbd_cleanup_device(struct drbd_device *device);
void drbd_print_uuids(struct drbd_peer_device *peer_device, const char *text);

extern u64 drbd_capacity_to_on_disk_bm_sect(u64 capacity_sect, unsigned int max_peers);
extern void drbd_md_set_sector_offsets(struct drbd_device *device,
				       struct drbd_backing_dev *bdev);
extern void drbd_md_sync(struct drbd_device *device);
extern int  drbd_md_read(struct drbd_device *device, struct drbd_backing_dev *bdev);
extern void drbd_uuid_received_new_current(struct drbd_device *, u64 , u64) __must_hold(local);
extern void drbd_uuid_set_bitmap(struct drbd_peer_device *peer_device, u64 val) __must_hold(local);
extern void _drbd_uuid_set_bitmap(struct drbd_peer_device *peer_device, u64 val) __must_hold(local);
extern void _drbd_uuid_set_current(struct drbd_device *device, u64 val) __must_hold(local);
extern void _drbd_uuid_new_current(struct drbd_device *device, bool forced) __must_hold(local);
extern void drbd_uuid_set_bm(struct drbd_peer_device *peer_device, u64 val) __must_hold(local);
extern void _drbd_uuid_push_history(struct drbd_peer_device *peer_device, u64 val) __must_hold(local);
extern u64 _drbd_uuid_pull_history(struct drbd_peer_device *peer_device) __must_hold(local);
extern void __drbd_uuid_set_current(struct drbd_device *device, u64 val) __must_hold(local);
extern void __drbd_uuid_set_bitmap(struct drbd_peer_device *peer_device, u64 val) __must_hold(local);
extern void drbd_md_set_flag(struct drbd_device *device, enum mdf_flag) __must_hold(local);
extern void drbd_md_clear_flag(struct drbd_device *device, enum mdf_flag)__must_hold(local);
extern int drbd_md_test_flag(struct drbd_backing_dev *, enum mdf_flag);
extern void drbd_md_set_peer_flag(struct drbd_peer_device *, enum mdf_peer_flag);
extern void drbd_md_clear_peer_flag(struct drbd_peer_device *, enum mdf_peer_flag);
extern bool drbd_md_test_peer_flag(struct drbd_peer_device *, enum mdf_peer_flag);
#ifndef DRBD_DEBUG_MD_SYNC
extern void drbd_md_mark_dirty(struct drbd_device *device);
#else
#define drbd_md_mark_dirty(m)	drbd_md_mark_dirty_(m, __LINE__ , __func__ )
extern void drbd_md_mark_dirty_(struct drbd_device *device,
		unsigned int line, const char *func);
#endif
extern void drbd_queue_bitmap_io(struct drbd_device *,
				 int (*io_fn)(struct drbd_device *, struct drbd_peer_device *),
				 void (*done)(struct drbd_device *, struct drbd_peer_device *, int),
				 char *why, enum bm_flag flags,
				 struct drbd_peer_device *);
extern int drbd_bitmap_io(struct drbd_device *,
		int (*io_fn)(struct drbd_device *, struct drbd_peer_device *),
		char *why, enum bm_flag flags,
		struct drbd_peer_device *);
extern int drbd_bitmap_io_from_worker(struct drbd_device *,
		int (*io_fn)(struct drbd_device *, struct drbd_peer_device *),
		char *why, enum bm_flag flags,
		struct drbd_peer_device *);
extern int drbd_bmio_set_n_write(struct drbd_device *device, struct drbd_peer_device *);
extern int drbd_bmio_clear_n_write(struct drbd_device *device, struct drbd_peer_device *);
extern void drbd_go_diskless(struct drbd_device *device);
extern void drbd_ldev_destroy(struct drbd_device *device);

static inline void drbd_uuid_new_current(struct drbd_device *device) __must_hold(local)
{
	_drbd_uuid_new_current(device, false);
}

/* Meta data layout
 *
 * We currently have two possible layouts.
 * Offsets in (512 byte) sectors.
 * external:
 *   |----------- md_size_sect ------------------|
 *   [ 4k superblock ][ activity log ][  Bitmap  ]
 *   | al_offset == 8 |
 *   | bm_offset = al_offset + X      |
 *  ==> bitmap sectors = md_size_sect - bm_offset
 *
 *  Variants:
 *     old, indexed fixed size meta data:
 *
 * internal:
 *            |----------- md_size_sect ------------------|
 * [data.....][  Bitmap  ][ activity log ][ 4k superblock ][padding*]
 *                        | al_offset < 0 |
 *            | bm_offset = al_offset - Y |
 *  ==> bitmap sectors = Y = al_offset - bm_offset
 *
 *  [padding*] are zero or up to 7 unused 512 Byte sectors to the
 *  end of the device, so that the [4k superblock] will be 4k aligned.
 *
 *  The activity log consists of 4k transaction blocks,
 *  which are written in a ring-buffer, or striped ring-buffer like fashion,
 *  which are writtensize used to be fixed 32kB,
 *  but is about to become configurable.
 */

/* Our old fixed size meta data layout
 * allows up to about 3.8TB, so if you want more,
 * you need to use the "flexible" meta data format. */
#define MD_128MB_SECT (128LLU << 11)  /* 128 MB, unit sectors */
#define MD_4kB_SECT	 8
#define MD_32kB_SECT	64

/* One activity log extent represents 4M of storage */
#define AL_EXTENT_SHIFT 22
#define AL_EXTENT_SIZE (1<<AL_EXTENT_SHIFT)

/* We could make these currently hardcoded constants configurable
 * variables at create-md time (or even re-configurable at runtime?).
 * Which will require some more changes to the DRBD "super block"
 * and attach code.
 *
 * updates per transaction:
 *   This many changes to the active set can be logged with one transaction.
 *   This number is arbitrary.
 * context per transaction:
 *   This many context extent numbers are logged with each transaction.
 *   This number is resulting from the transaction block size (4k), the layout
 *   of the transaction header, and the number of updates per transaction.
 *   See drbd_actlog.c:struct al_transaction_on_disk
 * */
#define AL_UPDATES_PER_TRANSACTION	 64	// arbitrary
#define AL_CONTEXT_PER_TRANSACTION	919	// (4096 - 36 - 6*64)/4

#if BITS_PER_LONG == 32
#define LN2_BPL 5
#define cpu_to_lel(A) cpu_to_le32(A)
#define lel_to_cpu(A) le32_to_cpu(A)
#elif BITS_PER_LONG == 64
#define LN2_BPL 6
#define cpu_to_lel(A) cpu_to_le64(A)
#define lel_to_cpu(A) le64_to_cpu(A)
#else
#error "LN2 of BITS_PER_LONG unknown!"
#endif

/* drbd_bitmap.c */
/*
 * We need to store one bit for a block.
 * Example: 1GB disk @ 4096 byte blocks ==> we need 32 KB bitmap.
 * Bit 0 ==> local node thinks this block is binary identical on both nodes
 * Bit 1 ==> local node thinks this block needs to be synced.
 */

#define SLEEP_TIME (HZ/10)

/* We do bitmap IO in units of 4k blocks.
 * We also still have a hardcoded 4k per bit relation. */
#define BM_BLOCK_SHIFT	12			 /* 4k per bit */
#define BM_BLOCK_SIZE	 (1<<BM_BLOCK_SHIFT)
/* mostly arbitrarily set the represented size of one bitmap extent,
 * aka resync extent, to 128 MiB (which is also 4096 Byte worth of bitmap
 * at 4k per bit resolution) */
#define BM_EXT_SHIFT	 27	/* 128 MiB per resync extent */
#define BM_EXT_SIZE	 (1<<BM_EXT_SHIFT)

#if (BM_BLOCK_SHIFT != 12)
#error "HAVE YOU FIXED drbdmeta AS WELL??"
#endif

/* thus many _storage_ sectors are described by one bit */
#define BM_SECT_TO_BIT(x)   ((x)>>(BM_BLOCK_SHIFT-9))
#define BM_BIT_TO_SECT(x)   ((sector_t)(x)<<(BM_BLOCK_SHIFT-9))
#define BM_SECT_PER_BIT     BM_BIT_TO_SECT(1)

/* bit to represented kilo byte conversion */
#define Bit2KB(bits) ((bits)<<(BM_BLOCK_SHIFT-10))

/* in which _bitmap_ extent (resp. sector) the bit for a certain
 * _storage_ sector is located in */
#define BM_SECT_TO_EXT(x)   ((x)>>(BM_EXT_SHIFT-9))

/* how much _storage_ sectors we have per bitmap sector */
#define BM_EXT_TO_SECT(x)   ((sector_t)(x) << (BM_EXT_SHIFT-9))
#define BM_SECT_PER_EXT     BM_EXT_TO_SECT(1)

/* in one sector of the bitmap, we have this many activity_log extents. */
#define AL_EXT_PER_BM_SECT  (1 << (BM_EXT_SHIFT - AL_EXTENT_SHIFT))

#define BM_BLOCKS_PER_BM_EXT_B (BM_EXT_SHIFT - BM_BLOCK_SHIFT)
#define BM_BLOCKS_PER_BM_EXT_MASK  ((1<<BM_BLOCKS_PER_BM_EXT_B) - 1)

/* the extent in "PER_EXTENT" below is an activity log extent
 * we need that many (long words/bytes) to store the bitmap
 *		     of one AL_EXTENT_SIZE chunk of storage.
 * we can store the bitmap for that many AL_EXTENTS within
 * one sector of the _on_disk_ bitmap:
 * bit	 0	  bit 37   bit 38	     bit (512*8)-1
 *	     ...|........|........|.. // ..|........|
 * sect. 0	 `296	  `304			   ^(512*8*8)-1
 *
#define BM_WORDS_PER_EXT    ( (AL_EXT_SIZE/BM_BLOCK_SIZE) / BITS_PER_LONG )
#define BM_BYTES_PER_EXT    ( (AL_EXT_SIZE/BM_BLOCK_SIZE) / 8 )  // 128
#define BM_EXT_PER_SECT	    ( 512 / BM_BYTES_PER_EXTENT )	 //   4
 */

#define DRBD_MAX_SECTORS_32 (0xffffffffLU)
/* we have a certain meta data variant that has a fixed on-disk size of 128
 * MiB, of which 4k are our "superblock", and 32k are the fixed size activity
 * log, leaving this many sectors for the bitmap.
 */

#define DRBD_MAX_SECTORS_FIXED_BM \
	  ((MD_128MB_SECT - MD_32kB_SECT - MD_4kB_SECT) * (1LL<<(BM_EXT_SHIFT-9)))
#if !defined(CONFIG_LBDAF) && !defined(CONFIG_LBD) && BITS_PER_LONG == 32
#define DRBD_MAX_SECTORS      DRBD_MAX_SECTORS_32
#define DRBD_MAX_SECTORS_FLEX DRBD_MAX_SECTORS_32
#else
#define DRBD_MAX_SECTORS      DRBD_MAX_SECTORS_FIXED_BM
/* 16 TB in units of sectors */
#if BITS_PER_LONG == 32
/* adjust by one page worth of bitmap,
 * so we won't wrap around in drbd_bm_find_next_bit.
 * you should use 64bit OS for that much storage, anyways. */
#define DRBD_MAX_SECTORS_FLEX BM_BIT_TO_SECT(0xffff7fff)
#else
/* we allow up to 1 PiB now on 64bit architecture with "flexible" meta data */
#define DRBD_MAX_SECTORS_FLEX (1UL << 51)
/* corresponds to (1UL << 38) bits right now. */
#endif
#endif

/* BIO_MAX_SIZE is 256 * PAGE_CACHE_SIZE,
 * so for typical PAGE_CACHE_SIZE of 4k, that is (1<<20) Byte.
 * Since we may live in a mixed-platform cluster,
 * we limit us to a platform agnostic constant here for now.
 * A followup commit may allow even bigger BIO sizes,
 * once we thought that through. */
#if DRBD_MAX_BIO_SIZE > BIO_MAX_SIZE
#error Architecture not supported: DRBD_MAX_BIO_SIZE > BIO_MAX_SIZE
#endif

#define DRBD_MAX_SIZE_H80_PACKET (1U << 15) /* Header 80 only allows packets up to 32KiB data */
#define DRBD_MAX_BIO_SIZE_P95    (1U << 17) /* Protocol 95 to 99 allows bios up to 128KiB */

extern struct drbd_bitmap *drbd_bm_alloc(void);
extern int  drbd_bm_resize(struct drbd_device *device, sector_t sectors, int set_new_bits);
void drbd_bm_free(struct drbd_bitmap *bitmap);
extern void drbd_bm_set_all(struct drbd_device *device);
extern void drbd_bm_clear_all(struct drbd_device *device);
/* set/clear/test only a few bits at a time */
extern unsigned int drbd_bm_set_bits(struct drbd_device *, unsigned int, unsigned long, unsigned long);
extern unsigned int drbd_bm_clear_bits(struct drbd_device *, unsigned int, unsigned long, unsigned long);
extern int drbd_bm_count_bits(struct drbd_device *, unsigned int, unsigned long, unsigned long);
/* bm_set_bits variant for use while holding drbd_bm_lock,
 * may process the whole bitmap in one go */
extern void drbd_bm_set_many_bits(struct drbd_peer_device *, unsigned long, unsigned long);
extern void drbd_bm_clear_many_bits(struct drbd_peer_device *, unsigned long, unsigned long);
extern int drbd_bm_test_bit(struct drbd_peer_device *, unsigned long);
extern int drbd_bm_write_range(struct drbd_peer_device *, unsigned long, unsigned long) __must_hold(local);
extern int  drbd_bm_read(struct drbd_device *, struct drbd_peer_device *) __must_hold(local);
extern void drbd_bm_mark_range_for_writeout(struct drbd_device *, unsigned long, unsigned long);
extern int  drbd_bm_write(struct drbd_device *, struct drbd_peer_device *) __must_hold(local);
extern int  drbd_bm_write_hinted(struct drbd_device *device) __must_hold(local);
extern int drbd_bm_write_all(struct drbd_device *, struct drbd_peer_device *) __must_hold(local);
extern int drbd_bm_write_copy_pages(struct drbd_device *, struct drbd_peer_device *) __must_hold(local);
extern size_t	     drbd_bm_words(struct drbd_device *device);
extern unsigned long drbd_bm_bits(struct drbd_device *device);
extern sector_t      drbd_bm_capacity(struct drbd_device *device);

#define DRBD_END_OF_BITMAP	(~(unsigned long)0)
extern unsigned long drbd_bm_find_next(struct drbd_peer_device *, unsigned long);
/* bm_find_next variants for use while you hold drbd_bm_lock() */
extern unsigned long _drbd_bm_find_next(struct drbd_peer_device *, unsigned long);
extern unsigned long _drbd_bm_find_next_zero(struct drbd_peer_device *, unsigned long);
extern unsigned long _drbd_bm_total_weight(struct drbd_peer_device *);
extern unsigned long drbd_bm_total_weight(struct drbd_peer_device *);
extern int drbd_bm_rs_done(struct drbd_device *device);
/* for receive_bitmap */
extern void drbd_bm_merge_lel(struct drbd_peer_device *peer_device, size_t offset,
		size_t number, unsigned long *buffer);
/* for _drbd_send_bitmap */
extern void drbd_bm_get_lel(struct drbd_peer_device *peer_device, size_t offset,
		size_t number, unsigned long *buffer);

extern void drbd_bm_lock(struct drbd_device *device, char *why, enum bm_flag flags);
extern void drbd_bm_unlock(struct drbd_device *device);
extern void drbd_bm_slot_lock(struct drbd_peer_device *peer_device, char *why, enum bm_flag flags);
extern void drbd_bm_slot_unlock(struct drbd_peer_device *peer_device);
/* drbd_main.c */

/* needs to be included here,
 * because of kmem_cache_t weirdness */
#include "drbd_wrappers.h"

extern struct kmem_cache *drbd_request_cache;
extern struct kmem_cache *drbd_ee_cache;	/* peer requests */
extern struct kmem_cache *drbd_bm_ext_cache;	/* bitmap extents */
extern struct kmem_cache *drbd_al_ext_cache;	/* activity log extents */
extern mempool_t *drbd_request_mempool;
extern mempool_t *drbd_ee_mempool;

/* drbd's page pool, used to buffer data received from the peer,
 * or data requested by the peer.
 *
 * This does not have an emergency reserve.
 *
 * When allocating from this pool, it first takes pages from the pool.
 * Only if the pool is depleted will try to allocate from the system.
 *
 * The assumption is that pages taken from this pool will be processed,
 * and given back, "quickly", and then can be recycled, so we can avoid
 * frequent calls to alloc_page(), and still will be able to make progress even
 * under memory pressure.
 */
extern struct page *drbd_pp_pool;
extern spinlock_t   drbd_pp_lock;
extern int	    drbd_pp_vacant;
extern wait_queue_head_t drbd_pp_wait;

/* We also need a standard (emergency-reserve backed) page pool
 * for meta data IO (activity log, bitmap).
 * We can keep it global, as long as it is used as "N pages at a time".
 * 128 should be plenty, currently we probably can get away with as few as 1.
 */
#define DRBD_MIN_POOL_PAGES	128
extern mempool_t *drbd_md_io_page_pool;

/* We also need to make sure we get a bio
 * when we need it for housekeeping purposes */
extern struct bio_set *drbd_md_io_bio_set;
/* to allocate from that set */
extern struct bio *bio_alloc_drbd(gfp_t gfp_mask);

extern struct mutex global_state_mutex;

extern int conn_lowest_minor(struct drbd_connection *connection);
extern struct drbd_peer_device *create_peer_device(struct drbd_device *, struct drbd_connection *);
extern enum drbd_ret_code drbd_create_device(struct drbd_resource *, unsigned int, int,
					     struct device_conf *, struct drbd_device **);
extern void drbd_unregister_device(struct drbd_device *);
extern void drbd_put_device(struct drbd_device *);
extern void drbd_unregister_connection(struct drbd_connection *);
extern void drbd_put_connection(struct drbd_connection *);

extern struct drbd_resource *drbd_create_resource(const char *, struct res_opts *);
extern void drbd_free_resource(struct drbd_resource *resource);

extern int set_resource_options(struct drbd_resource *resource, struct res_opts *res_opts);
extern struct drbd_connection *drbd_create_connection(struct drbd_resource *);
extern void drbd_destroy_connection(struct kref *kref);
extern struct drbd_connection *conn_get_by_addrs(void *my_addr, int my_addr_len,
						 void *peer_addr, int peer_addr_len);
extern struct drbd_resource *drbd_find_resource(const char *name);
extern void drbd_destroy_resource(struct kref *kref);
extern void conn_free_crypto(struct drbd_connection *connection);

/* drbd_req */
extern void do_submit(struct work_struct *ws);
extern void __drbd_make_request(struct drbd_device *, struct bio *, unsigned long);
extern MAKE_REQUEST_TYPE drbd_make_request(struct request_queue *q, struct bio *bio);
extern int drbd_read_remote(struct drbd_device *device, struct drbd_request *req);
extern int drbd_merge_bvec(struct request_queue *q,
#ifdef HAVE_bvec_merge_data
		struct bvec_merge_data *bvm,
#else
		struct bio *bvm,
#endif
		struct bio_vec *bvec);
extern int is_valid_ar_handle(struct drbd_request *, sector_t);


/* drbd_nl.c */
extern void drbd_suspend_io(struct drbd_device *device);
extern void drbd_resume_io(struct drbd_device *device);
extern char *ppsize(char *buf, unsigned long long size);
extern sector_t drbd_new_dev_size(struct drbd_device *, sector_t, int);
enum determine_dev_size { DEV_SIZE_ERROR = -1, UNCHANGED = 0, SHRUNK = 1, GREW = 2 };
extern enum determine_dev_size drbd_determine_dev_size(struct drbd_device *, enum dds_flags) __must_hold(local);
extern void resync_after_online_grow(struct drbd_peer_device *);
extern void drbd_reconsider_max_bio_size(struct drbd_device *device);
extern enum drbd_state_rv drbd_set_role(struct drbd_resource *, enum drbd_role, bool);
extern bool conn_try_outdate_peer(struct drbd_connection *connection);
extern void conn_try_outdate_peer_async(struct drbd_connection *connection);
extern int drbd_khelper(struct drbd_device *, struct drbd_connection *, char *);

/* drbd_sender.c */
extern int drbd_sender(struct drbd_thread *thi);
extern int drbd_worker(struct drbd_thread *thi);
enum drbd_ret_code drbd_resync_after_valid(struct drbd_device *device, int o_minor);
void drbd_resync_after_changed(struct drbd_device *device);
extern void drbd_start_resync(struct drbd_peer_device *, enum drbd_repl_state);
extern void resume_next_sg(struct drbd_device *device);
extern void suspend_other_sg(struct drbd_device *device);
extern int drbd_resync_finished(struct drbd_peer_device *);
/* maybe rather drbd_main.c ? */
extern void *drbd_md_get_buffer(struct drbd_device *device);
extern void drbd_md_put_buffer(struct drbd_device *device);
extern int drbd_md_sync_page_io(struct drbd_device *device,
		struct drbd_backing_dev *bdev, sector_t sector, int rw);
extern void drbd_ov_out_of_sync_found(struct drbd_peer_device *, sector_t, int);
extern void wait_until_done_or_force_detached(struct drbd_device *device,
		struct drbd_backing_dev *bdev, unsigned int *done);
extern void drbd_rs_controller_reset(struct drbd_peer_device *);
extern void drbd_ping_peer(struct drbd_connection *connection);

static inline void ov_out_of_sync_print(struct drbd_peer_device *peer_device)
{
	if (peer_device->ov_last_oos_size) {
		drbd_err(peer_device, "Out of sync: start=%llu, size=%lu (sectors)\n",
		     (unsigned long long)peer_device->ov_last_oos_start,
		     (unsigned long)peer_device->ov_last_oos_size);
	}
	peer_device->ov_last_oos_size = 0;
}


extern void drbd_csum_bio(struct crypto_hash *, struct bio *, void *);
extern void drbd_csum_ee(struct crypto_hash *, struct drbd_peer_request *, void *);
/* worker callbacks */
extern int w_e_end_data_req(struct drbd_work *, int);
extern int w_e_end_rsdata_req(struct drbd_work *, int);
extern int w_e_end_csum_rs_req(struct drbd_work *, int);
extern int w_e_end_ov_reply(struct drbd_work *, int);
extern int w_e_end_ov_req(struct drbd_work *, int);
extern int w_ov_finished(struct drbd_work *, int);
extern int w_resync_timer(struct drbd_work *, int);
extern int w_send_dblock(struct drbd_work *, int);
extern int w_send_read_req(struct drbd_work *, int);
extern int w_e_reissue(struct drbd_work *, int);
extern int w_restart_disk_io(struct drbd_work *, int);
extern int w_send_out_of_sync(struct drbd_work *, int);
extern int w_start_resync(struct drbd_work *, int);

extern void resync_timer_fn(unsigned long data);
extern void start_resync_timer_fn(unsigned long data);

/* drbd_receiver.c */
extern int drbd_receiver(struct drbd_thread *thi);
extern int drbd_asender(struct drbd_thread *thi);
extern int drbd_rs_should_slow_down(struct drbd_peer_device *, sector_t);
extern int drbd_submit_peer_request(struct drbd_device *,
				    struct drbd_peer_request *, const unsigned,
				    const int);
extern int drbd_free_peer_reqs(struct drbd_device *, struct list_head *);
extern struct drbd_peer_request *drbd_alloc_peer_req(struct drbd_peer_device *, u64,
						     sector_t, unsigned int,
						     gfp_t) __must_hold(local);
extern void __drbd_free_peer_req(struct drbd_device *, struct drbd_peer_request *,
				 int);
#define drbd_free_peer_req(m,e) __drbd_free_peer_req(m, e, 0)
#define drbd_free_net_peer_req(m,e) __drbd_free_peer_req(m, e, 1)
extern struct page *drbd_alloc_pages(struct drbd_peer_device *, unsigned int, bool);
extern void drbd_set_recv_tcq(struct drbd_device *device, int tcq_enabled);
extern void _drbd_clear_done_ee(struct drbd_device *device, struct list_head *to_be_freed);
extern int drbd_connected(struct drbd_peer_device *);

/* Yes, there is kernel_setsockopt, but only since 2.6.18.
 * So we have our own copy of it here. */
static inline int drbd_setsockopt(struct socket *sock, int level, int optname,
				  char *optval, int optlen)
{
	mm_segment_t oldfs = get_fs();
	char __user *uoptval;
	int err;

	uoptval = (char __user __force *)optval;

	set_fs(KERNEL_DS);
	if (level == SOL_SOCKET)
		err = sock_setsockopt(sock, level, optname, uoptval, optlen);
	else
		err = sock->ops->setsockopt(sock, level, optname, uoptval,
					    optlen);
	set_fs(oldfs);
	return err;
}

static inline void drbd_tcp_cork(struct socket *sock)
{
	int val = 1;
	(void) drbd_setsockopt(sock, SOL_TCP, TCP_CORK,
			(char*)&val, sizeof(val));
}

static inline void drbd_tcp_uncork(struct socket *sock)
{
	int val = 0;
	(void) drbd_setsockopt(sock, SOL_TCP, TCP_CORK,
			(char*)&val, sizeof(val));
}

static inline void drbd_tcp_nodelay(struct socket *sock)
{
	int val = 1;
	(void) drbd_setsockopt(sock, SOL_TCP, TCP_NODELAY,
			(char*)&val, sizeof(val));
}

static inline void drbd_tcp_quickack(struct socket *sock)
{
	int val = 2;
	(void) drbd_setsockopt(sock, SOL_TCP, TCP_QUICKACK,
			(char*)&val, sizeof(val));
}

void drbd_bump_write_ordering(struct drbd_resource *resource, enum write_ordering_e wo);

/* drbd_proc.c */
extern struct proc_dir_entry *drbd_proc;
extern const struct file_operations drbd_proc_fops;

/* drbd_actlog.c */
extern int drbd_al_begin_io_nonblock(struct drbd_device *device, struct drbd_interval *i);
extern void drbd_al_begin_io_commit(struct drbd_device *device, bool defer_to_worker);
extern bool drbd_al_begin_io_fastpath(struct drbd_device *device, struct drbd_interval *i);
extern void drbd_al_begin_io(struct drbd_device *device, struct drbd_interval *i, bool delegate);
extern void drbd_al_complete_io(struct drbd_device *device, struct drbd_interval *i);
extern void drbd_rs_complete_io(struct drbd_peer_device *, sector_t);
extern int drbd_rs_begin_io(struct drbd_peer_device *, sector_t);
extern int drbd_try_rs_begin_io(struct drbd_peer_device *, sector_t);
extern void drbd_rs_cancel_all(struct drbd_peer_device *);
extern int drbd_rs_del_all(struct drbd_peer_device *);
extern void drbd_rs_failed_io(struct drbd_peer_device *, sector_t, int);
extern void drbd_advance_rs_marks(struct drbd_peer_device *, unsigned long);
extern void drbd_set_in_sync(struct drbd_peer_device *, sector_t, int);
extern bool drbd_set_out_of_sync(struct drbd_peer_device *, sector_t, int);
extern bool drbd_set_all_out_of_sync(struct drbd_device *, sector_t, int);
extern bool drbd_set_sync(struct drbd_device *, sector_t, int, unsigned long, unsigned long);
extern void drbd_al_shrink(struct drbd_device *device);

/* drbd_sysfs.c */
extern struct kobj_type drbd_bdev_kobj_type;

/* drbd_nl.c */

extern atomic_t drbd_notify_id;
extern atomic_t drbd_genl_seq;

extern void notify_resource_state(struct sk_buff *,
				  unsigned int,
				  struct drbd_resource *,
				  struct resource_info *,
				  enum drbd_notification_type,
				  unsigned int);
extern void notify_device_state(struct sk_buff *,
				unsigned int,
				struct drbd_device *,
				struct device_info *,
				enum drbd_notification_type,
				unsigned int);
extern void notify_connection_state(struct sk_buff *,
				    unsigned int,
				    struct drbd_connection *,
				    struct connection_info *,
				    enum drbd_notification_type,
				    unsigned int);
extern void notify_peer_device_state(struct sk_buff *,
				     unsigned int,
				     struct drbd_peer_device *,
				     struct peer_device_info *,
				     enum drbd_notification_type,
				     unsigned int);
extern void notify_helper(enum drbd_notification_type, struct drbd_device *,
			  struct drbd_connection *, const char *, int);

/*
 * inline helper functions
 *************************/

/* see also page_chain_add and friends in drbd_receiver.c */
static inline struct page *page_chain_next(struct page *page)
{
	return (struct page *)page_private(page);
}
#define page_chain_for_each(page) \
	for (; page && ({ prefetch(page_chain_next(page)); 1; }); \
			page = page_chain_next(page))
#define page_chain_for_each_safe(page, n) \
	for (; page && ({ n = page_chain_next(page); 1; }); page = n)


static inline int drbd_peer_req_has_active_page(struct drbd_peer_request *peer_req)
{
	struct page *page = peer_req->pages;
	page_chain_for_each(page) {
		if (page_count(page) > 1)
			return 1;
	}
	return 0;
}

/*
 * When a device has a replication state above L_OFF, it must be
 * connected.  Otherwise, we report the connection state, which has values up
 * to C_CONNECTED == L_OFF.
 */
static inline int combined_conn_state(struct drbd_peer_device *peer_device, enum which_state which)
{
	enum drbd_repl_state repl_state = peer_device->repl_state[which];

	if (repl_state > L_OFF)
		return repl_state;
	else
		return peer_device->connection->cstate[which];
}

enum drbd_force_detach_flags {
	DRBD_READ_ERROR,
	DRBD_WRITE_ERROR,
	DRBD_META_IO_ERROR,
	DRBD_FORCE_DETACH,
};

#define __drbd_chk_io_error(m,f) __drbd_chk_io_error_(m,f, __func__)
static inline void __drbd_chk_io_error_(struct drbd_device *device,
					enum drbd_force_detach_flags df,
					const char *where)
{
	enum drbd_io_error_p ep;

	rcu_read_lock();
	ep = rcu_dereference(device->ldev->disk_conf)->on_io_error;
	rcu_read_unlock();
	switch (ep) {
	case EP_PASS_ON: /* FIXME would this be better named "Ignore"? */
		if (df == DRBD_READ_ERROR ||  df == DRBD_WRITE_ERROR) {
			if (drbd_ratelimit())
				drbd_err(device, "Local IO failed in %s.\n", where);
			if (device->disk_state[NOW] > D_INCONSISTENT) {
				begin_state_change_locked(device->resource, CS_HARD);
				__change_disk_state(device, D_INCONSISTENT);
				end_state_change_locked(device->resource);
			}
			break;
		}
		/* NOTE fall through for DRBD_META_IO_ERROR or DRBD_FORCE_DETACH */
	case EP_DETACH:
	case EP_CALL_HELPER:
		/* Remember whether we saw a READ or WRITE error.
		 *
		 * Recovery of the affected area for WRITE failure is covered
		 * by the activity log.
		 * READ errors may fall outside that area though. Certain READ
		 * errors can be "healed" by writing good data to the affected
		 * blocks, which triggers block re-allocation in lower layers.
		 *
		 * If we can not write the bitmap after a READ error,
		 * we may need to trigger a full sync (see w_go_diskless()).
		 *
		 * Force-detach is not really an IO error, but rather a
		 * desperate measure to try to deal with a completely
		 * unresponsive lower level IO stack.
		 * Still it should be treated as a WRITE error.
		 *
		 * Meta IO error is always WRITE error:
		 * we read meta data only once during attach,
		 * which will fail in case of errors.
		 */
		set_bit(WAS_IO_ERROR, &device->flags);
		if (df == DRBD_READ_ERROR)
			set_bit(WAS_READ_ERROR, &device->flags);
		if (df == DRBD_FORCE_DETACH)
			set_bit(FORCE_DETACH, &device->flags);
		if (device->disk_state[NOW] > D_FAILED) {
			begin_state_change_locked(device->resource, CS_HARD);
			__change_disk_state(device, D_FAILED);
			end_state_change_locked(device->resource);
			drbd_err(device,
				"Local IO failed in %s. Detaching...\n", where);
		}
		break;
	}
}

/**
 * drbd_chk_io_error: Handle the on_io_error setting, should be called from all io completion handlers
 * @device:	 DRBD device.
 * @error:	 Error code passed to the IO completion callback
 * @forcedetach: Force detach. I.e. the error happened while accessing the meta data
 *
 * See also drbd_main.c:after_state_ch() if (os.disk > D_FAILED && ns.disk == D_FAILED)
 */
#define drbd_chk_io_error(m,e,f) drbd_chk_io_error_(m,e,f, __func__)
static inline void drbd_chk_io_error_(struct drbd_device *device,
	int error, enum drbd_force_detach_flags forcedetach, const char *where)
{
	if (error) {
		unsigned long flags;
		spin_lock_irqsave(&device->resource->req_lock, flags);
		__drbd_chk_io_error_(device, forcedetach, where);
		spin_unlock_irqrestore(&device->resource->req_lock, flags);
	}
}


/**
 * drbd_md_first_sector() - Returns the first sector number of the meta data area
 * @bdev:	Meta data block device.
 *
 * BTW, for internal meta data, this happens to be the maximum capacity
 * we could agree upon with our peer node.
 */
static inline sector_t drbd_md_first_sector(struct drbd_backing_dev *bdev)
{
	switch (bdev->md.meta_dev_idx) {
	case DRBD_MD_INDEX_INTERNAL:
	case DRBD_MD_INDEX_FLEX_INT:
		return bdev->md.md_offset + bdev->md.bm_offset;
	case DRBD_MD_INDEX_FLEX_EXT:
	default:
		return bdev->md.md_offset;
	}
}

/**
 * drbd_md_last_sector() - Return the last sector number of the meta data area
 * @bdev:	Meta data block device.
 */
static inline sector_t drbd_md_last_sector(struct drbd_backing_dev *bdev)
{
	switch (bdev->md.meta_dev_idx) {
	case DRBD_MD_INDEX_INTERNAL:
	case DRBD_MD_INDEX_FLEX_INT:
		return bdev->md.md_offset + MD_4kB_SECT -1;
	case DRBD_MD_INDEX_FLEX_EXT:
	default:
		return bdev->md.md_offset + bdev->md.md_size_sect -1;
	}
}

/**
 * drbd_get_max_capacity() - Returns the capacity we announce to out peer
 * @bdev:	Meta data block device.
 *
 * returns the capacity we announce to out peer.  we clip ourselves at the
 * various MAX_SECTORS, because if we don't, current implementation will
 * oops sooner or later
 */
static inline sector_t drbd_get_max_capacity(struct drbd_backing_dev *bdev)
{
	sector_t s;

	switch (bdev->md.meta_dev_idx) {
	case DRBD_MD_INDEX_INTERNAL:
	case DRBD_MD_INDEX_FLEX_INT:
		s = drbd_get_capacity(bdev->backing_bdev)
			? min_t(sector_t, DRBD_MAX_SECTORS_FLEX,
				drbd_md_first_sector(bdev))
			: 0;
		break;
	case DRBD_MD_INDEX_FLEX_EXT:
		s = min_t(sector_t, DRBD_MAX_SECTORS_FLEX,
				drbd_get_capacity(bdev->backing_bdev));
		/* clip at maximum size the meta device can support */
		s = min_t(sector_t, s,
			BM_EXT_TO_SECT(bdev->md.md_size_sect
				     - bdev->md.bm_offset));
		break;
	default:
		s = min_t(sector_t, DRBD_MAX_SECTORS,
				drbd_get_capacity(bdev->backing_bdev));
	}
	return s;
}

/**
 * drbd_md_ss() - Return the sector number of our meta data super block
 * @bdev:	Meta data block device.
 */
static inline sector_t drbd_md_ss(struct drbd_backing_dev *bdev)
{
	const int meta_dev_idx = bdev->md.meta_dev_idx;

	if (meta_dev_idx == DRBD_MD_INDEX_FLEX_EXT)
		return 0;

	/* Since drbd08, internal meta data is always "flexible".
	 * position: last 4k aligned block of 4k size */
	if (meta_dev_idx == DRBD_MD_INDEX_INTERNAL ||
	    meta_dev_idx == DRBD_MD_INDEX_FLEX_INT)
		return (drbd_get_capacity(bdev->backing_bdev) & ~7ULL) - 8;

	/* external, some index; this is the old fixed size layout */
	return MD_128MB_SECT * bdev->md.meta_dev_idx;
}

static inline void
drbd_queue_work(struct drbd_work_queue *q, struct drbd_work *w)
{
	unsigned long flags;
	spin_lock_irqsave(&q->q_lock, flags);
	list_add_tail(&w->list, &q->q);
	spin_unlock_irqrestore(&q->q_lock, flags);
	wake_up(&q->q_wait);
}

extern void drbd_flush_workqueue(struct drbd_work_queue *work_queue);

static inline void wake_asender(struct drbd_connection *connection)
{
	if (test_and_clear_bit(SIGNAL_ASENDER, &connection->flags))
		force_sig(DRBD_SIG, connection->asender.task);
}

static inline void request_ping(struct drbd_connection *connection)
{
	set_bit(SEND_PING, &connection->flags);
	wake_asender(connection);
}

extern void *conn_prepare_command(struct drbd_connection *, struct drbd_socket *);
extern void *drbd_prepare_command(struct drbd_peer_device *, struct drbd_socket *);
extern int conn_send_command(struct drbd_connection *, struct drbd_socket *,
			     enum drbd_packet, unsigned int, void *,
			     unsigned int);
extern int drbd_send_command(struct drbd_peer_device *, struct drbd_socket *,
			     enum drbd_packet, unsigned int, void *,
			     unsigned int);

extern int drbd_send_ping(struct drbd_connection *connection);
extern int drbd_send_ping_ack(struct drbd_connection *connection);
extern int conn_send_state_req(struct drbd_connection *, int vnr, enum drbd_packet, union drbd_state, union drbd_state);
extern int drbd_send_peer_ack(struct drbd_connection *, struct drbd_request *);

static inline void drbd_thread_stop(struct drbd_thread *thi)
{
	_drbd_thread_stop(thi, false, true);
}

static inline void drbd_thread_stop_nowait(struct drbd_thread *thi)
{
	_drbd_thread_stop(thi, false, false);
}

static inline void drbd_thread_restart_nowait(struct drbd_thread *thi)
{
	_drbd_thread_stop(thi, true, false);
}

/* counts how many answer packets packets we expect from our peer,
 * for either explicit application requests,
 * or implicit barrier packets as necessary.
 * increased:
 *  w_send_barrier
 *  _req_mod(req, QUEUE_FOR_NET_WRITE or QUEUE_FOR_NET_READ);
 *    it is much easier and equally valid to count what we queue for the
 *    sender, even before it actually was queued or sent.
 *    (drbd_make_request_common; recovery path on read io-error)
 * decreased:
 *  got_BarrierAck (respective tl_clear, tl_clear_barrier)
 *  _req_mod(req, DATA_RECEIVED)
 *     [from receive_DataReply]
 *  _req_mod(req, WRITE_ACKED_BY_PEER or RECV_ACKED_BY_PEER or NEG_ACKED)
 *     [from got_BlockAck (P_WRITE_ACK, P_RECV_ACK)]
 *     FIXME
 *     for some reason it is NOT decreased in got_NegAck,
 *     but in the resulting cleanup code from report_params.
 *     we should try to remember the reason for that...
 *  _req_mod(req, SEND_FAILED or SEND_CANCELED)
 *  _req_mod(req, CONNECTION_LOST_WHILE_PENDING)
 *     [from tl_clear_barrier]
 */
static inline void inc_ap_pending(struct drbd_peer_device *peer_device)
{
	atomic_inc(&peer_device->ap_pending_cnt);
}

#define dec_ap_pending(peer_device) \
	((void)expect((peer_device), __dec_ap_pending(peer_device) >= 0))
static inline int __dec_ap_pending(struct drbd_peer_device *peer_device)
{
	int ap_pending_cnt = atomic_dec_return(&peer_device->ap_pending_cnt);
	if (ap_pending_cnt == 0)
		wake_up(&peer_device->device->misc_wait);
	return ap_pending_cnt;
}

/* counts how many resync-related answers we still expect from the peer
 *		     increase			decrease
 * L_SYNC_TARGET sends P_RS_DATA_REQUEST (and expects P_RS_DATA_REPLY)
 * L_SYNC_SOURCE sends P_RS_DATA_REPLY   (and expects P_WRITE_ACK with ID_SYNCER)
 *					   (or P_NEG_ACK with ID_SYNCER)
 */
static inline void inc_rs_pending(struct drbd_peer_device *peer_device)
{
	atomic_inc(&peer_device->rs_pending_cnt);
}

#define dec_rs_pending(peer_device) \
	((void)expect((peer_device), __dec_rs_pending(peer_device) >= 0))
static inline int __dec_rs_pending(struct drbd_peer_device *peer_device)
{
	return atomic_dec_return(&peer_device->rs_pending_cnt);
}

/* counts how many answers we still need to send to the peer.
 * increased on
 *  receive_Data	unless protocol A;
 *			we need to send a P_RECV_ACK (proto B)
 *			or P_WRITE_ACK (proto C)
 *  receive_RSDataReply (recv_resync_read) we need to send a P_WRITE_ACK
 *  receive_DataRequest (receive_RSDataRequest) we need to send back P_DATA
 *  receive_Barrier_*	we need to send a P_BARRIER_ACK
 */
static inline void inc_unacked(struct drbd_peer_device *peer_device)
{
	atomic_inc(&peer_device->unacked_cnt);
}

#define dec_unacked(peer_device) \
	((void)expect(peer_device, __dec_unacked(peer_device) >= 0))
static inline int __dec_unacked(struct drbd_peer_device *peer_device)
{
	return atomic_dec_return(&peer_device->unacked_cnt);
}

#define sub_unacked(peer_device, n) \
	((void)expect(peer_device, __sub_unacked(peer_device) >= 0))
static inline int __sub_unacked(struct drbd_peer_device *peer_device, int n)
{
	return atomic_sub_return(n, &peer_device->unacked_cnt);
}

/**
 * get_ldev() - Increase the ref count on device->ldev. Returns 0 if there is no ldev
 * @M:		DRBD device.
 *
 * You have to call put_ldev() when finished working with device->ldev.
 */
#define get_ldev(M) __cond_lock(local, _get_ldev_if_state(M,D_INCONSISTENT))
#define get_ldev_if_state(M,MINS) __cond_lock(local, _get_ldev_if_state(M,MINS))

static inline void put_ldev(struct drbd_device *device)
{
	int i = atomic_dec_return(&device->local_cnt);

	/* This may be called from some endio handler,
	 * so we must not sleep here. */

	__release(local);
	D_ASSERT(device, i >= 0);
	if (i == 0) {
		if (device->disk_state[NOW] == D_DISKLESS)
			/* even internal references gone, safe to destroy */
			drbd_ldev_destroy(device);
		if (device->disk_state[NOW] == D_FAILED)
			/* all application IO references gone. */
			drbd_go_diskless(device);
		wake_up(&device->misc_wait);
	}
}

#ifndef __CHECKER__
static inline int _get_ldev_if_state(struct drbd_device *device, enum drbd_disk_state mins)
{
	int io_allowed;

	/* never get a reference while D_DISKLESS */
	if (device->disk_state[NOW] == D_DISKLESS)
		return 0;

	atomic_inc(&device->local_cnt);
	io_allowed = (device->disk_state[NOW] >= mins);
	if (!io_allowed)
		put_ldev(device);
	return io_allowed;
}
#else
extern int _get_ldev_if_state(struct drbd_device *device, enum drbd_disk_state mins);
#endif

static inline bool drbd_state_is_stable(struct drbd_device *device)
{
	struct drbd_peer_device *peer_device;
	bool stable = true;

	/* DO NOT add a default clause, we want the compiler to warn us
	 * for any newly introduced state we may have forgotten to add here */

	rcu_read_lock();
	for_each_peer_device(peer_device, device) {
		switch (peer_device->repl_state[NOW]) {
		/* New io is only accepted when the peer device is unknown or there is
		 * a well-established connection. */
		case L_OFF:
		case L_ESTABLISHED:
		case L_SYNC_SOURCE:
		case L_SYNC_TARGET:
		case L_VERIFY_S:
		case L_VERIFY_T:
		case L_PAUSED_SYNC_S:
		case L_PAUSED_SYNC_T:
		case L_AHEAD:
		case L_BEHIND:
		case L_STARTING_SYNC_S:
		case L_STARTING_SYNC_T:
			break;

			/* Allow IO in BM exchange states with new protocols */
		case L_WF_BITMAP_S:
			if (peer_device->connection->agreed_pro_version < 96)
				stable = false;
			break;

			/* no new io accepted in these states */
		case L_WF_BITMAP_T:
		case L_WF_SYNC_UUID:
			stable = false;
			break;
		}
		if (!stable)
			break;
	}
	rcu_read_unlock();

	switch (device->disk_state[NOW]) {
	case D_DISKLESS:
	case D_INCONSISTENT:
	case D_OUTDATED:
	case D_CONSISTENT:
	case D_UP_TO_DATE:
	case D_FAILED:
		/* disk state is stable as well. */
		break;

	/* no new io accepted during transitional states */
	case D_ATTACHING:
	case D_NEGOTIATING:
	case D_UNKNOWN:
	case D_MASK:
		stable = false;
	}

	return stable;
}

extern void drbd_queue_pending_bitmap_work(struct drbd_device *);

static inline void dec_ap_bio(struct drbd_device *device)
{
	int ap_bio = atomic_dec_return(&device->ap_bio_cnt);

	D_ASSERT(device, ap_bio >= 0);

	if (ap_bio == 0) {
		smp_rmb();
		if (!list_empty(&device->pending_bitmap_work))
			drbd_queue_pending_bitmap_work(device);
	}

	/* this currently does wake_up for every dec_ap_bio!
	 * maybe rather introduce some type of hysteresis?
	 * e.g. (ap_bio == max_buffers/2 || ap_bio == 0) ? */
	if (ap_bio < device->device_conf.max_buffers)
		wake_up(&device->misc_wait);
}

static inline int drbd_suspended(struct drbd_device *device)
{
	struct drbd_resource *resource = device->resource;

	return resource->susp[NOW] || resource->susp_fen[NOW] || resource->susp_nod[NOW];
}

static inline bool may_inc_ap_bio(struct drbd_device *device)
{
	if (drbd_suspended(device))
		return false;
	if (test_bit(SUSPEND_IO, &device->flags))
		return false;

	/* to avoid potential deadlock or bitmap corruption,
	 * in various places, we only allow new application io
	 * to start during "stable" states. */

	/* no new io accepted when attaching or detaching the disk */
	if (!drbd_state_is_stable(device))
		return false;

	/* since some older kernels don't have atomic_add_unless,
	 * and we are within the spinlock anyways, we have this workaround.  */
	if (atomic_read(&device->ap_bio_cnt) > device->device_conf.max_buffers)
		return false;
	if (!list_empty(&device->pending_bitmap_work))
		return false;
	return true;
}

static inline bool inc_ap_bio_cond(struct drbd_device *device)
{
	bool rv = false;

	spin_lock_irq(&device->resource->req_lock);
	rv = may_inc_ap_bio(device);
	if (rv)
		atomic_inc(&device->ap_bio_cnt);
	spin_unlock_irq(&device->resource->req_lock);

	return rv;
}

static inline void inc_ap_bio(struct drbd_device *device)
{
	/* we wait here
	 *    as long as the device is suspended
	 *    until the bitmap is no longer on the fly during connection
	 *    handshake as long as we would exceed the max_buffer limit.
	 *
	 * to avoid races with the reconnect code,
	 * we need to atomic_inc within the spinlock. */

	wait_event(device->misc_wait, inc_ap_bio_cond(device));
}

static inline int drbd_set_exposed_data_uuid(struct drbd_device *device, u64 val)
{
	int changed = device->exposed_data_uuid != val;
	device->exposed_data_uuid = val;
	return changed;
}

static inline u64 drbd_current_uuid(struct drbd_device *device)
{
	if (!device->ldev)
		return 0;
	return device->ldev->md.current_uuid;
}

static inline bool verify_can_do_stop_sector(struct drbd_peer_device *peer_device)
{
	return peer_device->connection->agreed_pro_version >= 97 &&
		peer_device->connection->agreed_pro_version != 100;
}

static inline u64 drbd_bitmap_uuid(struct drbd_peer_device *peer_device)
{
	struct drbd_device *device = peer_device->device;
	struct drbd_peer_md *peer_md;

	if (!device->ldev)
		return 0;

	peer_md = &device->ldev->md.peers[peer_device->bitmap_index];
	return peer_md->bitmap_uuid;
}

static inline u64 drbd_history_uuid(struct drbd_device *device, int i)
{
	if (!device->ldev || i >= ARRAY_SIZE(device->ldev->md.history_uuids))
		return 0;

	return device->ldev->md.history_uuids[i];
}

static inline int drbd_queue_order_type(struct drbd_device *device)
{
	/* sorry, we currently have no working implementation
	 * of distributed TCQ stuff */
#ifndef QUEUE_ORDERED_NONE
#define QUEUE_ORDERED_NONE 0
#endif
	return QUEUE_ORDERED_NONE;
}

#ifdef blk_queue_plugged
static inline void drbd_blk_run_queue(struct request_queue *q)
{
	if (q && q->unplug_fn)
		q->unplug_fn(q);
}

static inline void drbd_kick_lo(struct drbd_device *device)
{
	if (get_ldev(device)) {
		drbd_blk_run_queue(bdev_get_queue(device->ldev->backing_bdev));
		put_ldev(device);
	}
}
#else
static inline void drbd_blk_run_queue(struct request_queue *q)
{
}
static inline void drbd_kick_lo(struct drbd_device *device)
{
}
#endif

static inline void drbd_md_flush(struct drbd_device *device)
{
	int r;

	if (device->ldev == NULL) {
		drbd_warn(device, "device->ldev == NULL in drbd_md_flush\n");
		return;
	}

	if (test_bit(MD_NO_BARRIER, &device->flags))
		return;

	r = blkdev_issue_flush(device->ldev->md_bdev, GFP_NOIO, NULL);
	if (r) {
		set_bit(MD_NO_BARRIER, &device->flags);
		drbd_err(device, "meta data flush failed with status %d, disabling md-flushes\n", r);
	}
}

/* resync bitmap */
/* 16MB sized 'bitmap extent' to track syncer usage */
struct bm_extent {
	int rs_left; /* number of bits set (out of sync) in this extent. */
	int rs_failed; /* number of failed resync requests in this extent. */
	unsigned long flags;
	struct lc_element lce;
};

#define BME_NO_WRITES  0  /* bm_extent.flags: no more requests on this one! */
#define BME_LOCKED     1  /* bm_extent.flags: syncer active on this one. */
#define BME_PRIORITY   2  /* finish resync IO on this extent ASAP! App IO waiting! */

/* should be moved to idr.h */
/**
 * idr_for_each_entry - iterate over an idr's elements of a given type
 * @idp:     idr handle
 * @entry:   the type * to use as cursor
 * @id:      id entry's key
 */
#define idr_for_each_entry(idp, entry, id)				\
	for (id = 0, entry = (typeof(entry))idr_get_next((idp), &(id)); \
	     entry != NULL;						\
	     ++id, entry = (typeof(entry))idr_get_next((idp), &(id)))

#define idr_for_each_entry_continue(idp, entry, id)			\
	for (entry = (typeof(entry))idr_get_next((idp), &(id));		\
	     entry;							\
	     ++id, entry = (typeof(entry))idr_get_next((idp), &(id)))

static inline struct drbd_connection *first_connection(struct drbd_resource *resource)
{
	return list_first_entry(&resource->connections, struct drbd_connection, connections);
}

#endif
