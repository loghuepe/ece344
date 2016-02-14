/*
 * Synchronization primitives.
 * See synch.h for specifications of the functions.
 */

#include <types.h>
#include <lib.h>
#include <synch.h>
#include <thread.h>
#include <curthread.h>
#include <machine/spl.h>

/* Forward declaration for a field in lock structure */
extern struct thread *curthread;

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *namearg, int initial_count)
{
	struct semaphore *sem;

	assert(initial_count >= 0);

	sem = kmalloc(sizeof(struct semaphore));
	if (sem == NULL) {
		return NULL;
	}

	sem->name = kstrdup(namearg);
	if (sem->name == NULL) {
		kfree(sem);
		return NULL;
	}

	sem->count = initial_count;
	return sem;
}

void
sem_destroy(struct semaphore *sem)
{
	int spl;
	assert(sem != NULL);

	spl = splhigh();
	assert(thread_hassleepers(sem)==0);
	splx(spl);

	/*
	 * Note: while someone could theoretically start sleeping on
	 * the semaphore after the above test but before we free it,
	 * if they're going to do that, they can just as easily wait
	 * a bit and start sleeping on the semaphore after it's been
	 * freed. Consequently, there's not a whole lot of point in 
	 * including the kfrees in the splhigh block, so we don't.
	 */

	kfree(sem->name);
	kfree(sem);
}

void 
P(struct semaphore *sem)
{
	int spl;
	assert(sem != NULL);

	/*
	 * May not block in an interrupt handler.
	 *
	 * For robustness, always check, even if we can actually
	 * complete the P without blocking.
	 */
	assert(in_interrupt==0);

	spl = splhigh();
	while (sem->count==0) {
		thread_sleep(sem);
	}
	assert(sem->count>0);
	sem->count--;
	splx(spl);
}

void
V(struct semaphore *sem)
{
	int spl;
	assert(sem != NULL);
	spl = splhigh();
	sem->count++;
	assert(sem->count>0);
	thread_wakeup(sem);
	splx(spl);
}

////////////////////////////////////////////////////////////
//
// Lock.

struct lock *
lock_create(const char *name)
{
	struct lock *lock;

	lock = kmalloc(sizeof(struct lock));
	if (lock == NULL) {
		return NULL;
	}

	lock->name = kstrdup(name);
	if (lock->name == NULL) {
		kfree(lock);
		return NULL;
	}
	
	// add stuff here as needed
	lock-> holder = NULL;
	return lock;
}

void
lock_destroy(struct lock *lock)
{
	assert(lock != NULL);

	// add stuff here as needed
	
	kfree(lock->name);
	kfree(lock);
}


void lock_acquire (struct lock *lock) {
	// disable interrupts
	int original_interrupt_level = splhigh();
	// spin until unlocked
	while (lock->held != 0) {
		// enable all interrupts
		spl0();
		// then disable
		splhigh();
	}
	// this thread gets the lock
	lock-> held = 1;	
	lock-> holder = curthread;
	// restore original interrupt
	splx(original_interrupt_level);
	return;
}

void
lock_release(struct lock *lock)
{
	int spl = splhigh();
	lock-> held = 0;
	lock-> holder = NULL;
	splx(spl);
}


int
lock_do_i_hold(struct lock *lock)
{
	return (lock->holder == curthread);
}

////////////////////////////////////////////////////////////
//
// CV


struct cv *
cv_create(const char *name)
{
	struct cv *cv;

	cv = kmalloc(sizeof(struct cv));
	if (cv == NULL) {
		return NULL;
	}

	cv->name = kstrdup(name);
	if (cv->name==NULL) {
		kfree(cv);
		return NULL;
	}
	
	// add stuff here as needed
	cv->wait_queue = array_create();
	if (cv->wait_queue == NULL) {
		kfree(cv);
		return NULL;
	}
	return cv;
}

void
cv_destroy(struct cv *cv)
{
	int spl = splhigh();
	assert(cv != NULL);

	// add stuff here as needed
	array_destroy(cv->wait_queue);
	kfree(cv->name);
	kfree(cv);
	splx(spl);
}

/* the way it work is that when some consumer threads are waiting*/
void
cv_wait(struct cv *cv, struct lock *lock)
{
	// disable interrupts to ensure atomic 
	// operations on wait_queue, and thread_sleep 
	// will not be interrupted
	int spl = splhigh();
	lock_release(lock);
	// we only need the size of the wait_queue to properly sleep.
	array_add(cv->wait_queue, 0);
	// let the index/address of the wait_queue elements to be the 
	// sleep address for the current thread
	thread_sleep(cv->wait_queue->v + array_getnum(cv->wait_queue) - 1);
	splx(spl);
	/* This lock is for application-level shared resources */
	lock_acquire(lock); 
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	(void*) lock;
	int spl = splhigh();

	if (array_getnum(cv->wait_queue) > 0) {
		// wake up the first thread in queue
		thread_wakeup(cv->wait_queue->v);
		array_remove(cv->wait_queue, 0);
	}
	splx(spl);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	(void*) lock;
	int spl = splhigh();
 
	while (array_getnum(cv->wait_queue) > 0) {
		thread_wakeup(cv->wait_queue->v);
		array_remove(cv->wait_queue, 0);
	}

	splx(spl);
}
