#!/usr/bin/env python3
"""
Enhanced analysis of libplctag debug logs with proper tag ID back-patching.

Tag IDs are always 0 when plc_tag_create_ex() starts (CREATE_START).
When creation completes, the actual tag ID is revealed.
We correlate CREATE_START and CREATED events by thread ID and back-patch
the CREATE_START event to show the final tag ID.
"""

import re
import sys
from datetime import datetime
from collections import defaultdict

# Get input file from command line argument
if len(sys.argv) < 2:
    print("Usage: python3 analyze_logs_enhanced.py <debug_log_file>", file=sys.stderr)
    sys.exit(1)

debug_log_file = sys.argv[1]

# Verify the file exists
import os
if not os.path.exists(debug_log_file):
    print(f"Error: File not found: {debug_log_file}", file=sys.stderr)
    sys.exit(1)

def parse_timestamp(ts_str):
    """Parse timestamp string to datetime object."""
    try:
        return datetime.strptime(ts_str, "%Y-%m-%d %H:%M:%S.%f")
    except:
        return None

def extract_thread(line):
    """Extract thread number from log line."""
    match = re.search(r'thread\((\d+)\)', line)
    return int(match.group(1)) if match else None

def extract_tag_id(line):
    """Extract tag ID from log line."""
    match = re.search(r'tag\((\d+)\)', line)
    return int(match.group(1)) if match else None

def extract_timestamp(line):
    """Extract timestamp from log line."""
    match = re.search(r'^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)', line)
    return match.group(1) if match else None

# Dictionary to map thread ID -> list of tag creation events (CREATE_START)
thread_pending_creates = defaultdict(list)

# Dictionary to store final tag creation info for back-patching
# Key: (thread_id, original_timestamp), Value: final_tag_id
tag_id_mapping = {}

print("=" * 80)
print("ENHANCED TAG CREATION ANALYSIS WITH ID BACK-PATCHING")
print("=" * 80)
print()

# First pass: identify CREATE_START events and CREATED events
print("PASS 1: Collecting tag creation events...")
print()

create_start_events = []  # (timestamp_str, thread_id, timestamp_obj)
create_complete_events = []  # (timestamp_str, thread_id, final_tag_id, timestamp_obj)

with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        if ':850 Starting' in line and 'plc_tag_create_ex' in line:
            ts_str = extract_timestamp(line)
            thread = extract_thread(line)
            ts_obj = parse_timestamp(ts_str)
            if ts_str and thread is not None and ts_obj:
                create_start_events.append((ts_str, thread, ts_obj))
                print(f"CREATE_START: Thread {thread} at {ts_str}")
        
        elif (':1074 Done' in line or ':1068 tag set up elapsed' in line) and 'plc_tag_create_ex' in line:
            ts_str = extract_timestamp(line)
            thread = extract_thread(line)
            tag_id = extract_tag_id(line)
            ts_obj = parse_timestamp(ts_str)
            if ts_str and thread is not None and tag_id is not None and ts_obj:
                create_complete_events.append((ts_str, thread, tag_id, ts_obj))
                line_marker = "line 1074" if "1074" in line else "line 1068"
                print(f"  -> CREATE_DONE: Thread {thread} -> Tag {tag_id} at {ts_str} ({line_marker})")

print()
print(f"Total CREATE_START events found: {len(create_start_events)}")
print(f"Total CREATE_DONE events found: {len(create_complete_events)}")
print()

# Match CREATE_START with CREATE_DONE by thread
print("=" * 80)
print("PASS 2: Correlating CREATE_START and CREATE_DONE by thread...")
print("=" * 80)
print()

matched_creates = []  # (thread_id, start_ts, done_ts, final_tag_id, latency_ms)

for start_ts_str, start_thread, start_ts_obj in create_start_events:
    # Find the corresponding CREATED event on the same thread
    matching_done = None
    for done_ts_str, done_thread, tag_id, done_ts_obj in create_complete_events:
        if done_thread == start_thread and done_ts_obj > start_ts_obj:
            # This is the first CREATED event on this thread after the CREATE_START
            if matching_done is None or done_ts_obj < matching_done[3]:
                matching_done = (done_ts_str, done_thread, tag_id, done_ts_obj)
    
    if matching_done:
        done_ts_str, done_thread, tag_id, done_ts_obj = matching_done
        latency_ms = (done_ts_obj - start_ts_obj).total_seconds() * 1000
        matched_creates.append((start_thread, start_ts_str, done_ts_str, tag_id, latency_ms))
        print(f"Thread {start_thread}: TAG 0 -> TAG {tag_id}")
        print(f"  START:  {start_ts_str}")
        print(f"  DONE:   {done_ts_str}")
        print(f"  LATENCY: {latency_ms:.3f} ms")
        print()

# Build a map of created tag info (will include reconstructed attributes)
created_info = {}  # tag_id -> {start_ts, done_ts, create_thread, attrs (list)}

# For each matched create, scan the debug log between start and done times and collect
# attr_create_from_str:137 key-value pairs (these are the raw key=value pairs)
# To avoid collecting overlapping attributes from other tags, we look for a pattern:
# - Start collecting from the "Starting." line
# - Stop collecting when we hit the line just before "Done."
attr_re = re.compile(r'attr_create_from_str:\d+\s+Key-value pair\s+"([^"]+)"')
for start_thread, start_ts_str, done_ts_str, tag_id, latency_ms in matched_creates:
    start_obj = parse_timestamp(start_ts_str)
    done_obj = parse_timestamp(done_ts_str)
    
    # Collect all attr lines during the window, then deduplicate by keeping unique values
    # This handles the case where multiple tags' attr logs overlap in time
    all_kvs = []
    with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
        in_window = False
        for line in f:
            ts = extract_timestamp(line)
            if not ts:
                continue
            ts_obj = parse_timestamp(ts)
            
            # Start collecting when we see the first attr line in the window
            if ts_obj and start_obj <= ts_obj < done_obj:
                if 'attr_create_from_str:108 Starting' in line:
                    in_window = True
                    all_kvs = []  # Reset for this tag
            
            # Collect attributes while in window
            if ts_obj and start_obj <= ts_obj < done_obj and in_window:
                if 'attr_create_from_str' in line and 'tag(0)' in line:
                    m = attr_re.search(line)
                    if m:
                        all_kvs.append(m.group(1))
            
            # Stop collecting when we hit Done
            if ts_obj and ts_obj >= done_obj:
                in_window = False
    
    created_info[tag_id] = {
        'start_ts': start_ts_str,
        'done_ts': done_ts_str,
        'create_thread': start_thread,
        'attrs': all_kvs,
        'latency_ms': latency_ms
    }

print()
print("=" * 80)
print("PASS 3: Building complete event timeline...")
print("=" * 80)
print()

all_events = []

# Add CREATE_START events (with back-patched tag IDs)
for start_thread, start_ts_str, done_ts_str, tag_id, latency_ms in matched_creates:
    all_events.append((
        parse_timestamp(start_ts_str),
        start_thread,
        f"TAG {tag_id} CREATE_START (plc_tag_create_ex called) - will complete in {latency_ms:.1f}ms"
    ))

# Add CREATE_DONE events
for start_thread, start_ts_str, done_ts_str, tag_id, latency_ms in matched_creates:
    all_events.append((
        parse_timestamp(done_ts_str),
        start_thread,
        f"TAG {tag_id} CREATED (line 1074/1068) - latency: {latency_ms:.1f}ms"
    ))

# Add tag destruction events
print("Collecting tag destruction events...")
with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        if ':1534 Done' in line and 'plc_tag_destroy' in line:
            ts_str = extract_timestamp(line)
            thread = extract_thread(line)
            tag_id = extract_tag_id(line)
            ts_obj = parse_timestamp(ts_str)
            if ts_str and thread is not None and tag_id is not None and ts_obj:
                all_events.append((ts_obj, thread, f"TAG {tag_id} DESTROYED"))

# Add connection events
print("Collecting connection events...")
with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        if ':689 PLC_CONNECT_START' in line or \
           ':722 PLC_READY' in line or \
           ':1948' in line and 'socket_destroy' in line:
            ts_str = extract_timestamp(line)
            thread = extract_thread(line)
            ts_obj = parse_timestamp(ts_str)
            if ts_str and thread is not None and ts_obj:
                if 'PLC_CONNECT_START' in line:
                    all_events.append((ts_obj, thread, "CONNECTION STARTED"))
                elif 'PLC_READY' in line:
                    all_events.append((ts_obj, thread, "CONNECTION READY"))
                elif 'socket_destroy' in line:
                    all_events.append((ts_obj, thread, "SOCKET CLOSED"))

# Sort by timestamp, then by thread for stable ordering
all_events.sort(key=lambda x: (x[0], x[1]))

print()
print("=" * 80)
print("COMPLETE TIMELINE SUMMARY (all major events)")
print("=" * 80)
print()

for ts_obj, thread, event_desc in all_events:
    ts_str = ts_obj.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    print(f"{ts_str} | Thread {thread:2d} | {event_desc}")

print()
print("=" * 80)
print("TIMELINE STATISTICS")
print("=" * 80)
print()

# Count events by type
create_starts = sum(1 for _, _, desc in all_events if 'CREATE_START' in desc)
create_dones = sum(1 for _, _, desc in all_events if 'CREATED' in desc)
destroyed = sum(1 for _, _, desc in all_events if 'DESTROYED' in desc)
conn_started = sum(1 for _, _, desc in all_events if 'CONNECTION STARTED' in desc)
conn_ready = sum(1 for _, _, desc in all_events if 'CONNECTION READY' in desc)
sock_closed = sum(1 for _, _, desc in all_events if 'SOCKET CLOSED' in desc)

print(f"Tag Creation Events:")
print(f"  CREATE_START:  {create_starts}")
print(f"  CREATE_DONE:   {create_dones}")
print(f"  DESTROYED:     {destroyed}")
print()
print(f"Connection Events:")
print(f"  CONNECTION STARTED: {conn_started}")
print(f"  CONNECTION READY:   {conn_ready}")
print(f"  SOCKET CLOSED:      {sock_closed}")
print()

if all_events:
    min_ts = min(e[0] for e in all_events)
    max_ts = max(e[0] for e in all_events)
    duration = (max_ts - min_ts).total_seconds()
    print(f"Timeline Duration: {duration:.3f} seconds ({duration/60:.1f} minutes)")
    print(f"Start: {min_ts.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")
    print(f"End:   {max_ts.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]}")

print()
print("=" * 80)
print("PER-TAG THREAD ANALYSIS")
print("=" * 80)
print()

# First, identify the global tickler thread (the thread that only logs tag_tickler_func messages)
print("PASS 4: Identifying global tickler thread...")
tickler_thread = None
thread_line_patterns = defaultdict(lambda: set())

# Scan debug.txt to find which thread only logs tickler messages
with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        thread = extract_thread(line)
        if thread is not None:
            # Extract the line number/function info to categorize thread activity
            if 'tag_tickler_func:' in line:
                line_marker = 'tickler'
            else:
                # Find function:line pattern
                match = re.search(r':(\d+)\s', line)
                line_marker = match.group(1) if match else 'other'
            
            thread_line_patterns[thread].add(line_marker)

# The tickler thread is one that ONLY has tickler markers (just wakes tags, doesn't process)
for thread, patterns in thread_line_patterns.items():
    if patterns == {'tickler'}:
        tickler_thread = thread
        print(f"  Identified Thread {thread} as global tickler thread (only logs tag_tickler_func)")
        break

if tickler_thread is None:
    print(f"  No dedicated tickler thread found (all threads have other activities)")

print()

# Build per-tag information: creation thread, destruction thread, request threads, reads, writes
tag_info = defaultdict(lambda: {
    'create_thread': None,
    'create_start_ts': None,
    'create_end_ts': None,
    'destroy_thread': None,
    'destroy_ts': None,
    'request_threads': set(),
    'read_start_ts': None,
    'read_start_thread': None,
    'read_end_ts': None,
    'read_end_thread': None,
    'read_count': 0,
    'write_start_ts': None,
    'write_start_thread': None,
    'write_end_ts': None,
    'write_end_thread': None,
    'write_count': 0,
    'plc_reused': None
})

# Collect creation and destruction info from matched_creates
for start_thread, start_ts_str, done_ts_str, tag_id, latency_ms in matched_creates:
    tag_info[tag_id]['create_thread'] = start_thread
    tag_info[tag_id]['create_start_ts'] = start_ts_str
    tag_info[tag_id]['create_end_ts'] = done_ts_str

# Collect destruction info
with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        if ':1534 Done' in line and 'plc_tag_destroy' in line:
            thread = extract_thread(line)
            tag_id = extract_tag_id(line)
            ts = extract_timestamp(line)
            if thread is not None and tag_id is not None:
                tag_info[tag_id]['destroy_thread'] = thread
                tag_info[tag_id]['destroy_ts'] = ts

# Collect PLC connection creation/reuse info from debug.txt
# Map timestamps to whether connection was created or reused
plc_connection_map = {}  # timestamp -> (thread, is_reused)
with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        if 'find_or_create_plc:' in line and 'tag(0)' in line:
            ts = extract_timestamp(line)
            thread = extract_thread(line)
            if 'Creating new PLC connection' in line:
                plc_connection_map[ts] = (thread, False)  # False = new connection
            elif 'Using existing PLC connection' in line:
                plc_connection_map[ts] = (thread, True)   # True = reused

# Now match PLC connections to tags based on creation timestamps
# Each tag creation should have a corresponding PLC connection entry shortly before it
for start_thread, start_ts_str, done_ts_str, tag_id, latency_ms in matched_creates:
    start_obj = parse_timestamp(start_ts_str)
    # Find the closest preceding PLC connection entry
    closest_plc_ts = None
    closest_plc_dist = None
    for plc_ts_str in plc_connection_map.keys():
        plc_obj = parse_timestamp(plc_ts_str)
        if plc_obj <= start_obj:
            dist = (start_obj - plc_obj).total_seconds()
            if closest_plc_dist is None or dist < closest_plc_dist:
                closest_plc_dist = dist
                closest_plc_ts = plc_ts_str
    
    # If we found a matching PLC connection within 100ms, use it
    if closest_plc_ts and closest_plc_dist is not None and closest_plc_dist < 0.1:
        plc_thread, is_reused = plc_connection_map[closest_plc_ts]
        tag_info[tag_id]['plc_reused'] = is_reused

# Collect request threads and read/write info from debug.txt
# Build per-tag read/write information
tag_read_write_info = defaultdict(lambda: {
    'read_start_ts': None,
    'read_start_thread': None,
    'read_end_ts': None,
    'read_end_thread': None,
    'read_count': 0,
    'write_start_ts': None,
    'write_start_thread': None,
    'write_end_ts': None,
    'write_end_thread': None,
    'write_count': 0,
    'request_threads': set()
})

with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        thread = extract_thread(line)
        tag_id = extract_tag_id(line)
        ts = extract_timestamp(line)
        
        if tag_id is None or tag_id == 0:
            continue
        
        # Track read operations - look for plc_tag_read:1558 Starting
        if 'plc_tag_read:' in line and 'Starting' in line:
            if tag_read_write_info[tag_id]['read_start_ts'] is None:
                tag_read_write_info[tag_id]['read_start_ts'] = ts
                tag_read_write_info[tag_id]['read_start_thread'] = thread
            tag_read_write_info[tag_id]['read_end_ts'] = ts
            tag_read_write_info[tag_id]['read_end_thread'] = thread
            tag_read_write_info[tag_id]['read_count'] += 1
        
        # Track write operations - look for plc_tag_write:1749 Starting
        if 'plc_tag_write:' in line and 'Starting' in line:
            if tag_read_write_info[tag_id]['write_start_ts'] is None:
                tag_read_write_info[tag_id]['write_start_ts'] = ts
                tag_read_write_info[tag_id]['write_start_thread'] = thread
            tag_read_write_info[tag_id]['write_end_ts'] = ts
            tag_read_write_info[tag_id]['write_end_thread'] = thread
            tag_read_write_info[tag_id]['write_count'] += 1
        
        # Look for actual I/O operations that indicate this thread is handling the request
        if any(keyword in line for keyword in ['send_request:', 'socket_write:', 'receive_response:', 'socket_read:']):
            if thread is not None and thread != tickler_thread:
                tag_read_write_info[tag_id]['request_threads'].add(thread)

# Copy read/write info into tag_info
for tag_id in tag_read_write_info.keys():
    tag_info[tag_id]['read_start_ts'] = tag_read_write_info[tag_id]['read_start_ts']
    tag_info[tag_id]['read_start_thread'] = tag_read_write_info[tag_id]['read_start_thread']
    tag_info[tag_id]['read_end_ts'] = tag_read_write_info[tag_id]['read_end_ts']
    tag_info[tag_id]['read_end_thread'] = tag_read_write_info[tag_id]['read_end_thread']
    tag_info[tag_id]['read_count'] = tag_read_write_info[tag_id]['read_count']
    tag_info[tag_id]['write_start_ts'] = tag_read_write_info[tag_id]['write_start_ts']
    tag_info[tag_id]['write_start_thread'] = tag_read_write_info[tag_id]['write_start_thread']
    tag_info[tag_id]['write_end_ts'] = tag_read_write_info[tag_id]['write_end_ts']
    tag_info[tag_id]['write_end_thread'] = tag_read_write_info[tag_id]['write_end_thread']
    tag_info[tag_id]['write_count'] = tag_read_write_info[tag_id]['write_count']
    tag_info[tag_id]['request_threads'] = tag_read_write_info[tag_id]['request_threads']

# Sort tags and display per-tag info (include reconstructed attributes and timestamps)
for tag_id in sorted(tag_info.keys()):
    info = tag_info[tag_id]
    created = created_info.get(tag_id)
    
    # Skip if no relevant info
    if not created and info['destroy_thread'] is None and not info['read_start_ts'] and not info['write_start_ts']:
        continue
    
    print(f"TAG {tag_id}:")
    print()

    # 1. Creation started
    if created:
        print(f"  1. CREATION STARTED:")
        print(f"     Thread:  {created['create_thread']}")
        print(f"     Time:    {created['start_ts']}")
        
        # 1.1 Check if PLC connection was reused
        plc_reused_info = info.get('plc_reused')
        if plc_reused_info is not None:
            plc_status = "REUSED existing connection" if plc_reused_info else "CREATED new connection"
        else:
            plc_status = "(PLC connection status unknown)"
        print(f"     PLC:     {plc_status}")
        print()
        
        # 2. Creation finished
        print(f"  2. CREATION FINISHED:")
        print(f"     Thread:  {created['create_thread']}")
        print(f"     Time:    {created['done_ts']}")
        print(f"     Latency: {created['latency_ms']:.1f}ms")
        print()
        
        # Attributes
        if created['attrs']:
            reconstructed = '&'.join(created['attrs'])
            print(f"     Attributes: {reconstructed}")
        print()
    else:
        print(f"  (No creation info found)")
        print()

    # 3. Reads started
    if info['read_start_ts']:
        print(f"  3. READS STARTED:")
        print(f"     Thread:  {info['read_start_thread']}")
        print(f"     Time:    {info['read_start_ts']}")
        
        # 4. Reads ended
        print(f"  4. READS ENDED:")
        print(f"     Thread:  {info['read_end_thread']}")
        print(f"     Time:    {info['read_end_ts']}")
        
        # Calculate read period
        read_start_obj = parse_timestamp(info['read_start_ts'])
        read_end_obj = parse_timestamp(info['read_end_ts'])
        if read_start_obj and read_end_obj:
            read_duration_ms = (read_end_obj - read_start_obj).total_seconds() * 1000
            print(f"     Period:  {read_duration_ms:.0f}ms ({read_duration_ms/1000:.1f}s, {read_duration_ms/60000:.1f}m)")
        
        # Read count
        print(f"     Count:   {info['read_count']} reads")
        print()
    else:
        print(f"  3. READS STARTED: (no reads)")
        print(f"  4. READS ENDED:   (no reads)")
        print()

    # 5. Writes started
    if info['write_start_ts']:
        print(f"  5. WRITES STARTED:")
        print(f"     Thread:  {info['write_start_thread']}")
        print(f"     Time:    {info['write_start_ts']}")
        
        # 6. Writes ended
        print(f"  6. WRITES ENDED:")
        print(f"     Thread:  {info['write_end_thread']}")
        print(f"     Time:    {info['write_end_ts']}")
        
        # Calculate write period
        write_start_obj = parse_timestamp(info['write_start_ts'])
        write_end_obj = parse_timestamp(info['write_end_ts'])
        if write_start_obj and write_end_obj:
            write_duration_ms = (write_end_obj - write_start_obj).total_seconds() * 1000
            print(f"     Period:  {write_duration_ms:.0f}ms ({write_duration_ms/1000:.1f}s, {write_duration_ms/60000:.1f}m)")
        
        # Write count
        print(f"     Count:   {info['write_count']} writes")
        print()
    else:
        print(f"  5. WRITES STARTED: (no writes)")
        print(f"  6. WRITES ENDED:   (no writes)")
        print()

    # Destruction info
    if info['destroy_thread'] is not None:
        print(f"  DESTRUCTION:")
        print(f"     Thread:  {info['destroy_thread']}")
        print(f"     Time:    {info['destroy_ts']}")
        print()

    print()

print()
print("=" * 80)
print("PER-THREAD ANALYSIS (excluding global tickler thread)")
print("=" * 80)
print()

# Build thread activity map: which threads perform which operations for which tags
thread_activity = defaultdict(lambda: {
    'first_seen_ts': None,
    'last_seen_ts': None,
    'tags_handled': set(),
    'operations': defaultdict(int),
})

# Scan the debug.txt file to build thread activity information
with open(debug_log_file, 'r', encoding='utf-8', errors='ignore') as f:
    for line in f:
        thread = extract_thread(line)
        tag_id = extract_tag_id(line)
        ts_str = extract_timestamp(line)
        ts_obj = parse_timestamp(ts_str)
        
        if thread is None or (tickler_thread and thread == tickler_thread):
            continue
        
        # Track thread first/last appearance
        if ts_obj:
            if thread_activity[thread]['first_seen_ts'] is None:
                thread_activity[thread]['first_seen_ts'] = ts_obj
            thread_activity[thread]['last_seen_ts'] = ts_obj
        
        # Track which tags this thread handles (anything with a tag ID > 0)
        if tag_id and tag_id > 0:
            thread_activity[thread]['tags_handled'].add(tag_id)
        
        # Track operation types by looking for function markers
        if 'send_request:' in line:
            thread_activity[thread]['operations']['send_request'] += 1
        if 'socket_write:' in line:
            thread_activity[thread]['operations']['socket_write'] += 1
        if 'receive_response:' in line:
            thread_activity[thread]['operations']['receive_response'] += 1
        if 'socket_read:' in line:
            thread_activity[thread]['operations']['socket_read'] += 1
        if 'modbus_plc_handler:' in line:
            thread_activity[thread]['operations']['modbus_plc_handler'] += 1
        if 'plc_tag_create_ex:' in line:
            thread_activity[thread]['operations']['plc_tag_create_ex'] += 1
        if 'plc_tag_destroy:' in line:
            thread_activity[thread]['operations']['plc_tag_destroy'] += 1
        if 'plc_tag_read:' in line:
            thread_activity[thread]['operations']['plc_tag_read'] += 1
        if 'plc_tag_write:' in line:
            thread_activity[thread]['operations']['plc_tag_write'] += 1

# Display per-thread information
for thread in sorted(thread_activity.keys()):
    info = thread_activity[thread]
    
    if info['first_seen_ts'] is None:
        continue
    
    print(f"THREAD {thread}:")
    print()
    
    # Active duration
    first_ts_str = info['first_seen_ts'].strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    last_ts_str = info['last_seen_ts'].strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    duration_ms = (info['last_seen_ts'] - info['first_seen_ts']).total_seconds() * 1000
    
    print(f"  First Appearance: {first_ts_str}")
    print(f"  Last Appearance:  {last_ts_str}")
    print(f"  Active Duration:  {duration_ms:.0f}ms ({duration_ms/1000:.1f}s)")
    print()
    
    # Tags handled
    if info['tags_handled']:
        sorted_tags = sorted(info['tags_handled'])
        print(f"  Tags Handled: {', '.join(map(str, sorted_tags))}")
        print()
    
    # Operation summary
    if info['operations']:
        print(f"  Operations:")
        for op_type in sorted(info['operations'].keys()):
            count = info['operations'][op_type]
            print(f"    {op_type:20s}: {count:6d}")
        print()
    
    print()

