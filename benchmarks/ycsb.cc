/*
 * A YCSB implementation based off of Silo's and equivalent to FOEDUS's.
 */
#include <iostream>
#include <sstream>
#include <vector>
#include <utility>
#include <string>
#include <set>

#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <numa.h>

#include "bench.h"
#include "ycsb.h"
#include "../third-party/foedus/zipfian_random.hpp"

uint64_t global_key_counter = 0;
uint g_reps_per_tx = 1;
uint g_rmw_additional_reads = 0;
char g_workload = 'F';
uint g_initial_table_size = 3000000;
int g_zipfian_rng = 0;
double g_zipfian_theta = 0.99;  // zipfian constant, [0, 1), more skewed as it approaches 1.
int g_distinct_keys = 0;

// { insert, read, update, scan, rmw }
YcsbWorkload YcsbWorkloadA('A', 0, 50U, 100U, 0, 0);  // Workload A - 50% read, 50% update
YcsbWorkload YcsbWorkloadB('B', 0, 95U, 100U, 0, 0);  // Workload B - 95% read, 5% update
YcsbWorkload YcsbWorkloadC('C', 0, 100U, 0, 0, 0);  // Workload C - 100% read
YcsbWorkload YcsbWorkloadD('D', 5U, 100U, 0, 0, 0);  // Workload D - 95% read, 5% insert
YcsbWorkload YcsbWorkloadE('E', 5U, 0, 0, 100U, 0);  // Workload E - 5% insert, 95% scan

// Combine reps_per_tx and rmw_additional_reads to have "10R+10RMW" style
// transactions.
YcsbWorkload YcsbWorkloadF('F', 0, 0, 0, 0, 100U);  // Workload F - 100% RMW

// Extra workloads (not in spec)
YcsbWorkload YcsbWorkloadG('G', 0, 0, 5U, 100U, 0);  // Workload G - 5% update, 95% scan
YcsbWorkload YcsbWorkloadH('H', 0, 0, 0, 100U, 0);  // Workload H - 100% scan

YcsbWorkload ycsb_workload = YcsbWorkloadF;

void BuildKey(uint64_t key, ermia::varstr &k) {
  *(uint64_t*)k.p = key;
}

class ycsb_worker : public bench_worker {
 public:
  ycsb_worker(unsigned int worker_id, unsigned long seed, ermia::Engine *db,
              const std::map<std::string, ermia::OrderedIndex *> &open_tables,
              spin_barrier *barrier_a, spin_barrier *barrier_b)
      : bench_worker(worker_id, true, seed, db, open_tables, barrier_a, barrier_b),
        tbl((ermia::ConcurrentMasstreeIndex*)open_tables.at("USERTABLE")),
        uniform_rng(1237 + worker_id) {
    if (g_zipfian_rng) {
      zipfian_rng.init(g_initial_table_size, g_zipfian_theta, 1237 + worker_id);
    }
  }

  virtual cmdlog_redo_workload_desc_vec get_cmdlog_redo_workload() const {
    LOG(FATAL) << "Not applicable";
  }
  virtual workload_desc_vec get_workload() const {
    workload_desc_vec w;
    if (ycsb_workload.insert_percent())
      w.push_back(workload_desc(
          "Insert", double(ycsb_workload.insert_percent()) / 100.0, TxnInsert));
    if (ycsb_workload.read_percent()) {
      w.push_back(workload_desc(
          "Read", double(ycsb_workload.read_percent()) / 100.0, TxnRead));
    }
    if (ycsb_workload.update_percent())
      w.push_back(workload_desc(
          "Update", double(ycsb_workload.update_percent()) / 100.0, TxnUpdate));
    if (ycsb_workload.scan_percent())
      w.push_back(workload_desc("Scan", double(ycsb_workload.scan_percent()) / 100.0,
                                TxnScan));
    if (ycsb_workload.rmw_percent())
      w.push_back(
          workload_desc("RMW", double(ycsb_workload.rmw_percent()) / 100.0, TxnRMW));
    return w;
  }

  static rc_t TxnInsert(bench_worker *w) { MARK_REFERENCED(w); return {RC_TRUE}; }

  static rc_t TxnRead(bench_worker *w) {
    return static_cast<ycsb_worker *>(w)->txn_read();
  }

  static rc_t TxnUpdate(bench_worker *w) { MARK_REFERENCED(w); return {RC_TRUE}; }

  static rc_t TxnScan(bench_worker *w) { MARK_REFERENCED(w); return {RC_TRUE}; }

  static rc_t TxnRMW(bench_worker *w) {
    return static_cast<ycsb_worker *>(w)->txn_rmw();
  }

  struct KeyCompare : public std::unary_function<ermia::varstr, bool>{
    explicit KeyCompare(ermia::varstr &baseline) : baseline(baseline) {}
    bool operator() (const ermia::varstr &arg) {
      return *(uint64_t*)arg.p == *(uint64_t*)baseline.p;
    }
    ermia::varstr &baseline;
  };

  rc_t txn_read() {
    arena.reset();
    ermia::transaction *txn = nullptr;
    txn = db->NewTransaction(ermia::transaction::TXN_FLAG_READ_ONLY, arena, txn_buf());

    for (uint i = 0; i < g_reps_per_tx; ++i) {
      auto &k = GenerateKey();
      ermia::varstr &v = str(sizeof(YcsbRecord));
      // TODO(tzwang): add read/write_all_fields knobs
      rc_t rc = rc_t{RC_INVALID};
      tbl->Get(txn, rc, k, v);  // Read

#if defined(SSI) || defined(SSN) || defined(MVOCC)
      TryCatch(rc);  // Might abort if we use SSI/SSN/MVOCC
#else
      // Under SI this must succeed
      ALWAYS_ASSERT(rc._val == RC_TRUE);
      ASSERT(*(char*)v.data() == 'a');
#endif
      memcpy((char*)(&v) + sizeof(ermia::varstr), (char *)v.data(), sizeof(YcsbRecord));
    }
    TryCatch(db->Commit(txn));
    return {RC_TRUE};
  }

  rc_t txn_rmw() {
    ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());
    arena.reset();
    for (uint i = 0; i < g_reps_per_tx; ++i) {
      ermia::varstr &k = GenerateKey();
      ermia::varstr &v = str(sizeof(YcsbRecord));
      // TODO(tzwang): add read/write_all_fields knobs
      rc_t rc = rc_t{RC_INVALID};
      ermia::OID oid = 0;
      tbl->Get(txn, rc, k, v, &oid);  // Read
#if defined(SSI) || defined(SSN) || defined(MVOCC)
      TryCatch(rc);  // Might abort if we use SSI/SSN/MVOCC
#else
      // Under SI this must succeed
      LOG_IF(FATAL, rc._val != RC_TRUE);
      ASSERT(rc._val == RC_TRUE);
      ASSERT(*(char*)v.data() == 'a');
#endif
      memcpy((char*)(&v) + sizeof(ermia::varstr), (char *)v.data(), sizeof(YcsbRecord));

      // Re-initialize the value structure to use my own allocated memory -
      // DoTupleRead will change v.p to the object's data area to avoid memory
      // copy (in the read op we just did).
      new (&v) ermia::varstr((char *)&v + sizeof(ermia::varstr), sizeof(YcsbRecord));
      memset(v.data(), 'a', sizeof(YcsbRecord));
      TryCatch(tbl->Put(txn, k, v));  // Modify-write
    }

    for (uint i = 0; i < g_rmw_additional_reads; ++i) {
      ermia::varstr &k = GenerateKey();
      ermia::varstr &v = str(sizeof(YcsbRecord));
      // TODO(tzwang): add read/write_all_fields knobs
      rc_t rc = rc_t{RC_INVALID};
      tbl->Get(txn, rc, k, v);  // Read
#if defined(SSI) || defined(SSN) || defined(MVOCC)
      TryCatch(rc);  // Might abort if we use SSI/SSN/MVOCC
#else
      // Under SI this must succeed
      ALWAYS_ASSERT(rc._val == RC_TRUE);
      ASSERT(*(char*)v.data() == 'a');
#endif
      memcpy((char*)(&v) + sizeof(ermia::varstr), (char *)v.data(), sizeof(YcsbRecord));

    }
    TryCatch(db->Commit(txn));
    return {RC_TRUE};
  }

 protected:
  ALWAYS_INLINE ermia::varstr &str(uint64_t size) { return *arena.next(size); }
  ermia::varstr &GenerateKey() {
    uint64_t r = 0;
    if (g_zipfian_rng) {
      r = zipfian_rng.next();
    } else {
      r = uniform_rng.uniform_within(0, g_initial_table_size - 1);
    }

    ermia::varstr &k = str(sizeof(uint64_t));  // 8-byte key
    new (&k) ermia::varstr((char *)&k + sizeof(ermia::varstr), sizeof(uint64_t));
    ::BuildKey(r, k);
    return k;
  }

 private:
  ermia::ConcurrentMasstreeIndex *tbl;
  foedus::assorted::UniformRandom uniform_rng;
  foedus::assorted::ZipfianRandom zipfian_rng;
  std::vector<ermia::varstr *> keys;
  std::vector<ermia::varstr *> values;
};

class ycsb_usertable_loader : public bench_loader {
 public:
  ycsb_usertable_loader(unsigned long seed, ermia::Engine *db,
                        const std::map<std::string, ermia::OrderedIndex *> &open_tables,
                        uint32_t loader_id)
      : bench_loader(seed, db, open_tables, loader_id), loader_id(loader_id) {}

 private:
  uint32_t loader_id;

 protected:
  void load() {
    ermia::OrderedIndex *tbl = open_tables.at("USERTABLE");
    int64_t to_insert = g_initial_table_size / ermia::config::worker_threads;
    uint64_t start_key = loader_id * to_insert;;
    for (uint64_t i = 0; i < to_insert; ++i) {
      arena.reset();
      ermia::varstr &k = str(sizeof(uint64_t));
      BuildKey(start_key + i, k);

      ermia::varstr &v = str(sizeof(YcsbRecord));
      new (&v) ermia::varstr((char *)&v + sizeof(ermia::varstr), sizeof(YcsbRecord));
      *(char*)v.p = 'a';

      ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());
      TryVerifyStrict(tbl->Insert(txn, k, v));
      TryVerifyStrict(db->Commit(txn));
    }

    // Verify inserted values
    for (uint64_t i = 0; i < to_insert; ++i) {
      ermia::transaction *txn = db->NewTransaction(0, arena, txn_buf());
      arena.reset();
      rc_t rc = rc_t{RC_INVALID};
      ermia::OID oid = 0;
      ermia::varstr &k = str(sizeof(uint64_t));
      BuildKey(start_key + i, k);
      ermia::varstr &v = str(0);
      BuildKey(start_key + i, k);
      tbl->Get(txn, rc, k, v, &oid);
      ALWAYS_ASSERT(*(char*)v.data() == 'a');
      TryVerifyStrict(rc);
      TryVerifyStrict(db->Commit(txn));
    }

    if (ermia::config::verbose) {
      std::cerr << "[INFO] loader " << loader_id <<  " loaded "
                << to_insert << " kyes in USERTABLE" << std::endl;
    }
  }
};

class ycsb_bench_runner : public bench_runner {
 public:
  ycsb_bench_runner(ermia::Engine *db) : bench_runner(db) {
    db->CreateMasstreeTable("USERTABLE", false);
  }

  virtual void prepare(char *) {
    open_tables["USERTABLE"] = ermia::IndexDescriptor::GetIndex("USERTABLE");
  }

 protected:
  virtual std::vector<bench_loader *> make_loaders() {
    uint64_t requested = g_initial_table_size;
    uint64_t records_per_thread = std::max<uint64_t>(1, g_initial_table_size / ermia::config::worker_threads);
    g_initial_table_size = records_per_thread * ermia::config::worker_threads;

    if (ermia::config::verbose) {
      std::cerr << "[INFO] requested for " << requested
           << " records, will load "
           << records_per_thread *ermia::config::worker_threads << std::endl;
    }

    std::vector<bench_loader *> ret;
    for (uint32_t i = 0; i < ermia::config::worker_threads; ++i) {
      ret.push_back(new ycsb_usertable_loader(0, db, open_tables, i));
    }
    return ret;
  }

  virtual std::vector<bench_worker *> make_cmdlog_redoers() {
    // Not implemented
    std::vector<bench_worker *> ret;
    return ret;
  }
  virtual std::vector<bench_worker *> make_workers() {
    util::fast_random r(8544290);
    std::vector<bench_worker *> ret;
    for (size_t i = 0; i < ermia::config::worker_threads; i++) {
      ret.push_back(new ycsb_worker(i, r.next(), db, open_tables, &barrier_a,
                                    &barrier_b));
    }
    return ret;
  }
};

void ycsb_do_test(ermia::Engine *db, int argc, char **argv) {
  // parse options
  optind = 1;
  while (1) {
    static struct option long_options[] = {
        {"reps-per-tx", required_argument, 0, 'r'},
        {"rmw-additional-reads", required_argument, 0, 'a'},
        {"workload", required_argument, 0, 'w'},
        {"initial-table-size", required_argument, 0, 's'},
        {"zipfian", no_argument, &g_zipfian_rng, 1},
        {"zipfian-theta", required_argument, 0, 'z'},
        {"distinct-keys", no_argument, &g_distinct_keys, 1},
        {0, 0, 0, 0}};

    int option_index = 0;
    int c = getopt_long(argc, argv, "r:a:w:s:", long_options, &option_index);
    if (c == -1) break;
    switch (c) {
      case 0:
        if (long_options[option_index].flag != 0) break;
        abort();
        break;

      case 'z':
        g_zipfian_theta = strtod(optarg, NULL);
        break;

      case 'r':
        g_reps_per_tx = strtoul(optarg, NULL, 10);
        break;

      case 'a':
        g_rmw_additional_reads = strtoul(optarg, NULL, 10);
        break;

      case 's':
        g_initial_table_size = strtoul(optarg, NULL, 10);
        break;

      case 'w':
        g_workload = optarg[0];
        if (g_workload == 'A')
          ycsb_workload = YcsbWorkloadA;
        else if (g_workload == 'B')
          ycsb_workload = YcsbWorkloadB;
        else if (g_workload == 'C')
          ycsb_workload = YcsbWorkloadC;
        else if (g_workload == 'D')
          ycsb_workload = YcsbWorkloadD;
        else if (g_workload == 'E')
          ycsb_workload = YcsbWorkloadE;
        else if (g_workload == 'F')
          ycsb_workload = YcsbWorkloadF;
        else if (g_workload == 'G')
          ycsb_workload = YcsbWorkloadG;
        else if (g_workload == 'H')
          ycsb_workload = YcsbWorkloadH;
        else {
          std::cerr << "Wrong workload type: " << g_workload << std::endl;
          abort();
        }
        break;

      case '?':
        /* getopt_long already printed an error message. */
        exit(1);

      default:
        abort();
    }
  }

  ALWAYS_ASSERT(g_initial_table_size);

  if (ermia::config::verbose) {
    std::cerr << "ycsb settings:" << std::endl
         << "  workload:                   " << g_workload << std::endl
         << "  initial user table size:    " << g_initial_table_size << std::endl
         << "  operations per transaction: " << g_reps_per_tx << std::endl
         << "  additional reads after RMW: " << g_rmw_additional_reads << std::endl
         << "  distinct keys:              " << g_distinct_keys << std::endl
         << "  distribution:               " << (g_zipfian_rng ? "zipfian" : "uniform") << std::endl;

    if (g_zipfian_rng) {
      std::cerr << "  zipfian theta:              " << g_zipfian_theta << std::endl;
    }
  }

  ycsb_bench_runner r(db);
  r.run();
}
