# Modbus Tag Scheduling Design

## Problem

The current implementation holds all tags in a single flat vector that is scanned
on every cycle of the handler thread.  For 1000 tags each auto-reading every 1000ms,
the vast majority of tags are idle at any given moment, yet the handler still iterates
over all 1000 entries to check whether any of them have something to do.  This O(n)
scan on every wakeup is the dominant CPU cost identified in profiling.

## Goals

- Scan only tags that have work to do right now.
- Let the handler sleep precisely until the next scheduled operation, with no busy loop.
- Keep the implementation simple enough to reason about correctly.

## Design: Single Sorted Vector

### Structure

One vector holds all tags that have outstanding or upcoming operations, sorted in
ascending order by `op_time` (the time at which the tag's next operation is due).
Tags that are fully idle (auto-sync not yet due, no pending read or write) are not
in the vector at all.

Each tag carries a state:

- REQUEST -- the tag is waiting for its op_time to arrive so a request can be sent.
- RESPONSE -- a request has been sent and the tag is waiting for a network response.

Each tag has a boolean flag (or perhaps a single bit) that indicates whether the tag is in the active tag vector or not.

### Invariant

Because RESPONSE-state tags were sent when their op_time was now or in the past,
and REQUEST-state tags with future op_times have not yet fired, the sort order
naturally produces a vector of the form:

    [ RESPONSE ... | REQUEST(due) ... | REQUEST(future) ... ]

RESPONSE-state tags always appear at or near the front.  Tags transition from
REQUEST to RESPONSE in place -- no movement in the vector is required.

### Execution Model

1. The handler iterates from the front of the vector, sending requests for every
   REQUEST-state tag whose op_time is now or in the past, up to the
   max_requests_in_flight limit.  Each tag that gets a request sent is flipped to
   RESPONSE state in place.

2. Once all available slots are filled (or no more REQUEST-state tags are due),
   the handler blocks on the socket waiting for an incoming response.

3. When a response arrives, the handler scans the RESPONSE-state tags at the front
   of the vector (at most max_requests_in_flight entries, currently up to 16) for
   the tag whose stored sequence ID matches the response transaction ID.  The scan
   is O(16) and is not a performance concern.

4. The matched tag is processed, removed from the vector, and re-inserted at the
   correct sorted position for its next scheduled op_time (auto-sync) or dropped
   from the vector entirely (one-shot explicit read or write).

5. With a slot now free, the handler immediately checks whether the next entry in
   the vector is a REQUEST-state tag that is due.  If so, it sends another request
   without waiting.

6. If there are no due REQUEST-state tags and no RESPONSE-state tags, the handler
   computes its socket wait timeout as the time remaining until the op_time of the
   first REQUEST-state tag in the vector.  This is simply vector[0].op_time - now.

### Why op_time on RESPONSE Tags Is Not a Problem

By the time a response is received, the corresponding RESPONSE-state tag's op_time
is always in the past -- responses may take several milliseconds to return from
a device.  The op_time is not consulted for scheduling while the tag is in RESPONSE
state.  When computing the socket wait timeout, the handler only looks at the first
REQUEST-state tag (the first entry that is not in RESPONSE state), so the stale
op_time of RESPONSE tags is never mistakenly used as a scheduling deadline.

### Insertion Order for Equal op_times

When two tags share the same op_time, new tags are inserted after the existing ones
at that time.  This preserves FIFO fairness within a priority group.  In practice,
a binary search finds the insertion point in O(log n) comparisons, and for the
common case where the new tag goes at the end of the vector (e.g., all tags inserted
at startup with op_time = now) the insertion is effectively an append.

## Tag Lifecycle

### Auto-Sync Read

1. Tag is created with auto_sync_read_ms > 0.  It is inserted into the vector with
   op_time = now (first read fires immediately).

2. When op_time arrives, a read request is sent.  The tag transitions to RESPONSE
   state in place.

3. When the response is received (or times out), the tag computes its next op_time:

       next_op_time = auto_sync_next_read + auto_sync_read_ms

   If the computed time is already in the past (e.g., the device was slow or
   reconnecting took a long time), the interval is advanced by whole multiples of
   auto_sync_read_ms until the result is in the future.  This prevents the tag from
   permanently camping at the front of the vector and starving other tags.

4. The tag is removed from its current position and re-inserted at the correct
   sorted position for next_op_time.

### Auto-Sync Write

The value of auto_sync_write_ms is the delay after which a dirty tag will be writte.  A dirty 
tag is one that has had some of its local (to the PC not PLC) data changed by one of the generic
data accessor functions in libplctag.h

New vtable slot: int (*tag_data_written)(plc_tag_p tag) — called from the generic     
  library's data-write accessors (plc_tag_set_int8, etc.) only when                     
  tag->auto_sync_write_ms > 0. This check happens at the call site so protocols that    
  don't implement it pay zero cost.                                                     
                                                                                        
  mb_tag_data_written behavior (called while api_mutex is already held):                
                                                                                        
  Depending on the tag's current op state:                                              
  Current op: TAG_OP_IDLE                                                               
  Action: Remove from active_tags if present, insert at op_time = now +                 
    auto_sync_write_ms with op = WRITE_REQUEST                                          
  ────────────────────────────────────────                                              
  Current op: TAG_OP_READ_REQUEST                                                       
  Action: Request not yet sent: remove from active_tags, re-insert as WRITE_REQUEST at  
    now + auto_sync_write_ms                                                            
  ────────────────────────────────────────                                              
  Current op: TAG_OP_READ_RESPONSE                                                      
  Action: Request in-flight: decrement pending_request_count, clear                     
    pending_transaction_id (response becomes an orphan), remove from active_tags,       
    re-insert as WRITE_REQUEST at now + auto_sync_write_ms                              
  ────────────────────────────────────────                                              
  Current op: TAG_OP_WRITE_REQUEST                                                      
  Action: Write already queued but not sent yet — new data will naturally be picked up  
    when the request fires. No action needed.                                           
  ────────────────────────────────────────                                              
  Current op: TAG_OP_WRITE_RESPONSE                                                     
  Action: Write in-flight — can't change the wire. Leave it. reschedule_or_remove will  
    need to detect tag_is_dirty and re-schedule a write after the response              
    completes.                                                                          
  The TAG_OP_READ_RESPONSE case is the key one you called out. Clearing                 
  pending_transaction_id ensures that when tickle_all_tags processes the matched        
  response outside the mutex, its re-verification check (pending_transaction_id != 0)   
  fails and the response is discarded without overwriting the user's data.              
                                                                                        
  reschedule_or_remove also needs a small addition: after a write completes, if         
  auto_sync_write_ms > 0 && tag_is_dirty, schedule another write immediately rather than
   going idle.                                                                          
                                                                                        
  Should I set this aside and finish the auto-read refactor first, then come back to    
  implement this? The check_tag_auto_write path in tickle_tag can remain temporarily for
   correctness (auto-write-only tags just won't benefit from the optimization until     
  mb_tag_data_written is implemented).            

### Explicit plc_tag_read() / plc_tag_write()

The tag is inserted into the vector with op_time = now.  If the tag is already in
the vector (e.g., it is an auto-sync tag whose timer has already fired and it is
sitting in the REQUEST section), the existing entry is updated to op_time = now
rather than inserting a duplicate.  The tag needs to be removed from the vector and reinserted at the correct sorted location in this case.

### Abort

The tag is put into the IDLE state, any read_in_progress/read_complete type flags are reset to 0 and the tag is removed from the vector.  It should be removed from the vector first as that requires mutex operations and other things can touch the tag in an incoherent state otherwise.

### plc_tag_destroy()

The tag is removed from the vector by linear search on the tag pointer.  This is
a rare operation and the linear scan cost is acceptable.  If the tag is in RESPONSE
state, any subsequent response for its sequence ID is discarded (the sequence ID
scan at the front of the vector will find no match and the response is dropped).

### Connection Loss

When the connection to the device is lost, all RESPONSE-state tags at the front of
the vector are reset to REQUEST state.  Their op_time values are already in the past,
so they naturally remain at the front of the vector and will fire as soon as the
connection is re-established.  No explicit op_time update is required, but the reset
should be documented clearly in the code so the behavior is not mistaken for a bug.

## Data Structure Justification

A sorted vector satisfies all requirements:

- First entry access is O(1) -- finding the next due tag or the socket wait timeout
  is trivially cheap.
- Sorted insertion is O(log n) to find the position plus an O(n) element shift.
  In steady state the vector is nearly empty (most tags are idle between auto-sync
  cycles), so n is small and the shift cost is negligible.
- Removal by pointer is O(n) linear search, which is acceptable because it only
  occurs on tag abort and destroy, both of which are infrequent.

A min-heap would give O(log n) insert and remove-min at the cost of losing the FIFO
guarantee for equal op_times.  An intrusive doubly-linked list would give O(1)
remove-by-pointer at the cost of O(n) sorted insertion.  Neither offers a meaningful
advantage over a sorted vector given the small active set sizes expected in practice.
