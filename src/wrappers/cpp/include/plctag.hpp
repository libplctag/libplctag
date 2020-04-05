#ifndef PLCTAG_HPP
#define PLCTAG_HPP

#include <boost/thread/mutex.hpp>

#include <string>
#include <vector>
#include <libplctag.h>
#include "logging.hpp"

class plctag
{
	public:
		plctag();	// class constructor
		~plctag();	// class destructor

        // library control.
        static int32_t version(void);

        enum DEBUG_LEVEL // Debug levels, see libplctag.h
        {
            DEBUG_NONE = PLCTAG_DEBUG_NONE,
            DEBUG_ERROR = PLCTAG_DEBUG_ERROR,
            DEBUG_WARN = PLCTAG_DEBUG_WARN,
            DEBUG_INFO = PLCTAG_DEBUG_INFO,
            DEBUG_DETAIL = PLCTAG_DEBUG_DETAIL,
            DEBUG_SPEW = PLCTAG_DEBUG_SPEW
        };
        static void set_debug_level(DEBUG_LEVEL debug_level);

		// Connection
		void error_recovery (int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count);
		void initialize_tags (int plc_prot, std::string ip_address);

		// Read
		std::vector <float> read_tag (int tag_num, int timeout, int element_size, int element_count);
		std::vector <std::string> read_tag_str (int tag_num, int timeout, int element_size, int element_count);

		// Write
		void write_tag (int tag_num, int timeout, int element_size, std::vector <float> fvec);
		void write_tag_str (int tag_num, int timeout, int element_size, std::vector <std::string> fvec);

		// Variables
		std::string plc_1_ip_addr, plc_2_ip_addr;
		enum PLC_PROT	// Protocols, see "check_cpu" in ab_common.c
		{
			PROT_AB_PLC = 1,	// PLC, PLC5, SLC, SLC500
			PROT_AB_MLGX800 = 2,	// MicroLogix 800
			PROT_AB_MLGX = 3,	// MicroLogix 1000-1400
			PROT_AB_LGX = 4,		// CompactLogix, ControlLogix, FlexLogix
		};
		enum ELE_SIZE	// element size
		{
			ELE_BOOL = 1,
			ELE_INT = 2,
			ELE_DINT_ = 4,
			ELE_FLOAT = 4,
			ELE_STRING = 88,
		};
	private:
		// Connection
		void create_tag (int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count);
		void close_tag (int tag_num);
		void close_all_tags (void);
		// Variables
		boost::mutex mutex;
		std::vector <int32_t> tag;
		std::string plc_protocol = "ab_eip";
		int plc_path = 1;	// backplane slot number
		int share_session = 1;	// 1: shares TCP connection with other tags at same IP address; 0: off
		int str_data_size = 82;
};

#endif
