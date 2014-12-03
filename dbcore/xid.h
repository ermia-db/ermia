// -*- mode:c++ -*-
#pragma once

#include "sm-common.h"

namespace TXN {

enum txn_state { TXN_EMBRYO, TXN_ACTIVE, TXN_COMMITTING, TXN_CMMTD, TXN_ABRTD };

struct xid_context {
    XID owner;
    LSN begin;
    LSN end;
    LSN hi; // youngest predecessor (\eta)
    LSN lo; // oldest successor (\pi)
    txn_state state;
};

/* Request a new XID and an associated context. The former is globally
   unique and the latter is distinct from any other transaction whose
   lifetime overlaps with this one.
 */
XID xid_alloc();

/* Release an XID and its associated context. The XID will no longer
   be associated with any context after this call returns.
 */
void xid_free(XID x);

/* Return the context associated with the givne XID.

   throw illegal_argument if [xid] is not currently associated with a context. 
 */
xid_context *xid_get_context(XID x);

inline bool ssn_check_exclusion(xid_context *xc) {
    if (xc->lo != INVALID_LSN and xc->hi >= xc->lo) printf("ssn exclusion failure\n");
    return xc->lo != INVALID_LSN and xc->hi < xc->lo; // \eta < \pi
}
};  // end of namespace