#ifndef __PF_TASKING_HPP__
#define __PF_TASKING_HPP__

#include "sys/ref.hpp"
#include "sys/atomic.hpp"

/*                 *** OVERVIEW OF THE TASKING SYSTEM ***
 *
 * Quick recap of what we have here. Basically, a "tasking system" here means
 * the possibility to schedule and asynchronously run functions in shared memory
 * "system threads". This is basically a thread pool. However, we try to propose
 * more in this API by letting the user:
 * 1 - Define *dependencies* between tasks
 * 2 - Setup priorities for each task ie a higher priority task will be more
 *     likely executed than a lower priority one
 * 3 - Setup affinities for each of them ie a task can be "pinned" on some
 *     specific hardware thread (typically useful when something depends on a
 *     context like an OpenGL context)
 *
 * The core of this tasking system is a "Task". A task represent a function to
 * call (to run) later. Each task can specify dependencies in two ways:
 * 1 - "Start dependencies" specified (see below) by Task::starts. Basically, to
 *     be able to start, a task must have all its start dependencies *ended*
 * 2 - "End dependencies" specified (see below) by Task::ends. In that case, tgo
 *     be able to finish, a task must have all its end dependencies *ended*
 *
 * Specifying dependencies in that way allows the user to *dynamically* (ie
 * during the task execution) create a direct acyclic graph of tasks (DAG). One
 * may look at the unit tests to see how it basically works.
 *
 * We also classicaly implement a TaskSet which is a function which can be run
 * n times (concurrently on any number of threads). TaskSet are a particularly
 * way to logically create n tasks in one chunk.
 *
 * Last feature we added is the ability to run *some* task (ie the user cannot
 * decide what it will run) from a running task (ie from the run function of the
 * task). The basic idea here is to overcome typical issues with tasking system:
 * How do you handle asynchronous/non-blocking IO? You may want to reschedule a
 * task if the IO takes too long. But in that case, how can you be sure that the
 * scheduler is not going to run the task you just pushed? Our idea (to assert
 * in later tests) is to offer the ability to run *something* already ready to
 * hide the IO latency from the task itself. At least, you can keep the HW
 * thread busy if you want to.
 *
 *                 *** SOME DETAILS ABOUT THE IMPLEMENTATION ***
 *
 * First thing is the commentaries in tasking.cpp which give some details about
 * the implementation. Roughly the implementation is done around three
 * components:
 *
 * 1 - A fast, distributed, fixed size growing pool to allocate / deallocate the
 *     tasks. OK, to be honest, the fact it is a growing pool is a rather
 *     aggressive approach which clearly should be refined later. Well, actually
 *     it is damn fast. I think the idea to keep speed while reducing memory
 *     footprint may be the implementation of a asynchronous reclaim using the
 *     tasking system itself.
 *
 * 2 - A work-stealing technique for all tasks that do not have any affinity.
 *     Basically, each HW thread in the thread pool has his own queue. Each
 *     thread can push new task *only in its own queue*. When a thread tries to
 *     find a ready task, it first tries to pick one from its queue in depth
 *     first order. If its queue is empty, it tries to *steal* a task from
 *     another HW thread in breadth first order. This approach strongly limits
 *     the memory requirement (ie the number of task currently allocated in the
 *     system) while also limiting the contention in the queues. Right now, the
 *     queues still use spin locks but a classical ABP lock free queue is
 *     clearly coming :-)
 *
 * 3 - A classical FIFO queue approach. Beside its work stealing queue, each
 *     thread owns another FIFO dedicated to tasks with affinities. Basically,
 *     this is more or less the opposite of work stealing: instead of pushing a
 *     affinity task in its own queue, the thread just puts it in the queue
 *     associated to the affinity
 *
 * Finally, note that we handle priorities in a somehow approximate way. Since
 * the system is entirely distributed, it is extremely hard to ensure that, when
 * a high priority task is ready somewhere in the system, it is going to be run
 * as soon as possible. Since it is hard, we use an approximate scheduling
 * strategy by *multiplexing* queues:
 * 0 - If the user returns a task to run, we run it regardless anything else in
 *     the system
 * 1 - Then, we try to pick a task from our own affinity queues. The highest
 *     priority task from these queues is picked up first
 * 2 - If no task was found in 1, we try to pick a task from our own work
 *     stealing queue. Here once again, we pick up the highest priority task
 *     since the four queues (from low to critical) are multiplexed
 * 3 - If no task was found in 2, we try to steal a task from a random other
 *     queue. Here also, we take the one with the highest priority
 *
 * As a final note, this tasking system is basically a mix of ideas from TBB (of
 * course :-) ), various tasking systems I used on Ps3 and also various tasking
 * system I had the chance to use and experiment when I was at Intel Labs during
 * the LRB era.
 */

#define PF_TASK_USE_DEDICATED_ALLOCATOR 1

namespace pf {

  /*! A task with a higher priority will be preferred to a task with a lower
   *  priority. Note that the system does not completely comply with
   *  priorities. Basically, because the system is distributed, it is possible
   *  that one particular worker thread processes a low priority task while
   *  another thread actually has higher priority tasks currently available
   */
  struct TaskPriority {
    enum {
      CRITICAL = 0u,
      HIGH     = 1u,
      NORMAL   = 2u,
      LOW      = 3u,
      NUM      = 4u,
      INVALID  = 0xffffu
    };
  };

  /*! Describe the current state of a task. This is only used in DEBUG mode to
   *  assert the correctness of the operations (like Task::starts or Task::ends
   *  which only operates on tasks with specific states). To be honest, this is a
   *  bit bullshit code. I think using different proxy types for tasks based on
   *  their state is the way to go. This would enforce correctness of the code
   *  through the typing system which is just better since static. Anyway.
   */
#ifndef NDEBUG
  struct TaskState {
    enum {
      NEW       = 0u,
      READY     = 1u,
      RUNNING   = 2u,
      DONE      = 3u,
      NUM       = 4u,
      INVALID   = 0xffffu
    };
  };
#endif /* NDEBUG */

  /*! Interface for all tasks handled by the tasking system */
  class Task : public RefCount
  {
  public:
    /*! It can complete one task and can be continued by one other task */
    INLINE Task(const char *taskName = NULL) :
      name(taskName),
      toStart(1), toEnd(1),
      priority(uint16(TaskPriority::NORMAL)), affinity(0xffffu)
#ifndef NDEBUG
      , state(uint16(TaskState::NEW))
#endif
    {
      // The scheduler will remove this reference once the task is done
      this->refInc();
    }
    /*! To override while specifying a task. This is basically the code to
     * execute. The user can optionally return a task which will by-pass the
     * scheduler and will run *immediately* after this one. This is a classical
     * continuation passing style strategy when we have a depth first scheduling
     */
    virtual Task* run(void) = 0;
    /*! Task is built and will be ready when all start dependencies are over */
    void scheduled(void);
    /*! The given task cannot *start* as long as "other" is not complete */
    INLINE void starts(Task *other) {
      if (UNLIKELY(other == NULL)) return;
      assert(other->state == TaskState::NEW);
      if (UNLIKELY(this->toBeStarted)) return; // already a task to start
      other->toStart++;
      this->toBeStarted = other;
    }
    /*! The given task cannot *end* as long as "other" is not complete */
    INLINE void ends(Task *other) {
      if (UNLIKELY(other == NULL)) return;
      assert(other->state == TaskState::NEW ||
             other->state == TaskState::RUNNING);
      if (UNLIKELY(this->toBeEnded)) return;  // already a task to end
      other->toEnd++;
      this->toBeEnded = other;
    }
    /*! Set / get task priority and affinity */
    INLINE void setPriority(uint16 prio) {
      assert(this->state == TaskState::NEW);
      this->priority = prio;
    }
    INLINE void setAffinity(uint16 affi) {
      assert(this->state == TaskState::NEW);
      this->affinity = affi;
    }
    INLINE uint16 getPriority(void) const { return this->priority; }
    INLINE uint16 getAffinity(void) const { return this->affinity; }

#if PF_TASK_USE_DEDICATED_ALLOCATOR
    /*! Tasks use a scalable fixed size allocator */
    void* operator new(size_t size);
    /*! Deallocations go through the dedicated allocator too */
    void operator delete(void* ptr);
#endif /* PF_TASK_USE_DEDICATED_ALLOCATOR */

  private:
    friend class TaskSet;        //!< Will tweak the ending criterium
    friend class TaskScheduler;  //!< Needs to access everything
    Ref<Task> toBeEnded;         //!< Signals it when finishing
    Ref<Task> toBeStarted;       //!< Triggers it when ready
    const char *name;            //!< Debug facility mostly
    Atomic32 toStart;            //!< MBZ before starting
    Atomic32 toEnd;              //!< MBZ before ending
    uint16 priority;             //!< Task priority
    uint16 affinity;             //!< The task will run on a particular thread
  public: IF_DEBUG(uint16 state);//!< Will assert correctness of the operations
  };

  /*! Allow the run function to be executed several times */
  class TaskSet : public Task
  {
  public:
    /*! elemNum is the number of times to execute the run function */
    INLINE TaskSet(size_t elemNum, const char *name = NULL) :
      Task(name), elemNum(elemNum) {}
    /*! This function is user-specified */
    virtual void run(size_t elemID) = 0;

  private:
    virtual Task* run(void);  //!< Reimplemented for all task sets
    Atomic elemNum;          //!< Number of outstanding elements
  };

  /*! Mandatory before creating and running any task (MAIN THREAD) */
  void TaskingSystemStart(void);

  /*! Make the main thread enter the tasking system (MAIN THREAD) */
  void TaskingSystemEnd(void);

  /*! Make the main thread enter the tasking system (MAIN THREAD) */
  void TaskingSystemEnter(void);

  /*! Signal the *main* thread only to stop (THREAD SAFE) */
  void TaskingSystemInterruptMain(void);

  /*! Signal *all* threads to stop (THREAD SAFE) */
  void TaskingSystemInterrupt(void);

  /*! Run any task (in READY state) in the system. Can be used from a task::run
   *  to overlap some IO for example. Return true if anything was executed
   */
  bool TaskingSystemRunAnyTask(void);

} /* namespace pf */

#endif /* __PF_TASKING_HPP__ */

