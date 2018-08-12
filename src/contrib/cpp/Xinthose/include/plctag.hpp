#ifndef __PLCTAG_HPP
#define __PLCTAG_HPP

#include <string>
#include <vector>
#include <iostream>
#include <ctime>
#include <iomanip>

#include <unistd.h>
#include <string.h> 

#include "plctag/libplctag.h"

#define INFO std::cout << std::put_time(std::localtime(&time_now), "%y-%m-%d %OH:%OM:%OS") << " [INFO] " << __FILE__ << "(" << __FUNCTION__ << ":" << __LINE__ << ") >> "
#define ERROR std::cout << std::put_time(std::localtime(&time_now), "%y-%m-%d %OH:%OM:%OS") << " [ERROR] " << __FILE__ << "(" << __FUNCTION__ << ":" << __LINE__ << ") >> "

static std::time_t time_now = std::time(nullptr);

class plctag
{
	public:
		//// Functions
		plctag();	// class constructor
		~plctag();	// class destructor
		/// Connection
		void create_tag (int tag_num, int plc_prot, std::string ip_address, std::string element_name, int element_size, int element_count);
		void close_tag (int tag_num);
		void close_all_tags (void);
		void error_recovery (int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count);
		void initialize_tags (int plc_prot, std::string ip_address);
		/// Read
		std::vector <float> read_tag (int tag_num, int timeout, int element_size, int element_count);
		std::vector <std::string> read_tag_str (int tag_num, int timeout, int element_size, int element_count);

		/// Write
		void write_tag (int tag_num, int timeout, int element_size, std::vector <float> fvec);
		void write_tag_str (int tag_num, int timeout, int element_size, std::vector <std::string> fvec);

		/// Other

		/// Variables
		std::string plc_1_ip_addr = "10.1.1.67";
		std::string plc_2_ip_addr = "10.0.0.4";
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
		/// Functions
		std::string decode_error (int rc);
		
		/// Variables
		std::vector <plc_tag> tag = std::vector <plc_tag> (15, PLC_TAG_NULL);		
		std::string plc_protocol = "ab_eip";
		int plc_path = 1;	// backplane slot number
		int share_session = 1;	// 1: shares TCP connection with other tags at same IP address; 0: off
		int str_data_size = 82;	
};

#endif	// __PLCTAG_HPP
