/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "wal.h"

#include "vclock.h"
#include "fiber.h"
#include "fio.h"
#include "errinj.h"
#include "error.h"
#include "exception.h"
#include "sio.h"
#include "coio.h"

#include "xlog.h"
#include "xrow.h"
#include "xrow_io.h"
#include "vy_log.h"
#include "cbus.h"
#include "coio_task.h"
#include "replication.h"
#include "recovery.h"
#include "relay.h"
#include "iproto_constants.h"

enum {
	/**
	 * Size of disk space to preallocate with xlog_fallocate().
	 * Obviously, we want to call this function as infrequent as
	 * possible to avoid the overhead associated with a system
	 * call, however at the same time we do not want to call it
	 * to allocate too big chunks, because this may increase tx
	 * latency. 1 MB seems to be a well balanced choice.
	 */
	WAL_FALLOCATE_LEN = 1024 * 1024,
	/** Wal memory threshold. */
	WAL_MEMORY_THRESHOLD = 4 * 1024 * 1024,
	/** Wal memory chunk threshold. */
	WAL_MEM_CHUNK_THRESHOLD = 16 * 1024,
};

const char *wal_mode_STRS[] = { "none", "write", "fsync", NULL };

int wal_dir_lock = -1;

static int64_t
wal_write(struct journal *, struct journal_entry *);

static int64_t
wal_write_in_wal_mode_none(struct journal *, struct journal_entry *);

/* WAL thread. */
struct wal_thread {
	/** 'wal' thread doing the writes. */
	struct cord cord;
	/** A pipe from 'tx' thread to 'wal' */
	struct cpipe wal_pipe;
	/**
	 * Return pipe from 'wal' to tx'. This is a
	 * priority pipe and DOES NOT support yield.
	 */
	struct cpipe tx_prio_pipe;
	struct rlist relay;
};

/*
 * WAL writer - maintain a Write Ahead Log for every change
 * in the data state.
 *
 * @sic the members are arranged to ensure proper cache alignment,
 * members used mainly in tx thread go first, wal thread members
 * following.
 */
struct wal_writer
{
	struct journal base;
	/* ----------------- tx ------------------- */
	wal_on_garbage_collection_f on_garbage_collection;
	wal_on_checkpoint_threshold_f on_checkpoint_threshold;
	/**
	 * The rollback queue. An accumulator for all requests
	 * that need to be rolled back. Also acts as a valve
	 * in wal_write() so that new requests never enter
	 * the wal-tx bus and are rolled back "on arrival".
	 */
	struct stailq rollback;
	/* ----------------- wal ------------------- */
	/** A setting from instance configuration - rows_per_wal */
	int64_t wal_max_rows;
	/** A setting from instance configuration - wal_max_size */
	int64_t wal_max_size;
	/** Another one - wal_mode */
	enum wal_mode wal_mode;
	/** wal_dir, from the configuration file. */
	struct xdir wal_dir;
	/**
	 * The vector clock of the WAL writer. It's a bit behind
	 * the vector clock of the transaction thread, since it
	 * "follows" the tx vector clock.
	 * By "following" we mean this: whenever a transaction
	 * is started in 'tx' thread, it's assigned a tentative
	 * LSN. If the transaction is rolled back, this LSN
	 * is abandoned. Otherwise, after the transaction is written
	 * to the log with this LSN, WAL writer vclock is advanced
	 * with this LSN and LSN becomes "real".
	 */
	struct vclock vclock;
	/**
	 * VClock of the most recent successfully created checkpoint.
	 * The WAL writer must not delete WAL files that are needed to
	 * recover from it even if it is running out of disk space.
	 */
	struct vclock checkpoint_vclock;
	/** Total size of WAL files written since the last checkpoint. */
	int64_t checkpoint_wal_size;
	/**
	 * Checkpoint threshold: when the total size of WAL files
	 * written since the last checkpoint exceeds the value of
	 * this variable, the WAL thread will notify TX that it's
	 * time to trigger checkpointing.
	 */
	int64_t checkpoint_threshold;
	/**
	 * This flag is set if the WAL thread has notified TX that
	 * the checkpoint threshold has been exceeded. It is cleared
	 * on checkpoint completion. Needed in order not to invoke
	 * the TX callback over and over again while checkpointing
	 * is in progress.
	 */
	bool checkpoint_triggered;
	/** The current WAL file. */
	struct xlog current_wal;
	/**
	 * Used if there was a WAL I/O error and we need to
	 * keep adding all incoming requests to the rollback
	 * queue, until the tx thread has recovered.
	 */
	struct cmsg in_rollback;
	/**
	 * WAL watchers, i.e. threads that should be alerted
	 * whenever there are new records appended to the journal.
	 * Used for replication relays.
	 */
	struct rlist watchers;
	/**
	 * Wal memory buffer routines.
	 * Writer stores rows into two memory buffers swapping by buffer
	 * threshold. Memory buffers are splitted into chunks with the
	 * same server id issued by chunk threshold. In order to search
	 * position in wal memory all chunks indexed by wal_mem_index array.
	 * Each wal_mem_index contains corresponding replica_id and vclock
	 * just before first row in the chunk as well as chunk buffer number,
	 * position and size of the chunk in memory. When buffer should be
	 * swapped then all buffers chunks discarded and wal_mem_discard_count_count
	 * increases in order to adjust relays position.
	 */
	/** Rows buffers. */
	struct ibuf wal_mem[2];
	/** Index buffer. */
	struct ibuf wal_mem_index;
	/** Count of discarded mem chunks. */
	uint64_t wal_mem_discard_count;
	/** Condition to signal if there are new rows. */
	struct fiber_cond memory_cond;
};

struct wal_msg {
	struct cmsg base;
	/** Approximate size of this request when encoded. */
	size_t approx_len;
	/** Input queue, on output contains all committed requests. */
	struct stailq commit;
	/**
	 * In case of rollback, contains the requests which must
	 * be rolled back.
	 */
	struct stailq rollback;
	/** vclock after the batch processed. */
	struct vclock vclock;
};

/**
 * Wal memory chunk index.
 */
struct wal_mem_index {
	/** replica id. */
	uint32_t replica_id;
	/** vclock just before first row in the chunk. */
	struct vclock vclock;
	/** Buffer number. */
	uint8_t buf_no;
	/** Chunk starting offset. */
	uint64_t pos;
	/** Chunk size. */
	uint64_t size;
};

/**
 * Wal memory position checkpoint.
 */
struct wal_mem_checkpoint {
	/** Chunks count. */
	uint32_t count;
	/** Chunk size. */
	uint32_t size;
};

/** Current relaying position. */
struct wal_relay_mem_pos {
	uint64_t wal_mem_discard_count;
	uint32_t chunk_index;
	uint32_t offset;
};

/**
 * Wal relay structure.
 */
struct wal_relay {
	struct rlist item;
	/** Writer. */
	struct wal_writer *writer;
	/** Sent vclock. */
	struct vclock vclock;
	/** Vclock when subscribed. */
	struct vclock vclock_at_subscribe;
	/** Socket to send data. */
	int fd;
	/** Peer replica. */
	struct replica *replica;
	/** Cord to recover and relay data from files. */
	struct cord cord;
	/** A diagnostic area. */
	struct diag diag;
	/** Current position. */
	struct wal_relay_mem_pos pos;
	/** Relay writer fiber. */
	struct fiber *writer_fiber;
	/** Relay reader fiber. */
	struct fiber *reader_fiber;
};

/**
 * Vinyl metadata log writer.
 */
struct vy_log_writer {
	/** The metadata log file. */
	struct xlog xlog;
};

static struct vy_log_writer vy_log_writer;
static struct wal_thread wal_thread;
static struct wal_writer wal_writer_singleton;

enum wal_mode
wal_mode()
{
	return wal_writer_singleton.wal_mode;
}

static void
wal_write_to_disk(struct cmsg *msg);

static void
tx_schedule_commit(struct cmsg *msg);

static struct cmsg_hop wal_request_route[] = {
	{wal_write_to_disk, &wal_thread.tx_prio_pipe},
	{tx_schedule_commit, NULL},
};

static void
wal_msg_create(struct wal_msg *batch)
{
	cmsg_init(&batch->base, wal_request_route);
	batch->approx_len = 0;
	stailq_create(&batch->commit);
	stailq_create(&batch->rollback);
	vclock_create(&batch->vclock);
}

static struct wal_msg *
wal_msg(struct cmsg *msg)
{
	return msg->route == wal_request_route ? (struct wal_msg *) msg : NULL;
}

/** Write a request to a log in a single transaction. */
static ssize_t
xlog_write_entry(struct xlog *l, struct journal_entry *entry)
{
	/*
	 * Iterate over request rows (tx statements)
	 */
	xlog_tx_begin(l);
	struct xrow_header **row = entry->rows;
	for (; row < entry->rows + entry->n_rows; row++) {
		struct errinj *inj = errinj(ERRINJ_WAL_BREAK_LSN, ERRINJ_INT);
		if (inj != NULL && inj->iparam == (*row)->lsn) {
			(*row)->lsn = inj->iparam - 1;
			say_warn("injected broken lsn: %lld",
				 (long long) (*row)->lsn);
		}
		if (xlog_write_row(l, *row) < 0) {
			/*
			 * Rollback all un-written rows
			 */
			xlog_tx_rollback(l);
			return -1;
		}
	}
	return xlog_tx_commit(l);
}

/**
 * Invoke fibers waiting for their journal_entry's to be
 * completed. The fibers are invoked in strict fifo order:
 * this ensures that, in case of rollback, requests are
 * rolled back in strict reverse order, producing
 * a consistent database state.
 */
static void
tx_schedule_queue(struct stailq *queue)
{
	/*
	 * fiber_wakeup() is faster than fiber_call() when there
	 * are many ready fibers.
	 */
	struct journal_entry *req;
	stailq_foreach_entry(req, queue, fifo)
		fiber_wakeup(req->fiber);
}

/**
 * Complete execution of a batch of WAL write requests:
 * schedule all committed requests, and, should there
 * be any requests to be rolled back, append them to
 * the rollback queue.
 */
static void
tx_schedule_commit(struct cmsg *msg)
{
	struct wal_writer *writer = &wal_writer_singleton;
	struct wal_msg *batch = (struct wal_msg *) msg;
	/*
	 * Move the rollback list to the writer first, since
	 * wal_msg memory disappears after the first
	 * iteration of tx_schedule_queue loop.
	 */
	vclock_copy(&replicaset.vclock, &batch->vclock);
	if (! stailq_empty(&batch->rollback)) {
		/* Closes the input valve. */
		stailq_concat(&writer->rollback, &batch->rollback);
	}
	tx_schedule_queue(&batch->commit);
}

static void
tx_schedule_rollback(struct cmsg *msg)
{
	(void) msg;
	struct wal_writer *writer = &wal_writer_singleton;
	/*
	 * Perform a cascading abort of all transactions which
	 * depend on the transaction which failed to get written
	 * to the write ahead log. Abort transactions
	 * in reverse order, performing a playback of the
	 * in-memory database state.
	 */
	stailq_reverse(&writer->rollback);
	/* Must not yield. */
	tx_schedule_queue(&writer->rollback);
	stailq_create(&writer->rollback);
}


/**
 * This message is sent from WAL to TX when the WAL thread hits
 * ENOSPC and has to delete some backup WAL files to continue.
 * The TX thread uses this message to shoot off WAL consumers
 * that needed deleted WAL files.
 */
struct tx_notify_gc_msg {
	struct cmsg base;
	/** VClock of the oldest WAL row preserved by WAL. */
	struct vclock vclock;
};

static void
tx_notify_gc(struct cmsg *msg)
{
	struct wal_writer *writer = &wal_writer_singleton;
	struct vclock *vclock = &((struct tx_notify_gc_msg *)msg)->vclock;
	writer->on_garbage_collection(vclock);
	free(msg);
}

static void
tx_notify_checkpoint(struct cmsg *msg)
{
	struct wal_writer *writer = &wal_writer_singleton;
	writer->on_checkpoint_threshold();
	free(msg);
}

/**
 * A message to initialize a writer in a wal thread.
 */
struct wal_writer_create_msg {
	struct cbus_call_msg base;
	struct wal_writer *writer;
};

/**
 * Writer initialization to do in a wal thread.
 */
static int
wal_writer_create_wal(struct cbus_call_msg *base)
{
	struct wal_writer_create_msg *msg =
		container_of(base, struct wal_writer_create_msg, base);
	struct wal_writer *writer= msg->writer;
	ibuf_create(&writer->wal_mem[0], &cord()->slabc, 65536);
	ibuf_create(&writer->wal_mem[1], &cord()->slabc, 65536);
	ibuf_create(&writer->wal_mem_index, &cord()->slabc, 8192);
	struct wal_mem_index *index;
	index = ibuf_alloc(&writer->wal_mem_index, sizeof(struct wal_mem_index));
	if (index == NULL) {
		/* Could not initialize wal writer. */
		panic("Could not create wal");
		unreachable();
	}
	writer->wal_mem_discard_count = 0;
	vclock_copy(&index->vclock, &writer->vclock);
	index->pos = 0;
	index->size = 0;
	index->buf_no = 0;
	fiber_cond_create(&writer->memory_cond);

	return 0;
}

/**
 * Initialize WAL writer context. Even though it's a singleton,
 * encapsulate the details just in case we may use
 * more writers in the future.
 */
static void
wal_writer_create(struct wal_writer *writer, enum wal_mode wal_mode,
		  const char *wal_dirname, int64_t wal_max_rows,
		  int64_t wal_max_size, const struct tt_uuid *instance_uuid,
		  const struct vclock *vclock,
		  const struct vclock *checkpoint_vclock,
		  wal_on_garbage_collection_f on_garbage_collection,
		  wal_on_checkpoint_threshold_f on_checkpoint_threshold)
{
	writer->wal_mode = wal_mode;
	writer->wal_max_rows = wal_max_rows;
	writer->wal_max_size = wal_max_size;
	journal_create(&writer->base, wal_mode == WAL_NONE ?
		       wal_write_in_wal_mode_none : wal_write, NULL);

	xdir_create(&writer->wal_dir, wal_dirname, XLOG, instance_uuid);
	xlog_clear(&writer->current_wal);
	if (wal_mode == WAL_FSYNC)
		writer->wal_dir.open_wflags |= O_SYNC;

	stailq_create(&writer->rollback);
	cmsg_init(&writer->in_rollback, NULL);

	writer->checkpoint_wal_size = 0;
	writer->checkpoint_threshold = INT64_MAX;
	writer->checkpoint_triggered = false;

	vclock_copy(&writer->vclock, vclock);
	vclock_copy(&writer->checkpoint_vclock, checkpoint_vclock);
	rlist_create(&writer->watchers);

	writer->on_garbage_collection = on_garbage_collection;
	writer->on_checkpoint_threshold = on_checkpoint_threshold;
	struct wal_writer_create_msg msg;
	msg.writer = writer;
	cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe,
			   &msg.base, wal_writer_create_wal, NULL,
			   TIMEOUT_INFINITY);
}

/** Destroy a WAL writer structure. */
static void
wal_writer_destroy(struct wal_writer *writer)
{
	xdir_destroy(&writer->wal_dir);
}

/** WAL thread routine. */
static int
wal_thread_f(va_list ap);

/** Start WAL thread and setup pipes to and from TX. */
void
wal_thread_start()
{
	if (cord_costart(&wal_thread.cord, "wal", wal_thread_f, NULL) != 0)
		panic("failed to start WAL thread");

	/* Create a pipe to WAL thread. */
	cpipe_create(&wal_thread.wal_pipe, "wal");
	cpipe_set_max_input(&wal_thread.wal_pipe, IOV_MAX);
}

static int
wal_open_f(struct cbus_call_msg *msg)
{
	(void)msg;
	struct wal_writer *writer = &wal_writer_singleton;
	const char *path = xdir_format_filename(&writer->wal_dir,
				vclock_sum(&writer->vclock), NONE);
	assert(!xlog_is_open(&writer->current_wal));
	return xlog_open(&writer->current_wal, path);
}

/**
 * Try to open the current WAL file for appending if it exists.
 */
static int
wal_open(struct wal_writer *writer)
{
	const char *path = xdir_format_filename(&writer->wal_dir,
				vclock_sum(&writer->vclock), NONE);
	if (access(path, F_OK) != 0) {
		if (errno == ENOENT) {
			/* No WAL, nothing to do. */
			return 0;
		}
		diag_set(SystemError, "failed to access %s", path);
		return -1;
	}

	/*
	 * The WAL file exists, try to open it.
	 *
	 * Note, an xlog object cannot be opened and used in
	 * different threads (because it uses slab arena), so
	 * we have to call xlog_open() on behalf of the WAL
	 * thread.
	 */
	struct cbus_call_msg msg;
	if (cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe, &msg,
		      wal_open_f, NULL, TIMEOUT_INFINITY) == 0) {
		/*
		 * Success: we can now append to
		 * the existing WAL file.
		 */
		return 0;
	}
	struct error *e = diag_last_error(diag_get());
	if (!type_assignable(&type_XlogError, e->type)) {
		/*
		 * Out of memory or system error.
		 * Nothing we can do.
		 */
		return -1;
	}
	diag_log();

	/*
	 * Looks like the WAL file is corrupted.
	 * Rename it so that we can proceed.
	 */
	say_warn("renaming corrupted %s", path);
	char new_path[PATH_MAX];
	snprintf(new_path, sizeof(new_path), "%s.corrupted", path);
	if (rename(path, new_path) != 0) {
		diag_set(SystemError, "failed to rename %s", path);
		return -1;
	}
	return 0;
}

/**
 * Initialize WAL writer.
 *
 * @pre   The instance has completed recovery from a snapshot
 *        and/or existing WALs. All WALs opened in read-only
 *        mode are closed. WAL thread has been started.
 */
int
wal_init(enum wal_mode wal_mode, const char *wal_dirname, int64_t wal_max_rows,
	 int64_t wal_max_size, const struct tt_uuid *instance_uuid,
	 const struct vclock *vclock, const struct vclock *checkpoint_vclock,
	 wal_on_garbage_collection_f on_garbage_collection,
	 wal_on_checkpoint_threshold_f on_checkpoint_threshold)
{
	assert(wal_max_rows > 1);

	struct wal_writer *writer = &wal_writer_singleton;
	wal_writer_create(writer, wal_mode, wal_dirname, wal_max_rows,
			  wal_max_size, instance_uuid, vclock,
			  checkpoint_vclock, on_garbage_collection,
			  on_checkpoint_threshold);

	/*
	 * Scan the WAL directory to build an index of all
	 * existing WAL files. Required for garbage collection,
	 * see wal_collect_garbage().
	 */
	if (xdir_scan(&writer->wal_dir))
		return -1;

	if (wal_open(writer) != 0)
		return -1;

	journal_set(&writer->base);
	return 0;
}

/**
 * Stop WAL thread, wait until it exits, and destroy WAL writer
 * if it was initialized. Called on shutdown.
 */
void
wal_thread_stop()
{
	cbus_stop_loop(&wal_thread.wal_pipe);

	if (cord_join(&wal_thread.cord)) {
		/* We can't recover from this in any reasonable way. */
		panic_syserror("WAL writer: thread join failed");
	}

	if (journal_is_initialized(&wal_writer_singleton.base))
		wal_writer_destroy(&wal_writer_singleton);
}

void
wal_sync(void)
{
	struct wal_writer *writer = &wal_writer_singleton;
	if (writer->wal_mode == WAL_NONE)
		return;
	cbus_flush(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe, NULL);
}

static int
wal_begin_checkpoint_f(struct cbus_call_msg *data)
{
	struct wal_checkpoint *msg = (struct wal_checkpoint *) data;
	struct wal_writer *writer = &wal_writer_singleton;
	if (writer->in_rollback.route != NULL) {
		/*
		 * We're rolling back a failed write and so
		 * can't make a checkpoint - see the comment
		 * in wal_begin_checkpoint() for the explanation.
		 */
		diag_set(ClientError, ER_CHECKPOINT_ROLLBACK);
		return -1;
	}
	/*
	 * Avoid closing the current WAL if it has no rows (empty).
	 */
	if (xlog_is_open(&writer->current_wal) &&
	    vclock_sum(&writer->current_wal.meta.vclock) !=
	    vclock_sum(&writer->vclock)) {

		xlog_close(&writer->current_wal, false);
		/*
		 * The next WAL will be created on the first write.
		 */
	}
	vclock_copy(&msg->vclock, &writer->vclock);
	msg->wal_size = writer->checkpoint_wal_size;
	return 0;
}

int
wal_begin_checkpoint(struct wal_checkpoint *checkpoint)
{
	struct wal_writer *writer = &wal_writer_singleton;
	if (writer->wal_mode == WAL_NONE) {
		vclock_copy(&checkpoint->vclock, &writer->vclock);
		checkpoint->wal_size = 0;
		return 0;
	}
	if (!stailq_empty(&writer->rollback)) {
		/*
		 * If cascading rollback is in progress, in-memory
		 * indexes can contain changes scheduled for rollback.
		 * If we made a checkpoint, we could write them to
		 * the snapshot. So we abort checkpointing in this
		 * case.
		 */
		diag_set(ClientError, ER_CHECKPOINT_ROLLBACK);
		return -1;
	}
	bool cancellable = fiber_set_cancellable(false);
	int rc = cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe,
			   &checkpoint->base, wal_begin_checkpoint_f, NULL,
			   TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
	if (rc != 0)
		return -1;
	return 0;
}

static int
wal_commit_checkpoint_f(struct cbus_call_msg *data)
{
	struct wal_checkpoint *msg = (struct wal_checkpoint *) data;
	struct wal_writer *writer = &wal_writer_singleton;
	/*
	 * Now, once checkpoint has been created, we can update
	 * the WAL's version of the last checkpoint vclock and
	 * reset the size of WAL files written since the last
	 * checkpoint. Note, since new WAL records may have been
	 * written while the checkpoint was created, we subtract
	 * the value of checkpoint_wal_size observed at the time
	 * when checkpointing started from the current value
	 * rather than just setting it to 0.
	 */
	vclock_copy(&writer->checkpoint_vclock, &msg->vclock);
	assert(writer->checkpoint_wal_size >= msg->wal_size);
	writer->checkpoint_wal_size -= msg->wal_size;
	writer->checkpoint_triggered = false;
	return 0;
}

void
wal_commit_checkpoint(struct wal_checkpoint *checkpoint)
{
	struct wal_writer *writer = &wal_writer_singleton;
	if (writer->wal_mode == WAL_NONE) {
		vclock_copy(&writer->checkpoint_vclock, &checkpoint->vclock);
		return;
	}
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe,
		  &checkpoint->base, wal_commit_checkpoint_f, NULL,
		  TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
}

struct wal_set_checkpoint_threshold_msg {
	struct cbus_call_msg base;
	int64_t checkpoint_threshold;
};

static int
wal_set_checkpoint_threshold_f(struct cbus_call_msg *data)
{
	struct wal_writer *writer = &wal_writer_singleton;
	struct wal_set_checkpoint_threshold_msg *msg;
	msg = (struct wal_set_checkpoint_threshold_msg *)data;
	writer->checkpoint_threshold = msg->checkpoint_threshold;
	return 0;
}

void
wal_set_checkpoint_threshold(int64_t threshold)
{
	struct wal_writer *writer = &wal_writer_singleton;
	if (writer->wal_mode == WAL_NONE)
		return;
	struct wal_set_checkpoint_threshold_msg msg;
	msg.checkpoint_threshold = threshold;
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe,
		  &msg.base, wal_set_checkpoint_threshold_f, NULL,
		  TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
}

struct wal_gc_msg
{
	struct cbus_call_msg base;
	const struct vclock *vclock;
};

static int
wal_collect_garbage_f(struct cbus_call_msg *data)
{
	struct wal_writer *writer = &wal_writer_singleton;
	const struct vclock *vclock = ((struct wal_gc_msg *)data)->vclock;

	if (!xlog_is_open(&writer->current_wal) &&
	    vclock_sum(vclock) >= vclock_sum(&writer->vclock)) {
		/*
		 * The last available WAL file has been sealed and
		 * all registered consumers have done reading it.
		 * We can delete it now.
		 */
	} else {
		/*
		 * Find the most recent WAL file that contains rows
		 * required by registered consumers and delete all
		 * older WAL files.
		 */
		vclock = vclockset_psearch(&writer->wal_dir.index, vclock);
	}
	if (vclock != NULL)
		xdir_collect_garbage(&writer->wal_dir, vclock_sum(vclock), 0);

	return 0;
}

void
wal_collect_garbage(const struct vclock *vclock)
{
	struct wal_writer *writer = &wal_writer_singleton;
	if (writer->wal_mode == WAL_NONE)
		return;
	struct wal_gc_msg msg;
	msg.vclock = vclock;
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe, &msg.base,
		  wal_collect_garbage_f, NULL, TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
}

static void
wal_notify_watchers(struct wal_writer *writer, unsigned events);

/**
 * If there is no current WAL, try to open it, and close the
 * previous WAL. We close the previous WAL only after opening
 * a new one to smoothly move local hot standby and replication
 * over to the next WAL.
 * In case of error, we try to close any open WALs.
 *
 * @post r->current_wal is in a good shape for writes or is NULL.
 * @return 0 in case of success, -1 on error.
 */
static int
wal_opt_rotate(struct wal_writer *writer)
{
	ERROR_INJECT_RETURN(ERRINJ_WAL_ROTATE);

	/*
	 * Close the file *before* we create the new WAL, to
	 * make sure local hot standby/replication can see
	 * EOF in the old WAL before switching to the new
	 * one.
	 */
	if (xlog_is_open(&writer->current_wal) &&
	    (writer->current_wal.rows >= writer->wal_max_rows ||
	     writer->current_wal.offset >= writer->wal_max_size)) {
		/*
		 * We can not handle xlog_close()
		 * failure in any reasonable way.
		 * A warning is written to the error log.
		 */
		xlog_close(&writer->current_wal, false);
	}

	if (xlog_is_open(&writer->current_wal))
		return 0;

	if (xdir_create_xlog(&writer->wal_dir, &writer->current_wal,
			     &writer->vclock) != 0) {
		diag_log();
		return -1;
	}
	/*
	 * Keep track of the new WAL vclock. Required for garbage
	 * collection, see wal_collect_garbage().
	 */
	xdir_add_vclock(&writer->wal_dir, &writer->vclock);

	wal_notify_watchers(writer, WAL_EVENT_ROTATE);
	return 0;
}

/**
 * Make sure there's enough disk space to append @len bytes
 * of data to the current WAL.
 *
 * If fallocate() fails with ENOSPC, delete old WAL files
 * that are not needed for recovery and retry.
 */
static int
wal_fallocate(struct wal_writer *writer, size_t len)
{
	bool warn_no_space = true, notify_gc = false;
	struct xlog *l = &writer->current_wal;
	struct errinj *errinj = errinj(ERRINJ_WAL_FALLOCATE, ERRINJ_INT);
	int rc = 0;

	/*
	 * Max LSN that can be collected in case of ENOSPC -
	 * we must not delete WALs necessary for recovery.
	 */
	int64_t gc_lsn = vclock_sum(&writer->checkpoint_vclock);

	/*
	 * The actual write size can be greater than the sum size
	 * of encoded rows (compression, fixheaders). Double the
	 * given length to get a rough upper bound estimate.
	 */
	len *= 2;

retry:
	if (errinj == NULL || errinj->iparam == 0) {
		if (l->allocated >= len)
			goto out;
		if (xlog_fallocate(l, MAX(len, WAL_FALLOCATE_LEN)) == 0)
			goto out;
	} else {
		errinj->iparam--;
		diag_set(ClientError, ER_INJECTION, "xlog fallocate");
		errno = ENOSPC;
	}
	if (errno != ENOSPC)
		goto error;
	if (!xdir_has_garbage(&writer->wal_dir, gc_lsn))
		goto error;

	if (warn_no_space) {
		say_crit("ran out of disk space, try to delete old WAL files");
		warn_no_space = false;
	}

	/* Keep the original error. */
	struct diag diag;
	diag_create(&diag);
	diag_move(diag_get(), &diag);
	if (xdir_collect_garbage(&writer->wal_dir, gc_lsn,
				 XDIR_GC_REMOVE_ONE) != 0) {
		diag_move(&diag, diag_get());
		goto error;
	}
	diag_destroy(&diag);

	notify_gc = true;
	goto retry;
error:
	diag_log();
	rc = -1;
out:
	/*
	 * Notify the TX thread if the WAL thread had to delete
	 * some WAL files to proceed so that TX can shoot off WAL
	 * consumers that still need those files.
	 *
	 * We allocate the message with malloc() and we ignore
	 * allocation failures, because this is a pretty rare
	 * event and a failure to send this message isn't really
	 * critical.
	 */
	if (notify_gc) {
		static struct cmsg_hop route[] = {
			{ tx_notify_gc, NULL },
		};
		struct tx_notify_gc_msg *msg = malloc(sizeof(*msg));
		if (msg != NULL) {
			if (xdir_first_vclock(&writer->wal_dir,
					      &msg->vclock) < 0)
				vclock_copy(&msg->vclock, &writer->vclock);
			cmsg_init(&msg->base, route);
			cpipe_push(&wal_thread.tx_prio_pipe, &msg->base);
		} else
			say_warn("failed to allocate gc notification message");
	}
	return rc;
}

static void
wal_writer_clear_bus(struct cmsg *msg)
{
	(void) msg;
}

static void
wal_writer_end_rollback(struct cmsg *msg)
{
	(void) msg;
	struct wal_writer *writer = &wal_writer_singleton;
	cmsg_init(&writer->in_rollback, NULL);
}

static void
wal_writer_begin_rollback(struct wal_writer *writer)
{
	static struct cmsg_hop rollback_route[4] = {
		/*
		 * Step 1: clear the bus, so that it contains
		 * no WAL write requests. This is achieved as a
		 * side effect of an empty message travelling
		 * through both bus pipes, while writer input
		 * valve is closed by non-empty writer->rollback
		 * list.
		 */
		{ wal_writer_clear_bus, &wal_thread.wal_pipe },
		{ wal_writer_clear_bus, &wal_thread.tx_prio_pipe },
		/*
		 * Step 2: writer->rollback queue contains all
		 * messages which need to be rolled back,
		 * perform the rollback.
		 */
		{ tx_schedule_rollback, &wal_thread.wal_pipe },
		/*
		 * Step 3: re-open the WAL for writing.
		 */
		{ wal_writer_end_rollback, NULL }
	};

	/*
	 * Make sure the WAL writer rolls back
	 * all input until rollback mode is off.
	 */
	cmsg_init(&writer->in_rollback, rollback_route);
	cpipe_push(&wal_thread.tx_prio_pipe, &writer->in_rollback);
}

static void
wal_assign_lsn(struct vclock *vclock, struct xrow_header **begin,
	       struct xrow_header **end)
{
	struct xrow_header **row = begin;
	/** Assign LSN to all local rows. */
	for ( ; row < end; row++) {
		if ((*row)->replica_id == 0) {
			(*row)->lsn = vclock_inc(vclock, instance_id);
			(*row)->replica_id = instance_id;
		} else {
			vclock_follow_xrow(vclock, *row);
		}
	}
	if ((*begin)->replica_id != instance_id) {
		/*
		 * Move all local changes to the end of rows array to form
		 * a fake local transaction (like an autonomous transaction)
		 * in order to be able to replicate local changes back.
		 */
		struct xrow_header **row = end - 1;
		while (row >= begin) {
			if (row[0]->replica_id != instance_id) {
				--row;
				continue;
			}
			/* Local row, move it back. */
			struct xrow_header **local_row = row;
			while (local_row < end - 1 &&
			       local_row[1]->replica_id != instance_id) {
				struct xrow_header *tmp = local_row[0];
				local_row[0] = local_row[1];
				local_row[1] = tmp;
			}
			--row;
		}
		while (begin < end && begin[0]->replica_id != instance_id)
			++begin;
	}
	/* Setup txn_id and tnx_replica_id for locally generated rows. */
	row = begin;
	while (row < end) {
		row[0]->txn_id = begin[0]->lsn;
		row[0]->txn_replica_id = instance_id;
		row[0]->txn_last = row == end - 1 ? 1 : 0;
		++row;
	}
}

static inline struct wal_mem_index *
wal_mem_index_first(struct wal_writer *writer)
{
	return (struct wal_mem_index *)writer->wal_mem_index.rpos;
}

static inline struct wal_mem_index *
wal_mem_index_last(struct wal_writer *writer)
{
	return (struct wal_mem_index *)writer->wal_mem_index.wpos - 1;
}

static inline struct wal_mem_index *
wal_mem_index_new(struct wal_writer *writer, int buf_no, struct vclock *vclock)
{
	struct wal_mem_index *last = wal_mem_index_last(writer);
	if (last->size == 0)
		return last;
	last = (struct wal_mem_index *)ibuf_alloc(&writer->wal_mem_index,
						  sizeof(struct wal_mem_index));
	if (last == NULL) {
		diag_set(OutOfMemory, sizeof(struct wal_mem_index),
			 "region", "struct wal_mem_index");
		return NULL;
	}
	last->buf_no = buf_no;
	last->size = 0;
	last->pos = ibuf_used(writer->wal_mem + buf_no);
	vclock_copy(&last->vclock, vclock);
	return last;
}

/** Save current memory position. */
static inline void
wal_mem_get_checkpoint(struct wal_writer *writer,
		       struct wal_mem_checkpoint *mem_checkpoint)
{
	mem_checkpoint->count = wal_mem_index_last(writer) -
			      wal_mem_index_first(writer);
	mem_checkpoint->size = wal_mem_index_last(writer)->size;
}

/** Restore memory position. */
static inline void
wal_mem_set_checkpoint(struct wal_writer *writer,
		       struct wal_mem_checkpoint *mem_checkpoint)
{
	struct wal_mem_index *index = wal_mem_index_first(writer) +
				      mem_checkpoint->count;
	assert(index->buf_no == wal_mem_index_last(writer)->buf_no);
	index->size = mem_checkpoint->size;
	/* Truncate buffers. */
	writer->wal_mem_index.wpos = (char *)(index + 1);
	struct ibuf *buf = writer->wal_mem + index->buf_no;
	buf->wpos = buf->buf + index->pos + index->size;
}

/**
 * Prepare wal memory to accept new rows and rotate buffer if it needs.
 */
static int
wal_mem_prepare(struct wal_writer *writer)
{
	struct wal_mem_index *last = wal_mem_index_last(writer);
	uint8_t buf_no = last->buf_no;
	if (ibuf_used(&writer->wal_mem[buf_no]) > WAL_MEMORY_THRESHOLD) {
		/* We are going to rotate buffers. */
		struct wal_mem_index *first = wal_mem_index_first(writer);
		while (first->buf_no == 1 - buf_no)
			++first;
		/* Discard all indexes on buffer to clear. */
		writer->wal_mem_discard_count += first - wal_mem_index_first(writer);
		writer->wal_mem_index.rpos = (char *)first;

		buf_no = 1 - buf_no;
		ibuf_reset(&writer->wal_mem[buf_no]);

		last = wal_mem_index_new(writer, buf_no, &writer->vclock);
		if (last == NULL)
			return -1;
	}
	return 0;
}

/** Get a chunk to store rows data. */
static struct wal_mem_index *
wal_get_chunk(struct wal_writer *writer, uint32_t replica_id,
	      struct vclock *vclock)
{
	struct wal_mem_index *last = wal_mem_index_last(writer);
	int buf_no = last->buf_no;

	if (last->size == 0)
		last->replica_id = replica_id;

	if (last->size > WAL_MEM_CHUNK_THRESHOLD ||
	    last->replica_id != replica_id) {
		/* Open new chunk. */
		last = wal_mem_index_new(writer, buf_no, vclock);
		if (last == NULL)
			return NULL;
		last->replica_id = replica_id;
	}
	return last;
}

/**
 * Encode an entry into a wal memory buffer.
 */
static ssize_t
wal_encode_entry(struct wal_writer *writer, struct journal_entry *entry,
		 struct vclock *vclock)
{
	double tm = ev_now(loop());
	struct xrow_header nop_row;
	nop_row.type = IPROTO_NOP;
	nop_row.group_id = GROUP_DEFAULT;
	nop_row.bodycnt = 0;
	nop_row.tm = tm;

	struct wal_mem_index *last = wal_mem_index_last(writer);
	int buf_no = last->buf_no;
	last = NULL;
	struct ibuf *mem_buf = writer->wal_mem + buf_no;

	/* vclock to track encoding row. */
	struct vclock chunk_vclock;
	vclock_copy(&chunk_vclock, vclock);

	wal_assign_lsn(vclock, entry->rows, entry->rows + entry->n_rows);
	entry->res = vclock_sum(vclock);

	struct xrow_header **row = entry->rows;
	while (row < entry->rows + entry->n_rows) {
		(*row)->tm = tm;
		struct iovec iov[XROW_IOVMAX];
		if ((*row)->group_id == GROUP_LOCAL) {
			nop_row.replica_id = (*row)->replica_id;
			nop_row.lsn = (*row)->lsn;
			nop_row.txn_id = (*row)->txn_id;
			nop_row.txn_replica_id = (*row)->txn_replica_id;
		}
		struct xrow_header *send_row = (*row)->group_id != GROUP_LOCAL ?
					        *row : &nop_row;
		if (last == NULL) {
			/* Do not allow split transactions into chunks. */
			last = wal_get_chunk(writer, send_row->replica_id,
					     &chunk_vclock);
		}
		if (last == NULL)
			goto error;
		vclock_follow_xrow(&chunk_vclock, send_row);

		int iovcnt = xrow_to_iovec(send_row, iov);
		if (iovcnt < 0)
			goto error;
		uint64_t xrow_size = 0;
		for (int i = 0; i < iovcnt; ++i)
			xrow_size += iov[i].iov_len;
		if (ibuf_reserve(mem_buf, xrow_size) == NULL) {
			diag_set(OutOfMemory, xrow_size,
				 "region", "entry memory");
			goto error;
		}

		for (int i = 0; i < iovcnt; ++i) {
			memcpy(ibuf_alloc(mem_buf, iov[i].iov_len),
			       iov[i].iov_base, iov[i].iov_len);
			last->size += iov[i].iov_len;
		}
		if (row[0]->txn_last == 1)
			last = NULL;
		++row;
	}
	return 0;

error:
	return -1;
}

static void
wal_write_to_disk(struct cmsg *msg)
{
	struct wal_writer *writer = &wal_writer_singleton;
	struct wal_msg *wal_msg = (struct wal_msg *) msg;
	struct error *error;

	/* Local vclock copy. */
	struct vclock vclock;
	vclock_create(&vclock);
	vclock_copy(&vclock, &writer->vclock);

	struct errinj *inj = errinj(ERRINJ_WAL_DELAY, ERRINJ_BOOL);
	while (inj != NULL && inj->bparam)
		usleep(10);

	if (writer->in_rollback.route != NULL) {
		/* We're rolling back a failed write. */
		stailq_concat(&wal_msg->rollback, &wal_msg->commit);
		return;
	}

	/* Xlog is only rotated between queue processing  */
	if (wal_opt_rotate(writer) != 0) {
		stailq_concat(&wal_msg->rollback, &wal_msg->commit);
		return wal_writer_begin_rollback(writer);
	}

	/* Ensure there's enough disk space before writing anything. */
	if (wal_fallocate(writer, wal_msg->approx_len) != 0) {
		stailq_concat(&wal_msg->rollback, &wal_msg->commit);
		return wal_writer_begin_rollback(writer);
	}

	/*
	 * This code tries to write queued requests (=transactions) using as
	 * few I/O syscalls and memory copies as possible. For this reason
	 * writev(2) and `struct iovec[]` are used (see `struct fio_batch`).
	 *
	 * For each request (=transaction) each request row (=statement) is
	 * added to iov `batch`. A row can contain up to XLOG_IOVMAX iovecs.
	 * A request can have an **unlimited** number of rows. Since OS has
	 * a hard coded limit up to `sysconf(_SC_IOV_MAX)` iovecs (usually
	 * 1024), a huge transaction may not fit into a single batch.
	 * Therefore, it is not possible to "atomically" write an entire
	 * transaction using a single writev(2) call.
	 *
	 * Request boundaries and batch boundaries are not connected at all
	 * in this code. Batches flushed to disk as soon as they are full.
	 * In order to guarantee that a transaction is either fully written
	 * to file or isn't written at all, ftruncate(2) is used to shrink
	 * the file to the last fully written request. The absolute position
	 * of request in xlog file is stored inside `struct journal_entry`.
	 */

	struct xlog *l = &writer->current_wal;

	/*
	 * Iterate over requests (transactions)
	 */
	int rc;
	struct journal_entry *entry;
	struct stailq_entry *last_committed = NULL;
	if (wal_mem_prepare(writer) != 0)
		goto done;
	struct wal_mem_checkpoint mem_checkpoint;
	wal_mem_get_checkpoint(writer, &mem_checkpoint);
	stailq_foreach_entry(entry, &wal_msg->commit, fifo) {
		rc = wal_encode_entry(writer, entry, &vclock);
		if (rc != 0)
			goto done;
		rc = xlog_write_entry(l, entry);
		if (rc < 0) {
			wal_mem_set_checkpoint(writer, &mem_checkpoint);
			goto done;
		}
		if (rc > 0) {
			writer->checkpoint_wal_size += rc;
			last_committed = &entry->fifo;
			vclock_copy(&writer->vclock, &vclock);
			wal_mem_get_checkpoint(writer, &mem_checkpoint);
		}
		/* rc == 0: the write is buffered in xlog_tx */
	}
	rc = xlog_flush(l);
	if (rc < 0) {
		wal_mem_set_checkpoint(writer, &mem_checkpoint);
		goto done;
	}

	writer->checkpoint_wal_size += rc;
	last_committed = stailq_last(&wal_msg->commit);
	vclock_copy(&writer->vclock, &vclock);

	/*
	 * Notify TX if the checkpoint threshold has been exceeded.
	 * Use malloc() for allocating the notification message and
	 * don't panic on error, because if we fail to send the
	 * message now, we will retry next time we process a request.
	 */
	if (!writer->checkpoint_triggered &&
	    writer->checkpoint_wal_size > writer->checkpoint_threshold) {
		static struct cmsg_hop route[] = {
			{ tx_notify_checkpoint, NULL },
		};
		struct cmsg *msg = malloc(sizeof(*msg));
		if (msg != NULL) {
			cmsg_init(msg, route);
			cpipe_push(&wal_thread.tx_prio_pipe, msg);
			writer->checkpoint_triggered = true;
		} else {
			say_warn("failed to allocate checkpoint "
				 "notification message");
		}
	}

done:
	error = diag_last_error(diag_get());
	if (error) {
		/* Until we can pass the error to tx, log it and clear. */
		error_log(error);
		diag_clear(diag_get());
	}
	/* Set resulting vclock. */
	vclock_copy(&wal_msg->vclock, &writer->vclock);
	/*
	 * We need to start rollback from the first request
	 * following the last committed request. If
	 * last_commit_req is NULL, it means we have committed
	 * nothing, and need to start rollback from the first
	 * request. Otherwise we rollback from the first request.
	 */
	struct stailq rollback;
	stailq_cut_tail(&wal_msg->commit, last_committed, &rollback);

	if (!stailq_empty(&rollback)) {
		/* Update status of the successfully committed requests. */
		stailq_foreach_entry(entry, &rollback, fifo)
			entry->res = -1;
		/* Rollback unprocessed requests */
		stailq_concat(&wal_msg->rollback, &rollback);
		wal_writer_begin_rollback(writer);
	}
	fiber_gc();
	fiber_cond_broadcast(&writer->memory_cond);
	wal_notify_watchers(writer, WAL_EVENT_WRITE);
}

/** WAL thread main loop.  */
static int
wal_thread_f(va_list ap)
{
	(void) ap;
	struct wal_writer *writer = &wal_writer_singleton;
	rlist_create(&wal_thread.relay);

	/** Initialize eio in this thread */
	coio_enable();

	struct cbus_endpoint endpoint;
	cbus_endpoint_create(&endpoint, "wal", fiber_schedule_cb, fiber());
	/*
	 * Create a pipe to TX thread. Use a high priority
	 * endpoint, to ensure that WAL messages are delivered
	 * even when tx fiber pool is used up by net messages.
	 */
	cpipe_create(&wal_thread.tx_prio_pipe, "tx_prio");

	cbus_loop(&endpoint);

	struct wal_relay *msg;
	rlist_foreach_entry(msg, &wal_thread.relay, item) {
		if (msg->cord.id != 0) {
			if (tt_pthread_cancel(msg->cord.id) == ESRCH)
				continue;
			tt_pthread_join(msg->cord.id, NULL);
		}
	}

	/*
	 * Create a new empty WAL on shutdown so that we don't
	 * have to rescan the last WAL to find the instance vclock.
	 * Don't create a WAL if the last one is empty.
	 */
	if (writer->wal_mode != WAL_NONE &&
	    (!xlog_is_open(&writer->current_wal) ||
	     vclock_compare(&writer->vclock,
			    &writer->current_wal.meta.vclock) > 0)) {
		struct xlog l;
		if (xdir_create_xlog(&writer->wal_dir, &l,
				     &writer->vclock) == 0)
			xlog_close(&l, false);
		else
			diag_log();
	}

	if (xlog_is_open(&writer->current_wal))
		xlog_close(&writer->current_wal, false);

	if (xlog_is_open(&vy_log_writer.xlog))
		xlog_close(&vy_log_writer.xlog, false);

	cpipe_destroy(&wal_thread.tx_prio_pipe);
	return 0;
}

/**
 * WAL writer main entry point: queue a single request
 * to be written to disk and wait until this task is completed.
 */
int64_t
wal_write(struct journal *journal, struct journal_entry *entry)
{
	struct wal_writer *writer = (struct wal_writer *) journal;

	ERROR_INJECT_RETURN(ERRINJ_WAL_IO);

	if (! stailq_empty(&writer->rollback)) {
		/*
		 * The writer rollback queue is not empty,
		 * roll back this transaction immediately.
		 * This is to ensure we do not accidentally
		 * commit a transaction which has seen changes
		 * that will be rolled back.
		 */
		say_error("Aborting transaction %llu during "
			  "cascading rollback",
			  vclock_sum(&writer->vclock));
		return -1;
	}

	struct wal_msg *batch;
	if (!stailq_empty(&wal_thread.wal_pipe.input) &&
	    (batch = wal_msg(stailq_first_entry(&wal_thread.wal_pipe.input,
						struct cmsg, fifo)))) {

		stailq_add_tail_entry(&batch->commit, entry, fifo);
	} else {
		batch = (struct wal_msg *)
			region_alloc(&fiber()->gc, sizeof(struct wal_msg));
		if (batch == NULL) {
			diag_set(OutOfMemory, sizeof(struct wal_msg),
				 "region", "struct wal_msg");
			return -1;
		}
		wal_msg_create(batch);
		/*
		 * Sic: first add a request, then push the batch,
		 * since cpipe_push() may pass the batch to WAL
		 * thread right away.
		 */
		stailq_add_tail_entry(&batch->commit, entry, fifo);
		cpipe_push(&wal_thread.wal_pipe, &batch->base);
	}
	batch->approx_len += entry->approx_len;
	wal_thread.wal_pipe.n_input += entry->n_rows * XROW_IOVMAX;
	cpipe_flush_input(&wal_thread.wal_pipe);
	/**
	 * It's not safe to spuriously wakeup this fiber
	 * since in that case it will ignore a possible
	 * error from WAL writer and not roll back the
	 * transaction.
	 */
	bool cancellable = fiber_set_cancellable(false);
	fiber_yield(); /* Request was inserted. */
	fiber_set_cancellable(cancellable);
	return entry->res;
}

int64_t
wal_write_in_wal_mode_none(struct journal *journal,
			   struct journal_entry *entry)
{
	struct wal_writer *writer = (struct wal_writer *) journal;
	wal_assign_lsn(&writer->vclock, entry->rows, entry->rows + entry->n_rows);
	vclock_copy(&replicaset.vclock, &writer->vclock);
	return vclock_sum(&writer->vclock);
}

void
wal_init_vy_log()
{
	xlog_clear(&vy_log_writer.xlog);
}

struct wal_write_vy_log_msg
{
	struct cbus_call_msg base;
	struct journal_entry *entry;
};

static int
wal_write_vy_log_f(struct cbus_call_msg *msg)
{
	struct journal_entry *entry =
		((struct wal_write_vy_log_msg *)msg)->entry;

	if (! xlog_is_open(&vy_log_writer.xlog)) {
		if (vy_log_open(&vy_log_writer.xlog) < 0)
			return -1;
	}

	if (xlog_write_entry(&vy_log_writer.xlog, entry) < 0)
		return -1;

	if (xlog_flush(&vy_log_writer.xlog) < 0)
		return -1;

	return 0;
}

int
wal_write_vy_log(struct journal_entry *entry)
{
	struct wal_write_vy_log_msg msg;
	msg.entry= entry;
	bool cancellable = fiber_set_cancellable(false);
	int rc = cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe,
			   &msg.base, wal_write_vy_log_f, NULL,
			   TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
	return rc;
}

static int
wal_rotate_vy_log_f(struct cbus_call_msg *msg)
{
	(void) msg;
	if (xlog_is_open(&vy_log_writer.xlog))
		xlog_close(&vy_log_writer.xlog, false);
	return 0;
}

void
wal_rotate_vy_log()
{
	struct cbus_call_msg msg;
	bool cancellable = fiber_set_cancellable(false);
	cbus_call(&wal_thread.wal_pipe, &wal_thread.tx_prio_pipe, &msg,
		  wal_rotate_vy_log_f, NULL, TIMEOUT_INFINITY);
	fiber_set_cancellable(cancellable);
}

static void
wal_watcher_notify(struct wal_watcher *watcher, unsigned events)
{
	assert(!rlist_empty(&watcher->next));

	struct wal_watcher_msg *msg = &watcher->msg;
	if (msg->cmsg.route != NULL) {
		/*
		 * If the notification message is still en route,
		 * mark the watcher to resend it as soon as it
		 * returns to WAL so as not to lose any events.
		 */
		watcher->pending_events |= events;
		return;
	}

	msg->events = events;
	cmsg_init(&msg->cmsg, watcher->route);
	cpipe_push(&watcher->watcher_pipe, &msg->cmsg);
}

static void
wal_watcher_notify_perform(struct cmsg *cmsg)
{
	struct wal_watcher_msg *msg = (struct wal_watcher_msg *) cmsg;
	struct wal_watcher *watcher = msg->watcher;
	unsigned events = msg->events;

	watcher->cb(watcher, events);
}

static void
wal_watcher_notify_complete(struct cmsg *cmsg)
{
	struct wal_watcher_msg *msg = (struct wal_watcher_msg *) cmsg;
	struct wal_watcher *watcher = msg->watcher;

	cmsg->route = NULL;

	if (rlist_empty(&watcher->next)) {
		/* The watcher is about to be destroyed. */
		return;
	}

	if (watcher->pending_events != 0) {
		/*
		 * Resend the message if we got notified while
		 * it was en route, see wal_watcher_notify().
		 */
		wal_watcher_notify(watcher, watcher->pending_events);
		watcher->pending_events = 0;
	}
}

static void
wal_watcher_attach(void *arg)
{
	struct wal_watcher *watcher = (struct wal_watcher *) arg;
	struct wal_writer *writer = &wal_writer_singleton;

	assert(rlist_empty(&watcher->next));
	rlist_add_tail_entry(&writer->watchers, watcher, next);

	/*
	 * Notify the watcher right after registering it
	 * so that it can process existing WALs.
	 */
	wal_watcher_notify(watcher, WAL_EVENT_ROTATE);
}

static void
wal_watcher_detach(void *arg)
{
	struct wal_watcher *watcher = (struct wal_watcher *) arg;

	assert(!rlist_empty(&watcher->next));
	rlist_del_entry(watcher, next);
}

void
wal_set_watcher(struct wal_watcher *watcher, const char *name,
		void (*watcher_cb)(struct wal_watcher *, unsigned events),
		void (*process_cb)(struct cbus_endpoint *))
{
	assert(journal_is_initialized(&wal_writer_singleton.base));

	rlist_create(&watcher->next);
	watcher->cb = watcher_cb;
	watcher->msg.watcher = watcher;
	watcher->msg.events = 0;
	watcher->msg.cmsg.route = NULL;
	watcher->pending_events = 0;

	assert(lengthof(watcher->route) == 2);
	watcher->route[0] = (struct cmsg_hop)
		{ wal_watcher_notify_perform, &watcher->wal_pipe };
	watcher->route[1] = (struct cmsg_hop)
		{ wal_watcher_notify_complete, NULL };
	cbus_pair("wal", name, &watcher->wal_pipe, &watcher->watcher_pipe,
		  wal_watcher_attach, watcher, process_cb);
}

void
wal_clear_watcher(struct wal_watcher *watcher,
		  void (*process_cb)(struct cbus_endpoint *))
{
	assert(journal_is_initialized(&wal_writer_singleton.base));

	cbus_unpair(&watcher->wal_pipe, &watcher->watcher_pipe,
		    wal_watcher_detach, watcher, process_cb);
}

static void
wal_notify_watchers(struct wal_writer *writer, unsigned events)
{
	struct wal_watcher *watcher;
	rlist_foreach_entry(watcher, &writer->watchers, next)
		wal_watcher_notify(watcher, events);
}

/**
 * Send data to peer.
 * Assume that socket already in non blocking mode.
 *
 * Return:
 * 0 chunk sent without yield
 * 1 chunk sent with yield
 * -1 an error occurred
 */
static int
wal_relay_send(void *data, size_t to_write, int fd)
{
	{
		struct errinj *inj = errinj(ERRINJ_RELAY_SEND_DELAY, ERRINJ_BOOL);
		if (inj != NULL && inj->bparam) {
			void *errinj_data =  region_alloc(&fiber()->gc, to_write);
			if (errinj_data == NULL) {
				diag_set(OutOfMemory, to_write, "region", "relay pending data");
				return -1;
			}
			memcpy(errinj_data, data, to_write);
			data = errinj_data;
			while (inj != NULL && inj->bparam)
				fiber_sleep(0.01);
		}
	}

	int rc = 0;
	ssize_t written = write(fd, data, to_write);
	if (written < 0 && ! (errno == EAGAIN || errno == EWOULDBLOCK))
		goto io_error;
	if (written == (ssize_t)to_write)
		goto done;

	/* Preserve data to send. */
	if (written < 0)
		written = 0;
	to_write -= written;
	void *pending_data = region_alloc(&fiber()->gc, to_write);
	if (pending_data == NULL) {
		diag_set(OutOfMemory, to_write, "region", "relay pending data");
		return -1;
	}
	data += written;
	memcpy(pending_data, data, to_write);
	while (to_write > 0) {
		if (coio_wait(fd, COIO_WRITE, TIMEOUT_INFINITY) < 0)
			goto error;
		written = write(fd, pending_data, to_write);
		if (written < 0 && ! (errno == EAGAIN || errno == EWOULDBLOCK))
			goto io_error;
		if (written < 0)
			written = 0;
		pending_data += written;
		to_write -= written;
	}
	rc = 1;

done:
	fiber_gc();
	{
		struct errinj *inj = errinj(ERRINJ_RELAY_SEND_DELAY, ERRINJ_BOOL);
		inj = errinj(ERRINJ_RELAY_TIMEOUT, ERRINJ_DOUBLE);
		if (inj != NULL && inj->dparam > 0)
			fiber_sleep(inj->dparam);
	}
	return rc;

io_error:
	diag_set(SocketError, sio_socketname(fd), "write");
error:
	fiber_gc();
	return -1;
}

/** Send heartbeat to the peer. */
static int
wal_relay_heartbeat(int fd)
{
	struct xrow_header row;
	xrow_encode_timestamp(&row, instance_id, ev_now(loop()));
	struct iovec iov[XROW_IOVMAX];
	int iovcnt = xrow_to_iovec(&row, iov);
	if (iovcnt < 0)
		return -1;
	for (int i = 0; i < iovcnt; ++i)
		if (wal_relay_send(iov[i].iov_base, iov[i].iov_len, fd) < 0)
			return -1;
	return 0;
}

/**
 * Send memory chunk to peer.
 * Assume that socket already in non blocking mode.
 *
 * Return:
 * 0 chunk sent without yield
 * 1 chunk sent with yield
 * -1 an error occurred
 */
static int
wal_relay_send_chunk(struct wal_relay *wal_relay)
{
	struct wal_writer *writer = wal_relay->writer;
	struct wal_relay_mem_pos *pos = &wal_relay->pos;

	/* Adjust position in case of rotation. */
	assert(pos->chunk_index >= writer->wal_mem_discard_count -
				 pos->wal_mem_discard_count);
	pos->chunk_index -= writer->wal_mem_discard_count -
			    pos->wal_mem_discard_count;
	pos->wal_mem_discard_count = writer->wal_mem_discard_count;

	struct wal_mem_index *first = wal_mem_index_first(writer);
	struct wal_mem_index *last = wal_mem_index_last(writer);
	struct wal_mem_index *chunk = first + pos->chunk_index;

	if (chunk < last && chunk->size == pos->offset) {
		/* Current chunk is done, use the next one. */
		++pos->chunk_index;
		pos->offset = 0;
		++chunk;
	}
	assert(first <= chunk && last >= chunk);
	assert(chunk->size >= pos->offset);
	ssize_t to_write = chunk->size - pos->offset;
	if (to_write == 0)
		return 0;
	/* Preserve the clock after the transfer because it might change. */
	struct vclock last_vclock;
	vclock_copy(&last_vclock,
		    chunk == last ? &writer->vclock : &(chunk + 1)->vclock);
	struct ibuf *buf = writer->wal_mem + chunk->buf_no;
	void *data = buf->rpos + chunk->pos + pos->offset;
	if ((wal_relay->replica->id != chunk->replica_id ||
	     vclock_get(&chunk->vclock, chunk->replica_id) <
	     vclock_get(&wal_relay->vclock_at_subscribe, chunk->replica_id)) &&
	    wal_relay_send(data, to_write, wal_relay->fd) < 0)
		return -1;
	vclock_copy(&wal_relay->vclock, &last_vclock);
	pos->offset += to_write;
	return 0;
}

/** Test if vclock position in a writer memory. */
static inline bool
wal_relay_in_mem(struct wal_writer *writer, struct vclock *vclock)
{
	struct errinj *inj = errinj(ERRINJ_WAL_RELAY_DISABLE_MEM,
				    ERRINJ_BOOL);
	if (inj != NULL && inj->bparam) {
		return false;
	}
	struct wal_mem_index *first = wal_mem_index_first(writer);
	int cmp = vclock_compare(&first->vclock, vclock);
	return cmp == -1 || cmp == 0;
}

/** Setup relay position. */
static void
wal_relay_setup_chunk(struct wal_relay *wal_relay)
{
	struct wal_writer *writer = wal_relay->writer;
	struct vclock *vclock = &wal_relay->vclock;
	struct wal_mem_index *first = wal_mem_index_first(writer);
	struct wal_mem_index *last = wal_mem_index_last(writer);
	struct wal_mem_index *mid = NULL;
	while (last - first > 1) {
		mid = first + (last - first + 1) / 2;
		int cmp = vclock_compare(vclock, &mid->vclock);
		if (cmp == 0) {
			first = last = mid;
			break;
		}
		if (cmp == 1)
			first = mid;
		else
			last = mid;
	}
	if (last != first) {
		int cmp = vclock_compare(vclock, &last->vclock);
		if (cmp == 0 || cmp == 1)
			++first;
	}
	wal_relay->pos.chunk_index = first - wal_mem_index_first(writer);
	wal_relay->pos.wal_mem_discard_count = writer->wal_mem_discard_count;
	wal_relay->pos.offset = 0;
}

/**
 * Recover wal memory from current position until the end.
 */
static int
wal_relay_recover_mem(struct wal_relay *wal_relay)
{
	struct wal_writer *writer = wal_relay->writer;
	struct vclock *vclock = &wal_relay->vclock;
	if (!wal_relay_in_mem(writer, vclock))
		return 0;
	wal_relay_setup_chunk(wal_relay);

	do {
		int rc = wal_relay_send_chunk(wal_relay);
		if (rc < 0)
			return -1;
		if (vclock_compare(&writer->vclock, vclock) == 1) {
			/* There are more data, send the next chunk. */
			continue;
		}
		double timeout = replication_timeout;
		struct errinj *inj = errinj(ERRINJ_RELAY_REPORT_INTERVAL,
					    ERRINJ_DOUBLE);
		if (inj != NULL && inj->dparam != 0)
			timeout = inj->dparam;
		/* Wait for new rows or heartbeat timeout. */
		while (fiber_cond_wait_timeout(&writer->memory_cond,
					       timeout) == -1 &&
		       !fiber_is_cancelled(fiber())) {
			if (wal_relay_heartbeat(wal_relay->fd) < 0)
				return -1;
			if (inj != NULL && inj->dparam != 0)
				timeout = inj->dparam;
			continue;
		}
	} while (!fiber_is_cancelled(fiber()) &&
		 wal_relay_in_mem(writer, vclock));

	return fiber_is_cancelled(fiber()) ? -1: 0;
}

/**
 * Cord function to recover xlog files.
 */
static int
wal_relay_recover_file_f(va_list ap)
{
	struct wal_relay *wal_relay = va_arg(ap, struct wal_relay *);
	struct wal_writer *writer = wal_relay->writer;
	struct replica *replica = wal_relay->replica;
	struct vclock *vclock = &wal_relay->vclock;

	struct recovery *recovery;
	recovery = recovery_new(writer->wal_dir.dirname, false, vclock);
	if (recovery == NULL)
		return -1;

	int res = relay_recover_wals(replica, recovery);
	vclock_copy(vclock, &recovery->vclock);

	recovery_delete(recovery);
	return res;
}

/**
 * Create cord to recover and relay xlog files.
 */
static int
wal_relay_recover_file(struct wal_relay *msg)
{
	cord_costart(&msg->cord, "recovery wal files",
		     wal_relay_recover_file_f, msg);
	int res =  cord_cojoin(&msg->cord);
	msg->cord.id = 0;
	return res;
}

/**
 * Message to inform relay about peer vclock changes.
 */
struct wal_relay_status_msg {
	struct cbus_call_msg base;
	/** Replica. */
	struct replica *replica;
	/** Known replica vclock. */
	struct vclock vclock;
};

static int
tx_wal_relay_status(struct cbus_call_msg *base)
{
	struct wal_relay_status_msg *msg =
		container_of(base, struct wal_relay_status_msg, base);
	relay_status_update(msg->replica, &msg->vclock);
	return 0;
}

/**
 * Peer vclock reader fiber function.
 */
static int
wal_relay_reader_f(va_list ap)
{
	struct wal_relay *wal_relay = va_arg(ap, struct wal_relay *);
	struct replica *replica = wal_relay->replica;

	struct ibuf ibuf;
	ibuf_create(&ibuf, &cord()->slabc, 1024);
	while (!fiber_is_cancelled()) {
		struct xrow_header xrow;
		if (coio_read_xrow_timeout(wal_relay->fd, &ibuf, &xrow,
					   replication_disconnect_timeout()) != 0)
			break;
		struct wal_relay_status_msg msg;
		/* vclock is followed while decoding, zeroing it. */
		vclock_create(&msg.vclock);
		xrow_decode_vclock(&xrow, &msg.vclock);
		msg.replica = replica;
		cbus_call(&wal_thread.tx_prio_pipe, &wal_thread.wal_pipe,
			  &msg.base, tx_wal_relay_status, NULL,
			  TIMEOUT_INFINITY);
	}
	if (diag_is_empty(&wal_relay->diag))
		diag_move(&fiber()->diag, &wal_relay->diag);
	fiber_cancel(wal_relay->writer_fiber);
	ibuf_destroy(&ibuf);
	return 0;

}

/**
 * Message to control wal relay.
 */
struct wal_relay_start_msg {
	struct cmsg base;
	/** Stop condition. */
	struct fiber_cond stop_cond;
	/** Done status to protect against spurious wakeup. */
	bool done;
	/** Replica. */
	struct replica *replica;
	/** Replica known vclock. */
	struct vclock *vclock;
	/** Replica socket. */
	int fd;
	/** Diagnostic area. */
	struct diag diag;
};

/**
 * Handler to inform when wal relay is exited.
 */
static void
wal_relay_done(struct cmsg *base)
{
	struct wal_relay_start_msg *msg;
	msg = container_of(base, struct wal_relay_start_msg, base);
	msg->done = true;
	fiber_cond_signal(&msg->stop_cond);
}

/**
 * Helper to send wal relay done message.
 */
static void
wal_relay_done_send(struct wal_relay_start_msg *msg)
{
	static struct cmsg_hop done_route[] = {
		{wal_relay_done, NULL}
	};
	/*
	 * Because of complicated cbus routing and fiber_start behavior
	 * message could be still in cbus processing, so let cbus finish
	 * with it.
	 */
	fiber_reschedule();
	cmsg_init(&msg->base, done_route);
	diag_move(&fiber()->diag, &msg->diag);
	cpipe_push(&wal_thread.tx_prio_pipe, &msg->base);
}

/**
 * Wal relay writer fiber.
 */
static int
wal_relay_f(va_list ap)
{
	struct wal_writer *writer = &wal_writer_singleton;

	struct wal_relay_start_msg *msg = va_arg(ap, struct wal_relay_start_msg *);
	struct wal_relay wal_relay;
	memset(&wal_relay, 0, sizeof(wal_relay));
	rlist_add(&wal_thread.relay, &wal_relay.item);
	wal_relay.writer = writer;
	wal_relay.replica = msg->replica;
	wal_relay.fd = msg->fd;
	vclock_copy(&wal_relay.vclock, msg->vclock);
	vclock_copy(&wal_relay.vclock_at_subscribe, &writer->vclock);
	diag_create(&wal_relay.diag);
	wal_relay.writer_fiber = fiber();
	wal_relay.reader_fiber = fiber_new("relay_status", wal_relay_reader_f);
	if (wal_relay.reader_fiber == NULL)
		goto done;
	fiber_set_joinable(wal_relay.reader_fiber, true);
	fiber_start(wal_relay.reader_fiber, &wal_relay);

	/* Open a new chunk to separate logs before subscribe. */
	if (wal_mem_index_new(writer, wal_mem_index_last(writer)->buf_no,
			      &writer->vclock) == NULL)
		goto done;

	if (wal_relay_heartbeat(msg->fd) < 0)
		goto done;

	while (!fiber_is_cancelled(fiber())) {
		if (wal_relay_recover_mem(&wal_relay) < 0)
			break;
		if (wal_relay_recover_file(&wal_relay) < 0)
			break;
	}

done:
	if (diag_is_empty(&fiber()->diag))
		diag_move(&wal_relay.diag, &fiber()->diag);
	fiber_cancel(wal_relay.reader_fiber);
	fiber_join(wal_relay.reader_fiber);

	rlist_del(&wal_relay.item);
	vclock_copy(msg->vclock, &wal_relay.vclock);

	wal_relay_done_send(msg);
	return 0;
}

/**
 * Start a wal relay in a wal thread.
 */
static void
wal_relay_start(struct cmsg *base)
{
	struct wal_relay_start_msg *msg;
	msg = container_of(base, struct wal_relay_start_msg, base);

	struct fiber *writer_fiber = fiber_new("wal_relay", wal_relay_f);
	if (writer_fiber == NULL)
		return wal_relay_done_send(msg);
	fiber_start(writer_fiber, msg);
}

/**
 * Start a wal relay.
 */
int
wal_relay(struct replica *replica, struct vclock *vclock, int fd)
{
	/*
	 * Send wal relay start to wal thread and then wait for a
	 * finish condition.
	 * We are not able to do that job in synchronous manner with
	 * cbus_call and fiber join because wal thread has no fiber pool
	 * and then cbus handler not allowed to yield.
	 */
	struct wal_relay_start_msg msg;
	memset(&msg, 0, sizeof(msg));
	msg.vclock = vclock;
	msg.fd = fd;
	msg.replica = replica;
	msg.done = false;
	fiber_cond_create(&msg.stop_cond);
	diag_create(&msg.diag);
	static struct cmsg_hop start_route[] = {
		{wal_relay_start, NULL}};
	cmsg_init(&msg.base, start_route);
	cpipe_push(&wal_thread.wal_pipe, &msg.base);
	while (!msg.done)
		fiber_cond_wait(&msg.stop_cond);
	if (!diag_is_empty(&msg.diag))
		diag_move(&msg.diag, &fiber()->diag);
	return diag_is_empty(&fiber()->diag) ? 0: -1;
}

/**
 * After fork, the WAL writer thread disappears.
 * Make sure that atexit() handlers in the child do
 * not try to stop a non-existent thread or write
 * a second EOF marker to an open file.
 */
void
wal_atfork()
{
	if (xlog_is_open(&wal_writer_singleton.current_wal))
		xlog_atfork(&wal_writer_singleton.current_wal);
	if (xlog_is_open(&vy_log_writer.xlog))
		xlog_atfork(&vy_log_writer.xlog);
}
