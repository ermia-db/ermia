#pragma once

#include "txn.h"
#include <map>
#include "../dbcore/sm-log-recover-impl.h"

namespace ermia {

class Engine {
private:
  void CreateTable(uint16_t index_type, const char *name,
                   const char *primary_name);

public:
  Engine();
  ~Engine() {}

  // All supported index types
  static const uint16_t kIndexConcurrentMasstree = 0x1;

  inline void CreateMasstreeTable(const char *name, bool decoupled,
                                  const char *primary_name = nullptr) {
    CreateTable(kIndexConcurrentMasstree, name, primary_name);
  }

  inline transaction *NewTransaction(uint64_t txn_flags, str_arena &arena,
                                     transaction *buf) {
    new (buf) transaction(txn_flags, arena);
    return buf;
  }

  inline rc_t Commit(transaction *t) {
    rc_t rc = t->commit();
    if (!rc.IsAbort()) {
      t->~transaction();
    }
    return rc;
  }

  inline void Abort(transaction *t) {
    t->Abort();
    t->~transaction();
  }
};

// Base class for user-facing index implementations
class OrderedIndex {
  friend class transaction;

protected:
  IndexDescriptor *descriptor_;

public:
  OrderedIndex(std::string name, const char *primary = nullptr) {
    descriptor_ = IndexDescriptor::New(this, name, primary);
  }
  inline IndexDescriptor *GetDescriptor() { return descriptor_; }
  virtual void *GetTable() = 0;

  class ScanCallback {
  public:
    ~ScanCallback() {}
    virtual bool Invoke(const char *keyp, size_t keylen,
                        const varstr &value) = 0;
  };

  /**
   * Get a key of length keylen. The underlying DB does not manage
   * the memory associated with key. [rc] stores TRUE if found, FALSE otherwise.
   */
  virtual void Get(transaction *t, rc_t &rc, const varstr &key, varstr &value,
                   OID *out_oid = nullptr) = 0;

  /**
   * Put a key of length keylen, with mapping of length valuelen.
   * The underlying DB does not manage the memory pointed to by key or value
   * (a copy is made).
   *
   * If a record with key k exists, overwrites. Otherwise, inserts.
   */
  virtual rc_t Put(transaction *t, const varstr &key, varstr &value) = 0;

  /**
   * Insert a key of length keylen.
   *
   * If a record with key k exists, behavior is unspecified- this function
   * is only to be used when you can guarantee no such key exists (ie in loading
   *phase)
   *
   * Default implementation calls put(). See put() for meaning of return value.
   */
  virtual rc_t Insert(transaction *t, const varstr &key, varstr &value,
                      OID *out_oid = nullptr) = 0;

  /**
   * Insert into a secondary index. Maps key to OID.
   */
  virtual rc_t Insert(transaction *t, const varstr &key, OID oid) = 0;

  /**
   * Search [start_key, *end_key) if end_key is not null, otherwise
   * search [start_key, +infty)
   */
  virtual rc_t Scan(transaction *t, const varstr &start_key,
                    const varstr *end_key, ScanCallback &callback,
                    str_arena *arena) = 0;
  /**
   * Search (*end_key, start_key] if end_key is not null, otherwise
   * search (-infty, start_key] (starting at start_key and traversing
   * backwards)
   */
  virtual rc_t ReverseScan(transaction *t, const varstr &start_key,
                           const varstr *end_key, ScanCallback &callback,
                           str_arena *arena) = 0;

  /**
   * Default implementation calls put() with NULL (zero-length) value
   */
  virtual rc_t Remove(transaction *t, const varstr &key) = 0;

  virtual size_t Size() = 0;
  virtual std::map<std::string, uint64_t> Clear() = 0;
  virtual void SetArrays() = 0;

  // Use transaction's TryInsertNewTuple to try insert a new tuple
  rc_t TryInsert(transaction &t, const varstr *k, varstr *v, bool upsert,
                 OID *inserted_oid);

  virtual void
  GetOID(const varstr &key, rc_t &rc, TXN::xid_context *xc, OID &out_oid,
         ConcurrentMasstree::versioned_node_t *out_sinfo = nullptr) = 0;

  /**
   * Insert key-oid pair to the underlying actual index structure.
   *
   * Returns false if the record already exists or there is potential phantom.
   */
  virtual bool InsertIfAbsent(transaction *t, const varstr &key, OID oid) = 0;
};

// User-facing concurrent Masstree
class ConcurrentMasstreeIndex : public OrderedIndex {
  friend class sm_log_recover_impl;
  friend class sm_chkpt_mgr;

private:
  ConcurrentMasstree masstree_;

  struct SearchRangeCallback {
    SearchRangeCallback(OrderedIndex::ScanCallback &upcall)
        : upcall(&upcall), return_code(rc_t{RC_FALSE}) {}
    ~SearchRangeCallback() {}

    inline bool Invoke(const ConcurrentMasstree::string_type &k,
                       const varstr &v) {
      return upcall->Invoke(k.data(), k.length(), v);
    }

    OrderedIndex::ScanCallback *upcall;
    rc_t return_code;
  };

  struct XctSearchRangeCallback
      : public ConcurrentMasstree::low_level_search_range_callback {
    XctSearchRangeCallback(transaction *t, SearchRangeCallback *caller_callback)
        : t(t), caller_callback(caller_callback) {}

    virtual void
    on_resp_node(const typename ConcurrentMasstree::node_opaque_t *n,
                 uint64_t version);
    virtual bool invoke(const ConcurrentMasstree *btr_ptr,
                        const typename ConcurrentMasstree::string_type &k,
                        dbtuple *v,
                        const typename ConcurrentMasstree::node_opaque_t *n,
                        uint64_t version);

  private:
    transaction *const t;
    SearchRangeCallback *const caller_callback;
  };

  struct PurgeTreeWalker : public ConcurrentMasstree::tree_walk_callback {
    virtual void
    on_node_begin(const typename ConcurrentMasstree::node_opaque_t *n);
    virtual void on_node_success();
    virtual void on_node_failure();

  private:
    std::vector<std::pair<typename ConcurrentMasstree::value_type, bool>>
        spec_values;
  };

  // expect_new indicates if we expect the record to not exist in the tree- is
  // just a hint that affects perf, not correctness. remove is put with
  // nullptr as value.
  rc_t DoTreePut(transaction &t, const varstr *k, varstr *v, bool expect_new,
                 bool upsert, OID *inserted_oid);

  static rc_t DoNodeRead(transaction *t,
                         const ConcurrentMasstree::node_opaque_t *node,
                         uint64_t version);

public:
  ConcurrentMasstreeIndex(std::string name, const char *primary)
      : OrderedIndex(name, primary) {}

  inline void *GetTable() override { return masstree_.get_table(); }

  virtual void Get(transaction *t, rc_t &rc, const varstr &key, varstr &value,
                   OID *out_oid = nullptr) override;

  inline rc_t Put(transaction *t, const varstr &key, varstr &value) override {
    return DoTreePut(*t, &key, &value, false, true, nullptr);
  }
  inline rc_t Insert(transaction *t, const varstr &key, varstr &value,
                     OID *out_oid = nullptr) override {
    return DoTreePut(*t, &key, &value, true, true, out_oid);
  }
  inline rc_t Insert(transaction *t, const varstr &key, OID oid) override {
    return DoTreePut(*t, &key, (varstr *)&oid, true, false, nullptr);
  }
  inline rc_t Remove(transaction *t, const varstr &key) override {
    return DoTreePut(*t, &key, nullptr, false, false, nullptr);
  }
  rc_t Scan(transaction *t, const varstr &start_key, const varstr *end_key,
            ScanCallback &callback, str_arena *arena) override;
  rc_t ReverseScan(transaction *t, const varstr &start_key,
                   const varstr *end_key, ScanCallback &callback,
                   str_arena *arena) override;

  inline size_t Size() override { return masstree_.size(); }
  std::map<std::string, uint64_t> Clear() override;
  inline void SetArrays() override { masstree_.set_arrays(descriptor_); }

  inline void
  GetOID(const varstr &key, rc_t &rc, TXN::xid_context *xc, OID &out_oid,
         ConcurrentMasstree::versioned_node_t *out_sinfo = nullptr) override {
    bool found = masstree_.search(key, out_oid, xc->begin_epoch, out_sinfo);
    volatile_write(rc._val, found ? RC_TRUE : RC_FALSE);
  }

private:
  bool InsertIfAbsent(transaction *t, const varstr &key, OID oid) override;
};
} // namespace ermia
