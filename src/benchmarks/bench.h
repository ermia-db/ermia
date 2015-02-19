#ifndef _NDB_BENCH_H_
#define _NDB_BENCH_H_

#include <stdint.h>

#include <map>
#include <vector>
#include <utility>
#include <string>

#include "abstract_db.h"
#include "../macros.h"
#include "../thread.h"
#include "../util.h"
#include "../spinbarrier.h"
#include "../dbcore/rcu.h"
#include "../dbcore/sm-alloc.h"
#include "../dbcore/serial.h"
#include "../dbcore/sm-trace.h"
#include "../dbcore/sm-rc.h"
#include <stdio.h>
#include <sys/mman.h> // Needed for mlockall()
#include <malloc.h>
#include <sys/time.h> // needed for getrusage
#include <sys/resource.h> // needed for getrusage
#include <numa.h>
#include <vector>
#include <set>

extern void ycsb_do_test(abstract_db *db, int argc, char **argv);
extern void tpcc_do_test(abstract_db *db, int argc, char **argv);
extern void tpce_do_test(abstract_db *db, int argc, char **argv);
extern void queue_do_test(abstract_db *db, int argc, char **argv);
extern void encstress_do_test(abstract_db *db, int argc, char **argv);
extern void bid_do_test(abstract_db *db, int argc, char **argv);

enum {
  RUNMODE_TIME = 0,
  RUNMODE_OPS  = 1
};

// benchmark global variables
extern size_t nthreads;
extern volatile bool running;
extern int verbose;
extern uint64_t txn_flags;
extern double scale_factor;
extern uint64_t runtime;
extern uint64_t ops_per_worker;
extern int run_mode;
extern int enable_parallel_loading;
extern int pin_cpus;
extern int slow_exit;
extern int retry_aborted_transaction;
extern int no_reset_counters;
extern int backoff_aborted_transaction;

template <typename T> static std::vector<T>
unique_filter(const std::vector<T> &v)
{
	std::set<T> seen;
	std::vector<T> ret;
	for (auto &e : v)
		if (!seen.count(e)) {
			ret.emplace_back(e);
			seen.insert(e);
		}
	return ret;
}

class scoped_db_thread_ctx {
public:
  scoped_db_thread_ctx(const scoped_db_thread_ctx &) = delete;
  scoped_db_thread_ctx(scoped_db_thread_ctx &&) = delete;
  scoped_db_thread_ctx &operator=(const scoped_db_thread_ctx &) = delete;

  scoped_db_thread_ctx(abstract_db *db, bool loader)
    : db(db)
  {
  }
  ~scoped_db_thread_ctx()
  {
  }
private:
  abstract_db *const db;
};

class bench_loader : public ndb_thread {
public:
  bench_loader(unsigned long seed, abstract_db *db,
               const std::map<std::string, abstract_ordered_index *> &open_tables)
    : r(seed), db(db), open_tables(open_tables), b(0)
  {
    txn_obj_buf.reserve(str_arena::MinStrReserveLength);
    txn_obj_buf.resize(db->sizeof_txn_object(txn_flags));
  }
  inline void
  set_barrier(spin_barrier &b)
  {
    ALWAYS_ASSERT(!this->b);
    this->b = &b;
  }
  virtual void
  run()
  {
#if defined(USE_PARALLEL_SSN) or defined(USE_PARALLEL_SSI)
    assign_reader_bitmap_entry();
#endif
	  // XXX. RCU register/deregister should be the outer most one b/c RA::ra_deregister could call cur_lsn inside
	RCU::rcu_register();
	RA::ra_register();
    ALWAYS_ASSERT(b);
    b->count_down();
    b->wait_for();
    scoped_db_thread_ctx ctx(db, true);
    load();
	RA::ra_deregister();
	RCU::rcu_deregister();
#if defined(USE_PARALLEL_SSN) or defined(USE_PARALLEL_SSI)
    deassign_reader_bitmap_entry();
#endif
  }
  inline ALWAYS_INLINE varstr &
  str(uint64_t size)
  {
    return *arena.next(size);
  }

protected:
  inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

  virtual void load() = 0;

  util::fast_random r;
  abstract_db *const db;
  std::map<std::string, abstract_ordered_index *> open_tables;
  spin_barrier *b;
  std::string txn_obj_buf;
  str_arena arena;
};

class bench_worker : public ndb_thread {
public:

  bench_worker(unsigned int worker_id,
               bool set_core_id,
               unsigned long seed, abstract_db *db,
               const std::map<std::string, abstract_ordered_index *> &open_tables,
               spin_barrier *barrier_a, spin_barrier *barrier_b)
    : worker_id(worker_id), set_core_id(set_core_id),
      r(seed), db(db), open_tables(open_tables),
      barrier_a(barrier_a), barrier_b(barrier_b),
      latency_numer_us(0),
      backoff_shifts(0), // spin between [0, 2^backoff_shifts) times before retry
      // the ntxn_* numbers are per worker
      ntxn_commits(0),
      ntxn_aborts(0),
      ntxn_user_aborts(0),
      ntxn_int_aborts(0),
      ntxn_si_aborts(0),
      ntxn_serial_aborts(0),
      ntxn_rw_aborts(0),
      ntxn_phantom_aborts(0),
      ntxn_query_commits(0),
      size_delta(0)
  {
    txn_obj_buf.reserve(str_arena::MinStrReserveLength);
    txn_obj_buf.resize(db->sizeof_txn_object(txn_flags));
  }

  virtual ~bench_worker() {}

  // returns [did_commit?, size_increase_bytes]
  typedef std::pair<bool, ssize_t> txn_result;
  typedef txn_result (*txn_fn_t)(bench_worker *);

  struct workload_desc {
    workload_desc() {}
    workload_desc(const std::string &name, double frequency, txn_fn_t fn)
      : name(name), frequency(frequency), fn(fn)
    {
      ALWAYS_ASSERT(frequency > 0.0);
      ALWAYS_ASSERT(frequency <= 1.0);
    }
    std::string name;
    double frequency;
    txn_fn_t fn;
  };
  typedef std::vector<workload_desc> workload_desc_vec;
  virtual workload_desc_vec get_workload() const = 0;

  virtual void run();

  inline size_t get_ntxn_commits() const { return ntxn_commits; }
  inline size_t get_ntxn_aborts() const { return ntxn_aborts; }
  inline size_t get_ntxn_user_aborts() const { return ntxn_user_aborts; }
  inline size_t get_ntxn_si_aborts() const { return ntxn_si_aborts; }
  inline size_t get_ntxn_serial_aborts() const { return ntxn_serial_aborts; }
  inline size_t get_ntxn_rw_aborts() const { return ntxn_rw_aborts; }
  inline size_t get_ntxn_int_aborts() const { return ntxn_int_aborts; }
  inline size_t get_ntxn_phantom_aborts() const { return ntxn_phantom_aborts; }
  inline size_t get_ntxn_query_commits() const { return ntxn_query_commits; }
  inline void inc_ntxn_user_aborts() { ++ntxn_user_aborts; }
  inline void inc_ntxn_si_aborts() { ++ntxn_si_aborts; }
  inline void inc_ntxn_serial_aborts() { ++ntxn_serial_aborts; }
  inline void inc_ntxn_rw_aborts() { ++ntxn_rw_aborts; }
  inline void inc_ntxn_int_aborts() { ++ntxn_int_aborts; }
  inline void inc_ntxn_phantom_aborts() { ++ntxn_phantom_aborts; }
  inline void inc_ntxn_query_commits() { ++ntxn_query_commits; }

  inline uint64_t get_latency_numer_us() const { return latency_numer_us; }

  inline double
  get_avg_latency_us() const
  {
    return double(latency_numer_us) / double(ntxn_commits);
  }

  std::map<std::string, size_t> get_txn_counts() const;

  typedef abstract_db::counter_map counter_map;
  typedef abstract_db::txn_counter_map txn_counter_map;

#ifdef ENABLE_BENCH_TXN_COUNTERS
  inline txn_counter_map
  get_local_txn_counters() const
  {
    return local_txn_counters;
  }
#endif

  inline ssize_t get_size_delta() const { return size_delta; }

protected:

  virtual void on_run_setup() {}

  inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

  unsigned int worker_id;
  bool set_core_id;
  util::fast_random r;
  abstract_db *const db;
  std::map<std::string, abstract_ordered_index *> open_tables;
  spin_barrier *const barrier_a;
  spin_barrier *const barrier_b;

private:
  uint64_t latency_numer_us;
  unsigned backoff_shifts;

  // stats
  size_t ntxn_commits;
  size_t ntxn_aborts;
  size_t ntxn_user_aborts;
  size_t ntxn_int_aborts;
  size_t ntxn_si_aborts;
  size_t ntxn_serial_aborts;
  size_t ntxn_rw_aborts;
  size_t ntxn_phantom_aborts;
  size_t ntxn_query_commits;

protected:

#ifdef ENABLE_BENCH_TXN_COUNTERS
  txn_counter_map local_txn_counters;
  void measure_txn_counters(void *txn, const char *txn_name);
#else
  inline ALWAYS_INLINE void measure_txn_counters(void *txn, const char *txn_name) {}
#endif

  std::vector<size_t> txn_counts; // breakdown of txns
  ssize_t size_delta; // how many logical bytes (of values) did the worker add to the DB

  std::string txn_obj_buf;
  str_arena arena;
};

class bench_runner {
public:
  bench_runner(const bench_runner &) = delete;
  bench_runner(bench_runner &&) = delete;
  bench_runner &operator=(const bench_runner &) = delete;

  bench_runner(abstract_db *db)
    : db(db), barrier_a(nthreads), barrier_b(1) {}
  virtual ~bench_runner() {}
  void run();
  void heap_prefault()
  {
#ifndef CHECK_INVARIANTS
	  uint64_t FAULT_SIZE = (((uint64_t)1<<30)*40);		// 45G for 24 warehouses
	  uint8_t* p = (uint8_t*)malloc( FAULT_SIZE );
	  ALWAYS_ASSERT(p);
      ALWAYS_ASSERT(not mlock(p, FAULT_SIZE));
	  mallopt (M_TRIM_THRESHOLD, -1);
	  mallopt (M_MMAP_MAX, 0);

	  struct rusage usage;
	  getrusage(RUSAGE_SELF, &usage);
	  std::cout<<"Major fault: " <<  usage.ru_majflt<< "Minor fault: " << usage.ru_minflt<< std::endl;

	  free(p);
#endif
  }
protected:
  // only called once
  virtual std::vector<bench_loader*> make_loaders() = 0;

  // only called once
  virtual std::vector<bench_worker*> make_workers() = 0;

  abstract_db *const db;
  std::map<std::string, abstract_ordered_index *> open_tables;

  // barriers for actual benchmark execution
  spin_barrier barrier_a;
  spin_barrier barrier_b;
};

// XXX(stephentu): limit_callback is not optimal, should use
// static_limit_callback if possible
class limit_callback : public abstract_ordered_index::scan_callback {
public:
  limit_callback(ssize_t limit = -1)
    : limit(limit), n(0)
  {
    ALWAYS_ASSERT(limit == -1 || limit > 0);
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const varstr &value)
  {
    INVARIANT(limit == -1 || n < size_t(limit));
    values.emplace_back(varstr(keyp, keylen), value);
    return (limit == -1) || (++n < size_t(limit));
  }

  typedef std::pair<varstr, varstr> kv_pair;
  std::vector<kv_pair> values;

  const ssize_t limit;
private:
  size_t n;
};


class latest_key_callback : public abstract_ordered_index::scan_callback {
public:
  latest_key_callback(varstr &k, ssize_t limit = -1)
    : limit(limit), n(0), k(&k)
  {
    ALWAYS_ASSERT(limit == -1 || limit > 0);
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const varstr &value)
  {
    INVARIANT(limit == -1 || n < size_t(limit));
    k->copy_from(keyp, keylen);
    ++n;
    return (limit == -1) || (n < size_t(limit));
  }

  inline size_t size() const { return n; }
  inline varstr &kstr() { return *k; }

private:
  ssize_t limit;
  size_t n;
  varstr *k;
};

// explicitly copies keys, because btree::search_range_call() interally
// re-uses a single string to pass keys (so using standard string assignment
// will force a re-allocation b/c of shared ref-counting)
//
// this isn't done for values, because each value has a distinct string from
// the string allocator, so there are no mutations while holding > 1 ref-count
template <size_t N>
class static_limit_callback : public abstract_ordered_index::scan_callback {
public:
  // XXX: push ignore_key into lower layer
  static_limit_callback(str_arena *arena, bool ignore_key)
    : n(0), arena(arena), ignore_key(ignore_key)
  {
    static_assert(N > 0, "xx");
  }

  virtual bool invoke(
      const char *keyp, size_t keylen,
      const varstr &value)
  {
    INVARIANT(n < N);
    INVARIANT(arena->manages(&value));
    if (ignore_key) {
      values.emplace_back(nullptr, &value);
    } else {
      varstr * const s_px = arena->next(keylen);
      INVARIANT(s_px);
      s_px->copy_from(keyp, keylen);
      values.emplace_back(s_px, &value);
    }
    return ++n < N;
  }

  inline size_t
  size() const
  {
    return values.size();
  }

  typedef std::pair<const varstr *, const varstr *> kv_pair;
  typename util::vec<kv_pair, N>::type values;

private:
  size_t n;
  str_arena *arena;
  bool ignore_key;
};

#define __abort_txn(r) \
{   \
  ASSERT(r._val != RC_ABORT and r._val & RC_ABORT); \
  switch(r._val){\
    case RC_ABORT_SERIAL: inc_ntxn_serial_aborts(); break;\
    case RC_ABORT_SI_CONFLICT: inc_ntxn_si_aborts(); break;\
    case RC_ABORT_RW_CONFLICT: inc_ntxn_rw_aborts(); break;\
    case RC_ABORT_INTERNAL: inc_ntxn_int_aborts(); break;\
    case RC_ABORT_PHANTOM: inc_ntxn_phantom_aborts(); break;\
    default: ALWAYS_ASSERT(false);\
  }\
  db->abort_txn(txn); \
  return bench_worker::txn_result(false, 0); \
}

// NOTE: only use these in transaction benchmark (e.g., TPCC) code, not in engine code

// reminescent the try...catch block:
// if return code is one of those RC_ABORT* then abort
#define try_catch(rc) \
{ \
  rc_t r = rc; \
  if (rc_is_abort(r)) \
    __abort_txn(r); \
}

// if rc == RC_FALSE then do op
#define try_catch_cond(rc, op) \
{ \
  rc_t r = rc; \
  if (rc_is_abort(r)) \
    __abort_txn(r); \
  if (r._val == RC_FALSE) \
    op; \
}

#define try_catch_cond_abort(rc) \
{ \
  rc_t r = rc; \
  if (rc_is_abort(r) or r._val == RC_FALSE) \
    __abort_txn(r); \
}
// combines the try...catch block with ALWAYS_ASSERT and allows abort.
// The rc_is_abort case is there because sometimes we want to make
// sure say, a get, succeeds, but the read itsef could also cause
// abort (by SSN). Use try_verify_strict if you need rc=true.
#define try_verify_relax(oper) \
{ \
  rc_t r = oper;   \
  ALWAYS_ASSERT(r._val == RC_TRUE or rc_is_abort(r)); \
  if (rc_is_abort(r))  \
    __abort_txn(r);  \
}

// No abort is allowed, usually for loading
#define try_verify_strict(oper) \
{ \
  rc_t rc = oper;   \
  ALWAYS_ASSERT(rc._val == RC_TRUE);    \
}

#endif /* _NDB_BENCH_H_ */
