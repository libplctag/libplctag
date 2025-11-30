#!/usr/bin/env python3
"""
Analyze Modbus tag operations from test logs.
Generates a table showing read requests, responses, and connection info.
"""

import re
from datetime import datetime
from collections import defaultdict
import sys

class TagAnalyzer:
    def __init__(self, log_file):
        self.log_file = log_file
        self.tags = defaultdict(lambda: {
            'read_requests': 0,
            'read_responses': 0,
            'write_requests': 0,
            'write_responses': 0,
            'fds': set(),
            'first_read_request_time': None,
            'first_read_response_time': None,
            'last_read_request_time': None,
            'last_read_response_time': None,
            'first_write_request_time': None,
            'first_write_response_time': None,
            'last_write_request_time': None,
            'last_write_response_time': None,
        })

    def parse_timestamp(self, ts_str):
        """Parse timestamp string to datetime object"""
        try:
            return datetime.strptime(ts_str, '%Y-%m-%d %H:%M:%S.%f')
        except:
            return None

    def process_log(self):
        """Process the log file and extract tag information"""
        with open(self.log_file, 'r') as f:
            for line in f:
                # Parse log line format: timestamp thread(N) tag(M) [MODULE] LEVEL function:line message
                match = re.match(
                    r'(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)\s+'
                    r'thread\((\d+)\)\s+'
                    r'tag\((\d+)\)\s+'
                    r'\[([^\]]+)\]\s+'
                    r'([A-Z]+)\s+'
                    r'([^:]+):(\d+)\s+'
                    r'(.*)',
                    line
                )

                if not match:
                    continue

                ts_str, thread_id, tag_id, module, level, function, line_no, message = match.groups()
                tag_id = int(tag_id)
                timestamp = self.parse_timestamp(ts_str)

                if not timestamp:
                    continue

                # Track socket fd assignments (from socket_write messages)
                if 'socket_write' in message and 'fd=' in message:
                    fd_match = re.search(r'fd=(\d+)', message)
                    if fd_match:
                        fd = fd_match.group(1)
                        self.tags[tag_id]['fds'].add(fd)

                # Track read requests
                if 'preparing read request' in message:
                    self.tags[tag_id]['read_requests'] += 1
                    if self.tags[tag_id]['first_read_request_time'] is None:
                        self.tags[tag_id]['first_read_request_time'] = timestamp
                    self.tags[tag_id]['last_read_request_time'] = timestamp

                # Track write requests
                if 'preparing write request' in message:
                    self.tags[tag_id]['write_requests'] += 1
                    if self.tags[tag_id]['first_write_request_time'] is None:
                        self.tags[tag_id]['first_write_request_time'] = timestamp
                    self.tags[tag_id]['last_write_request_time'] = timestamp

                # Track read responses (indicated by "Moving tag X to end" after tag_op_read_response)
                # Tags start with ID 0, so extract actual tag ID from message text
                if 'Moving tag' in message and 'READ_RESPONSE' in message:
                    actual_tag_match = re.search(r'Moving tag (\d+)', message)
                    if actual_tag_match:
                        actual_tag_id = int(actual_tag_match.group(1))
                        self.tags[actual_tag_id]['read_responses'] += 1
                        if self.tags[actual_tag_id]['first_read_response_time'] is None:
                            self.tags[actual_tag_id]['first_read_response_time'] = timestamp
                        self.tags[actual_tag_id]['last_read_response_time'] = timestamp

                # Track write responses (indicated by "Moving tag X to end" after tag_op_write_response)
                # Tags start with ID 0, so extract actual tag ID from message text
                if 'Moving tag' in message and 'WRITE_RESPONSE' in message:
                    actual_tag_match = re.search(r'Moving tag (\d+)', message)
                    if actual_tag_match:
                        actual_tag_id = int(actual_tag_match.group(1))
                        self.tags[actual_tag_id]['write_responses'] += 1
                        if self.tags[actual_tag_id]['first_write_response_time'] is None:
                            self.tags[actual_tag_id]['first_write_response_time'] = timestamp
                        self.tags[actual_tag_id]['last_write_response_time'] = timestamp

                # Track moving tags (which indicates connection/fd being used)
                if 'Moving tag' in message and 'completed' in message:
                    # Extract current fd from context - we need socket info
                    pass

    def print_table(self):
        """Print analysis results as a table"""
        # Filter to only tags that actually did something
        active_tags = {tid: info for tid, info in self.tags.items()
                      if info['read_requests'] > 0 or info['write_requests'] > 0}

        if not active_tags:
            print("No tag activity found in log")
            return

        print("\n" + "="*170)
        print(f"{'Tag':<6} {'Read Req':<10} {'Read Resp':<10} {'Write Req':<10} {'Write Resp':<10} {'FDs':<8} "
              f"{'First Read Req':<25} {'First Read Resp':<25} {'Last Read Req':<25} {'Last Read Resp':<25}")
        print("="*170)

        for tag_id in sorted(active_tags.keys()):
            info = active_tags[tag_id]

            # Format timestamps
            first_read_req = info['first_read_request_time'].strftime('%H:%M:%S.%f')[:-3] if info['first_read_request_time'] else "N/A"
            first_read_resp = info['first_read_response_time'].strftime('%H:%M:%S.%f')[:-3] if info['first_read_response_time'] else "N/A"
            last_read_req = info['last_read_request_time'].strftime('%H:%M:%S.%f')[:-3] if info['last_read_request_time'] else "N/A"
            last_read_resp = info['last_read_response_time'].strftime('%H:%M:%S.%f')[:-3] if info['last_read_response_time'] else "N/A"

            # Format FD info
            fd_str = ','.join(sorted(info['fds'])) if info['fds'] else "?"
            if len(info['fds']) > 1:
                fd_str = fd_str + " *"  # Mark if using multiple fds

            print(f"{tag_id:<6} {info['read_requests']:<10} {info['read_responses']:<10} "
                  f"{info['write_requests']:<10} {info['write_responses']:<10} {fd_str:<8} "
                  f"{first_read_req:<25} {first_read_resp:<25} {last_read_req:<25} {last_read_resp:<25}")

        print("="*170)

        # Summary statistics
        total_read_req = sum(info['read_requests'] for info in active_tags.values())
        total_read_resp = sum(info['read_responses'] for info in active_tags.values())
        total_write_req = sum(info['write_requests'] for info in active_tags.values())
        total_write_resp = sum(info['write_responses'] for info in active_tags.values())
        tags_with_multi_fd = sum(1 for info in active_tags.values() if len(info['fds']) > 1)

        print(f"\nSummary:")
        print(f"  Total Tags: {len(active_tags)}")
        print(f"  Tags using multiple FDs: {tags_with_multi_fd}")
        print(f"  Total Read Requests: {total_read_req}")
        print(f"  Total Read Responses: {total_read_resp}")
        print(f"  Read Completion Rate: {100*total_read_resp/total_read_req if total_read_req > 0 else 0:.1f}%")
        print(f"  Total Write Requests: {total_write_req}")
        print(f"  Total Write Responses: {total_write_resp}")
        print(f"  Write Completion Rate: {100*total_write_resp/total_write_req if total_write_req > 0 else 0:.1f}%")
        print(f"\nNote: '*' in FDs column indicates tag used multiple socket connections (possible reconnect)")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_tags.py <log_file>")
        sys.exit(1)

    log_file = sys.argv[1]
    analyzer = TagAnalyzer(log_file)
    analyzer.process_log()
    analyzer.print_table()
