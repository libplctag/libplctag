#
# Regular cron jobs for the libplctag package
#
0 4	* * *	root	[ -x /usr/bin/libplctag_maintenance ] && /usr/bin/libplctag_maintenance
