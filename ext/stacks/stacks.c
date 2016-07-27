/*
 * General idioms:
 *   - rb_* functions are attached to Ruby-accessible method calls (See Init_stacks)
 * General approach:
 *   - Because of how rb_profile_frames works, it must be called from within
 *     each thread running, rather than from a 3rd party thread.
 *   - We setup a global timer tick. The handler simply sends a thread signal
 *     to each registered thread, which causes each thread to capture its own
 *     trace
 */

/*
 * Ruby lib
 */
#include <ruby/ruby.h>
#include <ruby/debug.h>
#include <ruby/st.h>
#include <ruby/io.h>
#include <ruby/intern.h>

/*
 * Std lib
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/time.h>

int scout_profiling_installed = 0;
int scout_profiling_running = 0;

ID sym_ScoutApm;
ID sym_Stacks;
ID sym_collect;
ID sym_scrub_bang;
VALUE ScoutApm;
VALUE Stacks;

VALUE mScoutApm;
VALUE mInstruments;
VALUE cStacks;

VALUE interval;

#define BUF_SIZE 512
#define MAX_TRACES 2000

#define NANO_SECOND_MULTIPLIER  1000000  // 1 millisecond = 1,000,000 Nanoseconds
const long INTERVAL = 1 * NANO_SECOND_MULTIPLIER; // milliseconds * NANO_SECOND_MULTIPLIER

#ifdef RUBY_INTERNAL_EVENT_NEWOBJ

// Forward Declarations
static void init_thread_vars();
static void scout_profile_broadcast_signal_handler(int sig);
void scout_record_sample();

////////////////////////////////////////////////////////////////////////////////////////
// Per-Thread variables
////////////////////////////////////////////////////////////////////////////////////////

struct c_trace {
  int num_tracelines;
  int lines_buf[BUF_SIZE];
  VALUE frames_buf[BUF_SIZE];
};

static __thread struct c_trace *_traces;
static __thread atomic_bool _ok_to_sample, _in_signal_handler = ATOMIC_VAR_INIT(false);
static __thread atomic_uint_fast16_t _start_frame_index, _start_trace_index, _cur_traces_num = ATOMIC_VAR_INIT(0);
static __thread atomic_uint_fast32_t _skipped_in_gc, _skipped_in_signal_handler = ATOMIC_VAR_INIT(0);
static __thread VALUE _gc_hook;

////////////////////////////////////////////////////////////////////////////////////////
// Globald variables
////////////////////////////////////////////////////////////////////////////////////////

static atomic_bool _job_registered = ATOMIC_VAR_INIT(false);

////////////////////////////////////////////////////////////////////////////////////////
// Thread Linked List
////////////////////////////////////////////////////////////////////////////////////////

/*
 * Because of how rb_profile_frames works, we need to call it from inside of each thread
 * in ScoutProf.  To do this, we have a global linked list.  Each thread needs to register itself
 * via rb_scout_add_profiled_thread()
 */

/*
 * profiled_thread is a node in the linked list of threads.
 */
struct profiled_thread
{
  pthread_t th;
  struct profiled_thread *next;
};

/*
 * head_thread: The head of the linked list
 */
struct profiled_thread *head_thread = NULL;

// Mutex around editing of the thread linked list
pthread_mutex_t profiled_threads_mutex = PTHREAD_MUTEX_INITIALIZER;


// Background controller thread ID
pthread_t btid;


/*
 * rb_scout_add_profiled_thread: adds the currently running thread to the head of the linked list
 *
 * Initializes thread locals:
 *   - ok_to_sample to false
 *   - start_frame_index and start_trace_index to 0
 *   - cur_traces_num to 0
 */
static VALUE rb_scout_add_profiled_thread(VALUE self)
{
  struct profiled_thread *thr;
  init_thread_vars();

  pthread_mutex_lock(&profiled_threads_mutex);

  thr = (struct profiled_thread *) malloc(sizeof(struct profiled_thread));
  thr->th = pthread_self();
  thr->next = NULL;
  if (head_thread == NULL) {
    head_thread = thr;
  } else {
    thr->next = head_thread;
    head_thread = thr; // now we're head_thread
  }

  pthread_mutex_unlock(&profiled_threads_mutex);
  return Qtrue;
}

/*
 * remove_profiled_thread: removes a thread from the linked list.
 * if the linked list is empty, this is a noop
 */
static int remove_profiled_thread()
{
  struct profiled_thread *ptr, *prev;
  pthread_t th = pthread_self();

  atomic_store(&_ok_to_sample, false);

  pthread_mutex_lock(&profiled_threads_mutex);

  prev = NULL;

  for(ptr = head_thread; ptr != NULL; prev = ptr, ptr = ptr->next) {
    if (pthread_equal(th, ptr->th)) {
      if (prev == NULL) {
        head_thread = ptr->next; // We are head_thread
      } else {
        prev->next = ptr->next; // We are not head thread
      }
      free(ptr);
      break;
    }
  }

  pthread_mutex_unlock(&profiled_threads_mutex);

  rb_gc_unregister_address(&_gc_hook);
  xfree(&_traces);

  return 0;
}

/* rb_scout_remove_profiled_thread: removes a thread from the linked list
 *
 * Turns off _ok_to_sample, then proxies to remove_profiled_thread
 */
static VALUE rb_scout_remove_profiled_thread(VALUE self)
{
  remove_profiled_thread();
  return Qtrue;
}


/*
 *  The thread_cleanup_handler is pushed onto every registered thread's cancellation cleanup handlers.
 *  This lets us run cleanup in the context of the exiting thread since we may have no way to hook into
 *  the thread exit in ruby.
 */
static void
thread_cleanup_handler(void *arg)
{
  remove_profiled_thread();
}

////////////////////////////////////////////////////////////////////////////////////////
// Global timer
////////////////////////////////////////////////////////////////////////////////////////

static void
scout_signal_threads_to_profile()
{
    struct profiled_thread *ptr;

    if (pthread_mutex_trylock(&profiled_threads_mutex) == 0) { // Only run if we get the mutex.
      ptr = head_thread;
      while(ptr != NULL) {
        if (pthread_kill(ptr->th, 0) != ESRCH) { // Check for existence of thread. If ESRCH is returned, don't send the real signal!
          pthread_kill(ptr->th, SIGVTALRM);
        }
        ptr = ptr->next;
      }
      pthread_mutex_unlock(&profiled_threads_mutex);
    }

    atomic_store(&_job_registered, false);
}

// Should we block signals to this thread?
void *
background_worker()
{
  int clock_result, register_result;
  struct timespec clock_remaining;
  struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = INTERVAL};

  while (1) {
    //check to see if we should change values, exit, etc
    SNOOZE:
#ifdef CLOCK_MONOTONIC
    clock_result = clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, &clock_remaining);
#else
    clock_result = nanosleep(&sleep_time, &clock_remaining);
    if (clock_result == -1) {
      clock_result = errno;
    }
#endif

    if (clock_result == 0) {
      if (rb_during_gc()) {
        //_skipped_in_gc++;
      } else {
        if (atomic_load(&_job_registered) == false){
          register_result = rb_postponed_job_register_one(0, scout_signal_threads_to_profile, 0);
          if ((register_result == 1) || (register_result == 2)) {
            atomic_store(&_job_registered, true);
          } else {
            fprintf(stderr, "Error: job was not registered! Result: %d\n", register_result);
          }
        } // !_job_registered
      }
    } else if (clock_result == EINTR) {
      sleep_time = clock_remaining;
      goto SNOOZE;
    } else {
      fprintf(stderr, "Error: nanosleep returned value : %d\n", clock_result);
    }
  }
}

/* rb_scout_start_profiling: Installs the global timer
 */
static VALUE
rb_scout_start_profiling(VALUE self)
{
  if (scout_profiling_running) {
    return Qtrue;
  }

  rb_warn("Starting Profiling");
  scout_profiling_running = 1;

  return Qtrue;
}

/* rb_scout_uninstall_profiling: removes global timer, and removes global SIGALRM handler
 */
static VALUE
rb_scout_uninstall_profiling(VALUE self)
{
  // stop background worker threads
  pthread_cancel(btid); // TODO: doing a pthread_join after cancel is the only way to wait and know if the thread actually exited.

  return Qnil;
}

static VALUE
rb_scout_install_profiling(VALUE self)
{
  struct sigaction new_vtaction, old_vtaction;

  // We can only install once. If uninstall is called, we will NOT be able to call install again.
  // Instead, stop/start should be used to temporarily disable all ScoutProf sampling.
  if (scout_profiling_installed) {
    return Qfalse;
  }

  pthread_create(&btid, NULL, background_worker, NULL);

  // Also set up an interrupt handler for when we broadcast an alarm
  new_vtaction.sa_handler = scout_profile_broadcast_signal_handler;
  new_vtaction.sa_flags = SA_RESTART;
  sigemptyset(&new_vtaction.sa_mask);
  sigaction(SIGVTALRM, &new_vtaction, &old_vtaction);

  rb_define_const(cStacks, "INSTALLED", Qtrue);
  scout_profiling_installed = 1;

  return Qtrue;
}

////////////////////////////////////////////////////////////////////////////////////////
// Per-Thread Handler
////////////////////////////////////////////////////////////////////////////////////////

static void
scoutprof_gc_mark(void *data)
{
  uint_fast16_t i;
  int n;
  for (i = 0; i < atomic_load(&_cur_traces_num); i++) {
    for (n = 0; n < _traces[i].num_tracelines; n++) {
      rb_gc_mark(_traces[i].frames_buf[n]);
    }
  }
}

static void
init_thread_vars()
{
  atomic_store(&_ok_to_sample, false);
  atomic_store(&_in_signal_handler, false);
  atomic_store(&_start_frame_index, 0);
  atomic_store(&_start_trace_index, 0);
  atomic_store(&_cur_traces_num, 0);

  _traces = ALLOC_N(struct c_trace, MAX_TRACES); // TODO Check return

  _gc_hook = Data_Wrap_Struct(rb_cObject, &scoutprof_gc_mark, NULL, &_traces);
  rb_gc_register_address(&_gc_hook);

  //pthread_cleanup_push(thread_cleanup_handler, NULL);

  return;
}

/* scout_profile_broadcast_signal_handler: Signal handler for each thread. 
 *
 * proxies off to scout_profile_job_handler via Ruby's rb_postponed_job_register
 */
static void
scout_profile_broadcast_signal_handler(int sig)
{
  if (atomic_load(&_ok_to_sample) == false) return;

  if (atomic_load(&_in_signal_handler) == true) {
    atomic_fetch_add(&_skipped_in_signal_handler, 1);
    return;
  }

  atomic_store(&_in_signal_handler, true);

  scout_record_sample();

  atomic_store(&_in_signal_handler, false);
}

/*
 * scout_record_sample: Defered function run from the per-thread handler
 *
 * Note: that this is called from *EVERY PROFILED THREAD FOR EACH CLOCK TICK
 *       INTERVAL*, so the performance of this method is crucial.
 *
 *  A fair bit of code, but fairly simple logic:
 *   * bail out early if we have sampling off
 *   * bail out early if GC is running
 *   * bail out early if we've filled the traces buffer
 *   * run rb_profile_frames
 *   * extract various info from the frames, and store it in _traces
 */
void
scout_record_sample()
{
  int num_frames;
  uint_fast16_t cur_traces_num, start_frame_index;

  if (atomic_load(&_ok_to_sample) == false) return;
  if (rb_during_gc()) {
    atomic_fetch_add(&_skipped_in_gc, 1);
    return;
  }

  cur_traces_num = atomic_load(&_cur_traces_num);
  start_frame_index = atomic_load(&_start_frame_index);

  if (cur_traces_num < MAX_TRACES) {
    num_frames = rb_profile_frames(0, sizeof(_traces[cur_traces_num].frames_buf) / sizeof(VALUE), _traces[cur_traces_num].frames_buf, _traces[cur_traces_num].lines_buf);
    if (num_frames - start_frame_index > 2) {
      _traces[cur_traces_num].num_tracelines = num_frames - start_frame_index - 2; // The extra -2 is because there's a bug when reading the very first (bottom) 2 iseq objects for some reason
      atomic_fetch_add(&_cur_traces_num, 1);
    }
    // TODO: add an else with a counter so we can track if we skipped profiling here
  }
}

/* rb_scout_profile_frames: retreive the traces for the layer that is exiting
 *
 * Note: Calls to this must have already stopped sampling
 */
static VALUE rb_scout_profile_frames(VALUE self)
{
  int n;
  uint_fast16_t i, cur_traces_num, start_trace_index;
  VALUE traces, trace, trace_line;

  cur_traces_num = atomic_load(&_cur_traces_num);
  start_trace_index = atomic_load(&_start_trace_index);

  if (cur_traces_num - start_trace_index > 0) {
    fprintf(stderr, "CUR TRACES: %"PRIuFAST16"\n", cur_traces_num);
    fprintf(stderr, "START TRACE IDX: %"PRIuFAST16"\n", start_trace_index);
    fprintf(stderr, "TRACES COUNT: %"PRIuFAST16"\n", cur_traces_num - start_trace_index);
    traces = rb_ary_new2(cur_traces_num - start_trace_index);
    for(i = start_trace_index; i < cur_traces_num; i++) {
      if (_traces[i].num_tracelines > 0) {
        trace = rb_ary_new2(_traces[i].num_tracelines);
        for(n = 0; n < _traces[i].num_tracelines; n++) {
          trace_line = rb_ary_new2(2);
          rb_ary_store(trace_line, 0, _traces[i].frames_buf[n]);
          rb_ary_store(trace_line, 1, INT2FIX(_traces[i].lines_buf[n]));
          rb_ary_push(trace, trace_line);
        }
        rb_ary_push(traces, trace);
      }
    }
  } else {
    traces = rb_ary_new();
  }
  atomic_store(&_cur_traces_num, start_trace_index);
  return traces;
}



/*****************************************************/
/* Control code */
/*****************************************************/

/* Per thread start sampling */
static VALUE
rb_scout_start_sampling(VALUE self)
{
  atomic_store(&_ok_to_sample, true);
  return Qtrue;
}

/* Per thread stop sampling */
static VALUE
rb_scout_stop_sampling(VALUE self, VALUE reset)
{
  atomic_store(&_ok_to_sample, false);
  // TODO: I think this can be (reset == Qtrue)
  if (TYPE(reset) == T_TRUE) {
    atomic_store(&_cur_traces_num, 0);
    atomic_store(&_skipped_in_gc, 0);
    atomic_store(&_skipped_in_signal_handler, 0);
  }
  return Qtrue;
}

// rb_scout_update_indexes: Called when each layer starts or something
static VALUE
rb_scout_update_indexes(VALUE self, VALUE frame_index, VALUE trace_index)
{
  atomic_store(&_start_trace_index, NUM2INT(trace_index));
  atomic_store(&_start_frame_index, NUM2INT(frame_index));
  return Qtrue;
}

// rb_scout_current_trace_index: Get the current top of the trace stack
static VALUE
rb_scout_current_trace_index(VALUE self)
{
  return INT2NUM(atomic_load(&_cur_traces_num));
}

// rb_scout_current_trace_index: Get the current top of the trace stack
static VALUE
rb_scout_current_frame_index(VALUE self)
{
  int num_frames;
  VALUE frames_buf[BUF_SIZE];
  int lines_buf[BUF_SIZE];
  num_frames = rb_profile_frames(0, sizeof(frames_buf) / sizeof(VALUE), frames_buf, lines_buf);
  if (num_frames > 1) {
    return INT2NUM(num_frames - 1);
  } else {
    return INT2NUM(0);
  }
}

static VALUE
rb_scout_klass_for_frame(VALUE self, VALUE frame)
{
  return rb_profile_frame_classpath(frame);
}

// Gem Init. Set up constants, attach methods
void Init_stacks()
{
    mScoutApm = rb_define_module("ScoutApm");
    mInstruments = rb_define_module_under(mScoutApm, "Instruments");
    cStacks = rb_define_class_under(mInstruments, "Stacks", rb_cObject);

    sym_ScoutApm = rb_intern("ScoutApm");
    sym_Stacks = rb_intern("Stacks");
    sym_collect = rb_intern("collect");

    ScoutApm = rb_const_get(rb_cObject, sym_ScoutApm);
    Stacks = rb_const_get(ScoutApm, sym_Stacks);
    rb_warn("Init_stacks");

    // Installs/uninstalls the signal handler.
    rb_define_singleton_method(cStacks, "install", rb_scout_install_profiling, 0);
    rb_define_singleton_method(cStacks, "uninstall", rb_scout_uninstall_profiling, 0);

    rb_define_singleton_method(cStacks, "start", rb_scout_start_profiling, 0);

    rb_define_singleton_method(cStacks, "add_profiled_thread", rb_scout_add_profiled_thread, 0);
    rb_define_singleton_method(cStacks, "remove_profiled_thread", rb_scout_remove_profiled_thread, 0);

    rb_define_singleton_method(cStacks, "profile_frames", rb_scout_profile_frames, 0);
    rb_define_singleton_method(cStacks, "start_sampling", rb_scout_start_sampling, 0);
    rb_define_singleton_method(cStacks, "stop_sampling", rb_scout_stop_sampling, 1);
    rb_define_singleton_method(cStacks, "update_indexes", rb_scout_update_indexes, 2);
    rb_define_singleton_method(cStacks, "current_trace_index", rb_scout_current_trace_index, 0);
    rb_define_singleton_method(cStacks, "current_frame_index", rb_scout_current_frame_index, 0);
    rb_define_singleton_method(cStacks, "klass_for_frame", rb_scout_klass_for_frame, 1);

    rb_define_const(cStacks, "ENABLED", Qtrue);
    rb_warn("Finished Initializing ScoutProf Native Extension");
}

#else

void scout_install_profiling(VALUE module)
{
  return Qnil;
}

void scout_uninstall_profiling(VALUE module)
{
  return Qnil;
}

void scout_start_profiling(VALUE module)
{
  return Qnil;
}

void scout_stop_profiling(VALUE module)
{
  return Qnil;
}

void rb_scout_add_profiled_thread(VALUE module)
{
  return Qnil;
}

void rb_scout_remove_profiled_thread(VALUE module)
{
  return Qnil;
}

static VALUE rb_scout_profile_frames(VALUE self)
{
  return rb_ary_new();
}

static VALUE
rb_scout_start_sampling(VALUE self)
{
  return Qtrue;
}

static VALUE
rb_scout_update_indexes(VALUE self, VALUE frame_index, VALUE trace_index)
{
  return Qtrue;
}

// rb_scout_current_trace_index: Get the current top of the trace stack
static VALUE
rb_scout_current_trace_index(VALUE self)
{
  return INT2NUM(0);
}

// rb_scout_current_trace_index: Get the current top of the trace stack
static VALUE
rb_scout_current_frame_index(VALUE self)
{
  return INT2NUM(0);
}

static VALUE
rb_scout_klass_for_frame(VALUE self, VALUE frame)
{
  return Qnil;
}

void Init_stacks()
{
    mScoutApm = rb_define_module("ScoutApm");
    mInstruments = rb_define_module_under(mScoutApm, "Instruments");
    cStacks = rb_define_class_under(mInstruments, "Stacks", rb_cObject);

    // Installs/uninstalls the signal handler.
    rb_define_singleton_method(cStacks, "install", rb_scout_install_profiling, 0);
    rb_define_singleton_method(cStacks, "uninstall", rb_scout_uninstall_profiling, 0);

    // Starts/removes the timer tick, leaving the sighandler.
    rb_define_singleton_method(cStacks, "start", rb_scout_start_profiling, 0);
    rb_define_singleton_method(cStacks, "stop", rb_scout_stop_profiling, 0);

    rb_define_singleton_method(cStacks, "add_profiled_thread", rb_scout_add_profiled_thread, 0);
    rb_define_singleton_method(cStacks, "remove_profiled_thread", rb_scout_remove_profiled_thread, 0);

    rb_define_singleton_method(cStacks, "profile_frames", rb_scout_profile_frames, 0);
    rb_define_singleton_method(cStacks, "start_sampling", rb_scout_start_sampling, 0);
    rb_define_singleton_method(cStacks, "stop_sampling", rb_scout_stop_sampling, 1);
    rb_define_singleton_method(cStacks, "update_indexes", rb_scout_update_indexes, 2);
    rb_define_singleton_method(cStacks, "current_trace_index", rb_scout_current_trace_index, 0);
    rb_define_singleton_method(cStacks, "current_frame_index", rb_scout_current_frame_index, 0);
    rb_define_singleton_method(cStacks, "klass_for_frame", rb_scout_klass_for_frame, 1);

    rb_define_const(cStacks, "ENABLED", Qfalse);
    rb_define_const(cStacks, "INSTALLED", Qfalse);
}

#endif //#ifdef RUBY_INTERNAL_EVENT_NEWOBJ
