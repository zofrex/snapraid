/*
 * Copyright (C) 2016 Andrea Mazzoleni
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "portable.h"

#include "io.h"

void io_init(struct snapraid_io* io, struct snapraid_state* state, unsigned buffer_max,
	void (*data_reader)(struct snapraid_worker*, struct snapraid_task*),
	struct snapraid_handle* handle_map, unsigned handle_max,
	void (*parity_reader)(struct snapraid_worker*, struct snapraid_task*),
	struct snapraid_parity_handle* parity_handle_map, unsigned parity_handle_max)
{
	unsigned i;

	pthread_mutex_init(&io->mutex, 0);
	pthread_cond_init(&io->read_done, 0);
	pthread_cond_init(&io->read_sched, 0);

	io->state = state;

	io->buffer_max = buffer_max;
	for (i = 0; i < IO_MAX; ++i) {
		io->buffer_map[i] = malloc_nofail_vector_align(handle_max, buffer_max, state->block_size, &io->buffer_alloc_map[i]);
		if (!state->opt.skip_self)
			mtest_vector(io->buffer_max, state->block_size, io->buffer_map[i]);
	}

	io->reader_max = handle_max + parity_handle_max;
	io->reader_map = malloc_nofail(sizeof(struct snapraid_worker) * io->reader_max);
	io->reader_list = malloc_nofail(io->reader_max + 1);

	io->data_base = 0;
	io->data_count = handle_max;
	io->parity_base = handle_max;
	io->parity_count = parity_handle_max;

	for (i = 0; i < io->reader_max; ++i) {
		struct snapraid_worker* worker = &io->reader_map[i];

		worker->io = io;

		if (i < handle_max) {
			/* it's a data read */
			worker->handle = &handle_map[i];
			worker->parity_handle = 0;
			worker->func = data_reader;

			/* data read is put in lower buffer index */
			worker->buffer_skew = 0;
		} else {
			/* it's a parity read */
			worker->handle = 0;
			worker->parity_handle = &parity_handle_map[i - handle_max];
			worker->func = parity_reader;

			/* parity read is put after data and computed parity */
			worker->buffer_skew = parity_handle_max;
		}
	}
}

void io_done(struct snapraid_io* io)
{
	unsigned i;

	for (i = 0; i < IO_MAX; ++i) {
		free(io->buffer_map[i]);
		free(io->buffer_alloc_map[i]);
	}

	free(io->reader_map);
	free(io->reader_list);

	pthread_mutex_destroy(&io->mutex);
	pthread_cond_destroy(&io->read_done);
	pthread_cond_destroy(&io->read_sched);
}

/**
 * Get the next block position to process.
 */
static block_off_t io_position_next(struct snapraid_io* io)
{
	block_off_t blockcur;

	/* get the next position */
	while (io->block_next < io->block_max && !io->block_is_enabled(io->block_arg, io->block_next))
		++io->block_next;

	blockcur = io->block_next;

	/* next block for the next call */
	++io->block_next;

	return blockcur;
}

/**
 * Setup the next pending task for all readers.
 */
static void io_reader_sched(struct snapraid_io* io, int index, block_off_t blockcur)
{
	unsigned i;

	for (i = 0; i < io->reader_max; ++i) {
		struct snapraid_worker* worker = &io->reader_map[i];
		struct snapraid_task* task = &worker->task_map[index];

		/* setup the new pending task */
		if (blockcur < io->block_max)
			task->state = TASK_STATE_READY;
		else
			task->state = TASK_STATE_EMPTY;

		task->path[0] = 0;
		if (worker->handle)
			task->disk = worker->handle->disk;
		else
			task->disk = 0;
		task->buffer = io->buffer_map[index][worker->buffer_skew + i];
		task->position = blockcur;
		task->block = 0;
		task->file = 0;
		task->file_pos = 0;
		task->read_size = 0;
		task->is_timestamp_different = 0;
	}
}

/**
 * Get the next task to work on for a reader.
 *
 * This is the synchronization point for workers with the io.
 */
static struct snapraid_task* io_reader_step(struct snapraid_worker* worker)
{
	struct snapraid_io* io = worker->io;
	struct snapraid_task* task = 0;

	/* the synchronization is protected by the io mutex */
	pthread_mutex_lock(&io->mutex);

	while (1) {
		unsigned index;

		/* check if the worker has to exit */
		/* even if there is work to do */
		if (io->done)
			break;

		/* get the next pending task */
		index = (worker->index + 1) % IO_MAX;

		/* if the queue of pending tasks is not empty */
		if (index != io->reader_index) {
			/* get the new working task */
			worker->index = index;
			task = &worker->task_map[worker->index];

			/* notify the io that a new read is complete */
			pthread_cond_signal(&io->read_done);

			/* return the new task */
			break;
		}

		/* otherwise wait for a read_sched event */
		pthread_cond_wait(&io->read_sched, &io->mutex);
	}

	pthread_mutex_unlock(&io->mutex);

	return task;
}

/**
 * Get the next block position to operate on.
 *
 * This is the synchronization point for workers with the io.
 */
block_off_t io_read_next(struct snapraid_io* io, void*** buffer)
{
	block_off_t blockcur_schedule;
	block_off_t blockcur_caller;
	unsigned i;

	/* get the next parity position to process */
	blockcur_schedule = io_position_next(io);

	/* ensure that all data/parity was read */
	assert(io->reader_list[0] == io->reader_max);

	/* setup the list of workers to process */
	for (i = 0; i <= io->reader_max; ++i)
		io->reader_list[i] = i;

	/* the synchronization is protected by the io mutex */
	pthread_mutex_lock(&io->mutex);

	/* schedule the next read */
	io_reader_sched(io, io->reader_index, blockcur_schedule);

	/* get the next task to return to the caller */
	io->reader_index = (io->reader_index + 1) % IO_MAX;

	/* get the position to operate at high level from one task */
	blockcur_caller = io->reader_map[0].task_map[io->reader_index].position;

	/* set the buffer to use */
	*buffer = io->buffer_map[io->reader_index];

	/* signal all the workers that there is a new pending task */
	pthread_cond_broadcast(&io->read_sched);

	pthread_mutex_unlock(&io->mutex);

	return blockcur_caller;
}

static struct snapraid_task* io_task_read(struct snapraid_io* io, unsigned base, unsigned count, unsigned* pos)
{
	/* the synchronization is protected by the io mutex */
	pthread_mutex_lock(&io->mutex);

	while (1) {
		unsigned char* let;
		unsigned index;

		/* get the index the caller is using */
		/* we must ensure that this index has not a read in progress */
		/* to avoid a concurrent access */
		index = io->reader_index;

		/* search for a worker that has already finished */
		let = &io->reader_list[0];
		while (1) {
			unsigned i = *let;

			/* if we are at the end */
			if (i == io->reader_max)
				break;

			/* if it's in range */
			if (base <= i && i < base + count) {
				struct snapraid_worker* worker;

				worker = &io->reader_map[i];

				/* if the worker has finished this index */
				if (index != worker->index) {
					struct snapraid_task* task;

					task = &worker->task_map[io->reader_index];

					pthread_mutex_unlock(&io->mutex);

					/* mark the worker as processed */
					/* setting the previous one to point at the next one */
					*let = io->reader_list[i + 1];

					/* return the position */
					*pos = i - base;

					return task;
				}
			}

			/* next position to check */
			let = &io->reader_list[i + 1];
		}

		/* if not worker is ready, wait for an event */
		pthread_cond_wait(&io->read_done, &io->mutex);
	}
}

struct snapraid_task* io_data_read(struct snapraid_io* io, unsigned* pos)
{
	return io_task_read(io, io->data_base, io->data_count, pos);
}

struct snapraid_task* io_parity_read(struct snapraid_io* io, unsigned* pos)
{
	return io_task_read(io, io->parity_base, io->parity_count, pos);
}

static void io_reader_worker(struct snapraid_worker* worker, struct snapraid_task* task)
{
	/* if we reached the end */
	if (task->position >= worker->io->block_max) {
		/* complete a dummy task */
		task->state = TASK_STATE_EMPTY;
	} else {
		worker->func(worker, task);
	}
}

static void* io_reader_thread(void* arg)
{
	struct snapraid_worker* worker = arg;

	/* force completion of the first task */
	io_reader_worker(worker, &worker->task_map[0]);

	while (1) {
		struct snapraid_task* task;

		/* get the new task */
		task = io_reader_step(worker);

		/* if no task, it means to exit */
		if (!task)
			break;

		/* nothing more to do */
		if (task->state == TASK_STATE_EMPTY)
			continue;

		assert(task->state == TASK_STATE_READY);

		/* work on the assigned task */
		io_reader_worker(worker, task);
	}

	return 0;
}

void io_start(struct snapraid_io* io,
	block_off_t blockstart, block_off_t blockmax,
	int (*block_is_enabled)(void* arg,block_off_t), void* blockarg)
{
	unsigned i;

	io->block_start = blockstart;
	io->block_max = blockmax;
	io->block_is_enabled = block_is_enabled;
	io->block_arg = blockarg;
	io->block_next = blockstart;

	io->done = 0;
	io->reader_index = IO_MAX - 1;

	/* setup the initial read pending tasks, except the latest one, */
	/* the latest will be initialized at the fist io_read_next() call */
	for (i = 0; i < IO_MAX - 1; ++i) {
		block_off_t blockcur = io_position_next(io);

		io_reader_sched(io, i, blockcur);
	}

	/* setup the lists of workers to process */
	io->reader_list[0] = io->reader_max;

	/* start the reader threads */
	for (i = 0; i < io->reader_max; ++i) {
		struct snapraid_worker* worker = &io->reader_map[i];

		worker->index = 0;

		if (pthread_create(&worker->thread, 0, io_reader_thread, worker) != 0) {
			/* LCOV_EXCL_START */
			log_fatal("Failed to create reader thread.\n");
			exit(EXIT_FAILURE);
			/* LCOV_EXCL_STOP */
		}
	}
}

void io_stop(struct snapraid_io* io)
{
	unsigned i;

	pthread_mutex_lock(&io->mutex);

	/* mark that we are stopping */
	io->done = 1;

	/* signal all the threads to recognize the new state */
	pthread_cond_broadcast(&io->read_sched);

	pthread_mutex_unlock(&io->mutex);

	/* wait for all readers to terminate */
	for (i = 0; i < io->reader_max; ++i) {
		struct snapraid_worker* worker = &io->reader_map[i];
		void* retval;

		/* wait for thread termination */
		if (pthread_join(worker->thread, &retval) != 0) {
			/* LCOV_EXCL_START */
			log_fatal("Failed to join reader thread.\n");
			exit(EXIT_FAILURE);
			/* LCOV_EXCL_STOP */
		}
	}
}
