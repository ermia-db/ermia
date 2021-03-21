**TPC-E Keys: Data sets that contains 64-bit integer keys for TPC-E tables:**
- Company (7500 keys)
- Customers (15000 keys)
- Address (22504 keys)
- NewsItem (15000 keys)
- CustomerAccount (75000 keys)
- Trade (259200000 keys)
- Settlement (259200000 keys)
- CashTransaction (238467837 keys)
- Broker (150 keys)

Obtained by loading the database with 15000 customers, 300 ITD (initial trading days). 
This gives ((ITD * 8 * 3600) / SF) * customer = 259200000 keys in Trade and Settlement tables. See also [TPC-E spec](http://www.tpc.org/tpc_documents_current_versions/pdf/tpc-e_v1.14.0.pdf) (page 70) for detailed records in each table.

Keys in each dataset file is laid out one after another in binary format. You may use it by opening the file (e.g., using `open`) and reading from it (e.g., using `read`).

To generate the datasets by yourself:

````
# 1. Build ERMIA with EXPORT_TPCE_INT64_KEYS:
$ cmake -DCMAKE_BUILD_TYPE=Release -DEXPORT_TPCE_INT64_KEYS=1 ..
$ makr -jN

# 2. Run it:
./run.sh ./ermia_SI tpce_org 500 20 10 "" "--working-days 300 --customers=15000"

# 3. The datasets will be available in tpce_*_keys files. 
````

