/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_CURSOR_LSM --
 *	An LSM cursor.
 */
struct __wt_cursor_lsm {
	WT_CURSOR iface;

	WT_LSM_TREE *lsm_tree;
	uint64_t dsk_gen;

	u_int nchunks;			/* Number of chunks in the cursor */
	u_int nupdates;			/* Updates needed (including
					   snapshot isolation checks). */
	WT_BLOOM **blooms;		/* Bloom filter handles. */
	size_t bloom_alloc;

	WT_CURSOR **cursors;		/* Cursor handles. */
	size_t cursor_alloc;

	WT_CURSOR *current;     	/* The current cursor for iteration */
	WT_LSM_CHUNK *primary_chunk;	/* The current primary chunk */

	uint64_t *switch_txn;		/* Switch txn for each chunk */
	size_t txnid_alloc;

	u_int update_count;		/* Updates performed. */

#define	WT_CLSM_ACTIVE		0x01    /* Incremented the session count */
#define	WT_CLSM_ITERATE_NEXT    0x02    /* Forward iteration */
#define	WT_CLSM_ITERATE_PREV    0x04    /* Backward iteration */
#define	WT_CLSM_MERGE           0x08    /* Merge cursor, don't update */
#define	WT_CLSM_MINOR_MERGE	0x10    /* Minor merge, include tombstones */
#define	WT_CLSM_MULTIPLE        0x20    /* Multiple cursors have values for the
					   current key */
#define	WT_CLSM_OPEN_READ	0x40    /* Open for reads */
#define	WT_CLSM_OPEN_SNAPSHOT	0x80    /* Open for snapshot isolation */
	uint32_t flags;
};

/*
 * WT_LSM_CHUNK --
 *	A single chunk (file) in an LSM tree.
 */
struct __wt_lsm_chunk {
	const char *uri;		/* Data source for this chunk */
	const char *bloom_uri;		/* URI of Bloom filter, if any */
	struct timespec create_ts;	/* Creation time (for rate limiting) */
	uint64_t count;			/* Approximate count of records */
	uint64_t size;			/* Final chunk size */

	uint64_t switch_txn;		/*
					 * Largest transaction that can write
					 * to this chunk, set by a worker
					 * thread when the chunk is switched
					 * out, or by compact to get the most
					 * recent chunk flushed.
					 */

	uint32_t id;			/* ID used to generate URIs */
	uint32_t generation;		/* Merge generation */
	uint32_t refcnt;		/* Number of worker thread references */
	uint32_t bloom_busy;		/* Number of worker thread references */

	int8_t empty;			/* 1/0: checkpoint missing */
	int8_t evicted;			/* 1/0: in-memory chunk was evicted */

#define	WT_LSM_CHUNK_BLOOM	0x01
#define	WT_LSM_CHUNK_MERGING	0x02
#define	WT_LSM_CHUNK_ONDISK	0x04
#define	WT_LSM_CHUNK_STABLE	0x08
	uint32_t flags;
} WT_GCC_ATTRIBUTE((aligned(WT_CACHE_LINE_ALIGNMENT)));

/*
 * Different types of work units. Used by LSM worker threads to choose which
 * type of work they will execute, and by work units to define which action
 * is required.
 */
#define	WT_LSM_WORK_BLOOM	0x01	/* Create a bloom filter */
#define	WT_LSM_WORK_DROP	0x02	/* Drop unused chunks */
#define	WT_LSM_WORK_FLUSH	0x04	/* Flush a chunk to disk */
#define	WT_LSM_WORK_MERGE	0x08	/* Look for a tree merge */
#define	WT_LSM_WORK_SWITCH	0x10	/* Switch to a new in memory chunk */
#define	WT_LSM_WORK_FORCE	0x10000	/* Force last chunk flush */
#define	WT_LSM_WORK_MASK	0xffff	/* Mask for work types */

/*
 * WT_LSM_WORK_UNIT --
 *	A definition of maintenance that an LSM tree needs done.
 */
struct __wt_lsm_work_unit {
	TAILQ_ENTRY(__wt_lsm_work_unit) q;	/* Worker unit queue */
	uint32_t flags;				/* The type of operation */
	WT_LSM_TREE *lsm_tree;
};

/*
 * WT_LSM_MANAGER --
 *	A structure that holds resources used to manage any LSM trees in a
 *	database.
 */
struct __wt_lsm_manager {
	/*
	 * Queues of work units for LSM worker threads. We maintain three
	 * queues, to allow us to keep each queue FIFO, rather than needing
	 * to manage the order of work by shuffling the queue order.
	 * One queue for switches - since switches should never wait for other
	 *   work to be done.
	 * One queue for application requested work. For example flushing
	 *   and creating bloom filters.
	 * One queue that is for longer running operations such as merges.
	 */
	TAILQ_HEAD(__wt_lsm_work_switch_qh, __wt_lsm_work_unit)  switchqh;
	TAILQ_HEAD(__wt_lsm_work_app_qh, __wt_lsm_work_unit)	  appqh;
	TAILQ_HEAD(__wt_lsm_work_manager_qh, __wt_lsm_work_unit) managerqh;
	WT_SPINLOCK	switch_lock;	/* Lock for switch queue */
	WT_SPINLOCK	app_lock;	/* Lock for application queue */
	WT_SPINLOCK	manager_lock;	/* Lock for manager queue */
	WT_CONDVAR     *work_cond;	/* Used to notify worker of activity */
	uint32_t	lsm_workers;	/* Current number of LSM workers */
	uint32_t	lsm_workers_max;
	WT_LSM_WORKER_ARGS *lsm_worker_cookies;
};

/*
 * WT_LSM_TREE --
 *	An LSM tree.
 */
struct __wt_lsm_tree {
	const char *name, *config, *filename;
	const char *key_format, *value_format;
	const char *bloom_config, *file_config;

	WT_COLLATOR *collator;
	const char *collator_name;

	int refcnt;			/* Number of users of the tree */
	int queue_ref;
	WT_RWLOCK *rwlock;
	TAILQ_ENTRY(__wt_lsm_tree) q;

	WT_DSRC_STATS stats;		/* LSM-level statistics */

	uint64_t dsk_gen;

	long ckpt_throttle;		/* Rate limiting due to checkpoints */
	long merge_throttle;		/* Rate limiting due to merges */
	uint64_t chunk_fill_ms;		/* Estimate of time to fill a chunk */
	struct timespec last_flush_ts;	/* Timestamp last flush finished */
	struct timespec work_push_ts;	/* Timestamp last work unit added */
	uint64_t merge_progressing;	/* Bumped when merges are active */

	/* Configuration parameters */
	uint32_t bloom_bit_count;
	uint32_t bloom_hash_count;
	uint64_t chunk_size;
	uint64_t chunk_max;
	u_int merge_min, merge_max;
	u_int merge_threads;

	u_int merge_idle;		/* Count of idle merge threads */

#define	WT_LSM_BLOOM_MERGED				0x00000001
#define	WT_LSM_BLOOM_OFF				0x00000002
#define	WT_LSM_BLOOM_OLDEST				0x00000004
	uint32_t bloom;			/* Bloom creation policy */

#define	WT_LSM_MAX_WORKERS	10
					/* Passed to thread_create */
	WT_SESSION_IMPL *worker_sessions[WT_LSM_MAX_WORKERS];
					/* LSM worker thread(s) */
	pthread_t worker_tids[WT_LSM_MAX_WORKERS];
	WT_SESSION_IMPL *ckpt_session;	/* For checkpoint worker */
	pthread_t ckpt_tid;		/* LSM checkpoint worker thread */

	WT_LSM_CHUNK **chunk;		/* Array of active LSM chunks */
	size_t chunk_alloc;		/* Space allocated for chunks */
	u_int nchunks;			/* Number of active chunks */
	uint32_t last;			/* Last allocated ID */
	int modified;			/* Have there been updates? */

	WT_LSM_CHUNK **old_chunks;	/* Array of old LSM chunks */
	size_t old_alloc;		/* Space allocated for old chunks */
	u_int nold_chunks;		/* Number of old chunks */
	int freeing_old_chunks;		/* Whether chunks are being freed */
	uint32_t merge_aggressiveness;	/* Increase amount of work per merge */

#define	WT_LSM_TREE_ACTIVE		0x01	/* Workers are active */
#define	WT_LSM_TREE_COMPACTING		0x02	/* Tree being compacted */
#define	WT_LSM_TREE_NEED_SWITCH		0x04	/* New chunk needs creating */
#define	WT_LSM_TREE_OPEN		0x08	/* The tree is open */
#define	WT_LSM_TREE_THROTTLE		0x10	/* Throttle updates */
	uint32_t flags;

#define	WT_LSM_TREE_EXCLUSIVE	0x01	/* Tree is opened exclusively */
	uint32_t flags_atomic;
};

/*
 * WT_LSM_DATA_SOURCE --
 *	Implementation of the WT_DATA_SOURCE interface for LSM.
 */
struct __wt_lsm_data_source {
	WT_DATA_SOURCE iface;

	WT_RWLOCK *rwlock;
};

/*
 * WT_LSM_WORKER_COOKIE --
 *	State for an LSM worker thread.
 */
struct __wt_lsm_worker_cookie {
	WT_LSM_CHUNK **chunk_array;
	size_t chunk_alloc;
	u_int nchunks;
};

/*
 * WT_LSM_WORKER_ARGS --
 *	State for an LSM worker thread.
 */
struct __wt_lsm_worker_args {
	WT_SESSION_IMPL *session;
	WT_CONDVAR *work_cond;		/* Owned by the manager */
	pthread_t tid;
	u_int id;
	uint32_t flags;
};
