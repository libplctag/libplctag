#include "../include/plctag.hpp"

plctag::plctag (void)	// class constructor
{
	INFO << "started" << std::endl;
	
	try
	{

	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;		
	}
	
	INFO << "ended" << std::endl;
}

plctag::~plctag (void)	// class destructor
{
	INFO << "started" << std::endl;
	
	try
	{	
		close_all_tags();
	}
	catch (int error)
	{
		ERROR << "error = " << error << std::endl;
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;		
	}
	
	INFO << "ended" << std::endl;
}

/// Connection

void plctag::create_tag (int tag_num, int plc_prot, std::string ip_address, std::string element_name, int element_size, int element_count)
{			
	std::string cpu, tag_path;
	int status;
	bool debug = false;
	
	if (debug) INFO << "started" << std::endl;

	try
	{
		// 1. build tag path
		switch (plc_prot)
		{
			case PROT_AB_PLC:	// 1
			{
				cpu = "plc";
				break;
			}
			case PROT_AB_MLGX800:	// 2
			{
				cpu = "mlgx800";				
				break;
			}
			case PROT_AB_MLGX:	// 3
			{
				cpu = "mlgx";
				break;
			}
			case PROT_AB_LGX:	// 4
			{
				cpu = "lgx";
				break;
			}
			default:
			{
				ERROR << "plc_prot unhandled = " << plc_prot << std::endl;
				throw -2;
			}
		}
		// "protocol=ab_eip&gateway=192.168.1.42&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=myDINTArray"
		tag_path = "protocol=" + plc_protocol +
			"&gateway=" + ip_address + 
			"&path=" + std::to_string(plc_path) + 
			"&cpu=" + cpu + 
			"&elem_size=" + std::to_string(element_size) + 
			"&elem_count=" + std::to_string(element_count) + 
			"&name=" + element_name +
			"&share_session=1";	// share same session with other tags
		if (debug) INFO << "tag_path = " + tag_path << std::endl;	
		
		// 2. connect tag to PLC
		tag.at(tag_num) = plc_tag_create(tag_path.c_str());			
		while (plc_tag_status(tag.at(tag_num)) == PLCTAG_STATUS_PENDING) 
		{
			usleep(500000);
		}		
		status = plc_tag_status(tag.at(tag_num));
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_create >> tag_path = " + tag_path + "; status = " + decode_error(status) << std::endl;
			throw -3;
		}	
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}
	
	INFO << "tag[" + std::to_string(tag_num) + "] >> created; ended" << std::endl;

	return;
}

void plctag::close_tag (int tag_num)
{
	INFO << "started" << std::endl;
	
	int status = 0;
			
	try
	{
		plc_tag_unlock(tag.at(tag_num));
		status = plc_tag_destroy(tag.at(tag_num));
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_destroy >> status = " + decode_error(status) << std::endl;
			throw -2;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}

	INFO << "ended" << std::endl;

	return;
}

void plctag::close_all_tags (void)
{
	INFO << "started" << std::endl;
	
	int status = 0;
			
	try
	{
		for (size_t i = 0; i < tag.size(); i++)
		{
			plc_tag_unlock(tag.at(i));
			if (tag.at(i))
			{
				status = plc_tag_destroy(tag.at(i));
				if (status != PLCTAG_STATUS_OK) 
				{
					ERROR << "tag[" + std::to_string(i) + "] >> plc_tag_destroy >> status = " + decode_error(status) << std::endl;
				}
			}
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}

	INFO << "ended" << std::endl;

	return;
}

void plctag::error_recovery (int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count)
{
	INFO << "started" << std::endl;
			
	std::string cpu, tag_path;
	std::vector <float> fvec (1, 0);
	int status;
	bool debug = false;

	try
	{
		// 1.  close all tags
		close_all_tags();
		
		// 2. build tag path
		switch (plc_prot)
		{
			case PROT_AB_PLC:	// 1
			{
				cpu = "plc";
				break;
			}
			case PROT_AB_MLGX800:	// 2
			{
				cpu = "mlgx800";				
				break;
			}
			case PROT_AB_MLGX:	// 3
			{
				cpu = "mlgx";
				break;
			}
			case PROT_AB_LGX:	// 4
			{
				cpu = "lgx";
				break;
			}
			default:
			{
				ERROR << "plc_prot unhandled = " << plc_prot << std::endl;
				throw -2;
			}
		}
		// "protocol=ab_eip&gateway=192.168.1.42&path=1,0&cpu=LGX&elem_size=4&elem_count=10&name=myDINTArray"
		tag_path = "protocol=" + plc_protocol +
			"&gateway=" + ip_address + 
			"&path=" + std::to_string(plc_path) + 
			"&cpu=" + cpu + 
			"&elem_size=" + std::to_string(element_size) + 
			"&elem_count=" + std::to_string(element_count) + 
			"&name=" + element_name +
			"&share_session=1";	// share same session with other tags
		if (debug) INFO << "tag_path = " + tag_path << std::endl;	
		
		// 3. connect tag to PLC
		tag.at(tag_num) = plc_tag_create(tag_path.c_str());			
		while (plc_tag_status(tag.at(tag_num)) == PLCTAG_STATUS_PENDING) 
		{
			usleep(500000);
		}		
		status = plc_tag_status(tag.at(tag_num));
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_create >> tag_path = " + tag_path + "; status = " + decode_error(status) << std::endl;
			throw -3;
		}	
		
		// 4.  attempt read
		read_tag(tag_num, timeout, element_size, element_count);
		
		// 5.  attempt write
		write_tag(tag_num, timeout, element_size, fvec);		
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}
	
	INFO << "tag[" + std::to_string(tag_num) + "] >> created; ended" << std::endl;

	return;
}

void plctag::initialize_tags (int plc_prot, std::string ip_address)
{
	INFO << "started; plc_prot = " << plc_prot << "; ip_address = " << ip_address << std::endl;
				
	try
	{
		create_tag(1, plc_prot, ip_address, "test_string", ELE_STRING, 5);
		create_tag(2, plc_prot, ip_address, "test_string[0]", ELE_STRING, 1);
		create_tag(3, plc_prot, ip_address, "test_float", ELE_FLOAT, 5);
	}
	catch (int error)
	{
		ERROR << "create_tag >> error = " << error << std::endl;	
		throw -2;		
	}				
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}

	INFO << "ended" << std::endl;

	return;
}

/// Read

std::vector <float> plctag::read_tag (int tag_num, int timeout, int element_size, int element_count)
{
	int status = 0;
	float fval = 0;
	std::vector <float> fvec;
	bool debug = false;
	
	if (debug) INFO << "started" << std::endl;
			
	try
	{
		plc_tag_lock(tag.at(tag_num));
		
		// 1. read data
		status = plc_tag_read(tag.at(tag_num), timeout);	// timeout (ms)
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_read >> status = " + decode_error(status) << std::endl;
			throw -2;
		}
		
		// 2. format data
		for (int i = 0; i < element_count; i++) 
		{
			fval = plc_tag_get_float32(tag.at(tag_num), (i * element_size));	// tag, offset
			status = plc_tag_status(tag.at(tag_num));
			if (status != PLCTAG_STATUS_OK) 
			{
				ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_get_float32 >> status = " + decode_error(status) << std::endl;
				throw -3;
			}
			fvec.push_back(fval);
		}
		
		plc_tag_unlock(tag.at(tag_num));
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}
	
	if (debug) INFO << "ended" << std::endl;
	
	return fvec;
}

std::vector <std::string> plctag::read_tag_str (int tag_num, int timeout, int element_size, int element_count)
{
	int status = 0;
	std::vector <std::string> svec;
	bool debug = false;
	
	if (debug) INFO << "started" << std::endl;
			
	try
	{
		plc_tag_lock(tag.at(tag_num));
		
		// 1. read data
		status = plc_tag_read(tag.at(tag_num), timeout);	// timeout (ms)
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_read >> status = " + decode_error(status) << std::endl;
			throw -2;
		}
		// 2. format data
		for (int i = 0; i < element_count; i++) 
		{
			/// method 1
			int str_size = plc_tag_get_int32(tag.at(tag_num), (i * element_size));
			status = plc_tag_status(tag.at(tag_num));
			if (status != PLCTAG_STATUS_OK) 
			{
				ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_get_int32 >> status = " + decode_error(status) << std::endl;
				throw -3;
			}
			
			char char_str[83] = {0};
			int j = 0;
			
			for (j = 0; j < str_size; j++) 
			{
				char_str[j] = (char) plc_tag_get_uint8(tag.at(tag_num), ((i * element_size) + j + 4));
				status = plc_tag_status(tag.at(tag_num));
				if (status != PLCTAG_STATUS_OK) 
				{
					ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_get_uint8 >> status = " + decode_error(status) << std::endl;
					throw -4;
				}
			}
			char_str[j] = (char) 0;	// null terminate char array
			std::string str(char_str);	// convert char array to string
			svec.push_back(str);			
		}
		
		plc_tag_unlock(tag.at(tag_num));
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}
	
	if (debug) INFO << "ended" << std::endl;
	
	return svec;
}

/// Write

void plctag::write_tag (int tag_num, int timeout, int element_size, std::vector <float> fvec)
{
	int status = 0;
	bool debug = false;

	if (debug) INFO << "started" << std::endl;
		
	try
	{		
		plc_tag_lock(tag.at(tag_num));
		
		// 1. set data
		for (size_t i = 0; i < fvec.size(); i++) 
		{
			status = plc_tag_set_float32(tag.at(tag_num), (i * element_size), fvec.at(i));	// tag, offset, value
			if (status != PLCTAG_STATUS_OK) 
			{
				ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_set_float32 >> status = " + decode_error(status) << std::endl;
				throw -2;
			}
		}
		
		// 2. write data
		status = plc_tag_write(tag.at(tag_num), timeout);	// timeout (ms)
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_write >> status = " + decode_error(status) << std::endl;
			throw -3;
		}
		
		plc_tag_unlock(tag.at(tag_num));
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}
	
	if (debug) INFO << "ended" << std::endl;
	
	return;
}

void plctag::write_tag_str (int tag_num, int timeout, int element_size, std::vector <std::string> svec)
{
	int status = 0;
	bool debug = false;
			
	if (debug) INFO << "started" << std::endl;
	
	try
	{
		plc_tag_lock(tag.at(tag_num));

		// 1. set the data
		for (size_t i = 0; i < svec.size(); i++)
		{
			int str_len = 0, str_index = 0;
			int base_offset = i * element_size;
			char* str = (char*) svec.at(i).c_str();
			
			// a.  set the length
			str_len = strlen(str);
			if (debug) INFO << "base_offset = " << base_offset << "; str_len = " << str_len << std::endl;
			status = plc_tag_set_int32(tag.at(tag_num), base_offset, str_len);
			if (status != PLCTAG_STATUS_OK)
			{
				ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_set_int32 >> status = " + decode_error(status) << std::endl;
				throw -2;
			}
						
			// b.  copy the data
			for (str_index = 0; str_index < str_len; str_index++) 
			{
				status = plc_tag_set_uint8(tag.at(tag_num), (base_offset + 4 + str_index), str[str_index]);
				if (status != PLCTAG_STATUS_OK) 
				{
					ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_set_uint8 >> status = " + decode_error(status) << std::endl;
					throw -3;
				}
			}

			// c.  pad with zeros
			for (; str_index < str_data_size; str_index++) 
			{
				status = plc_tag_set_uint8(tag.at(tag_num), (base_offset + 4 + str_index), 0);
				if (status != PLCTAG_STATUS_OK) 
				{
					ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_set_uint8 >> status = " + decode_error(status) << std::endl;
					throw -4;
				}
			}
		}
		
		// 2. write the data
		status = plc_tag_write(tag.at(tag_num), timeout);	// timeout (ms)
		if (status != PLCTAG_STATUS_OK) 
		{
			ERROR << "tag[" + std::to_string(tag_num) + "] >> plc_tag_write >> status = " + decode_error(status) << std::endl;
			throw -5;
		}

		plc_tag_unlock(tag.at(tag_num));
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}
	
	if (debug) INFO << "ended" << std::endl;
	
	return;
}

/// Other

std::string plctag::decode_error (int rc)
{
	switch (rc) 
	{
		case PLCTAG_STATUS_PENDING: return "PLCTAG_STATUS_PENDING"; break;
		case PLCTAG_STATUS_OK: return "PLCTAG_STATUS_OK"; break;
		case PLCTAG_ERR_NULL_PTR: return "PLCTAG_ERR_NULL_PTR"; break;
		case PLCTAG_ERR_OUT_OF_BOUNDS: return "PLCTAG_ERR_OUT_OF_BOUNDS"; break;
		case PLCTAG_ERR_NO_MEM: return "PLCTAG_ERR_NO_MEM"; break;
		case PLCTAG_ERR_LL_ADD: return "PLCTAG_ERR_LL_ADD"; break;
		case PLCTAG_ERR_BAD_PARAM: return "PLCTAG_ERR_BAD_PARAM"; break;
		case PLCTAG_ERR_CREATE: return "PLCTAG_ERR_CREATE"; break;
		case PLCTAG_ERR_NOT_EMPTY: return "PLCTAG_ERR_NOT_EMPTY"; break;
		case PLCTAG_ERR_OPEN: return "PLCTAG_ERR_OPEN"; break;
		case PLCTAG_ERR_SET: return "PLCTAG_ERR_SET"; break;
		case PLCTAG_ERR_WRITE: return "PLCTAG_ERR_WRITE"; break;
		case PLCTAG_ERR_TIMEOUT: return "PLCTAG_ERR_TIMEOUT"; break;
		case PLCTAG_ERR_TIMEOUT_ACK: return "PLCTAG_ERR_TIMEOUT_ACK"; break;
		case PLCTAG_ERR_RETRIES: return "PLCTAG_ERR_RETRIES"; break;
		case PLCTAG_ERR_READ: return "PLCTAG_ERR_READ"; break;
		case PLCTAG_ERR_BAD_DATA: return "PLCTAG_ERR_BAD_DATA"; break;
		case PLCTAG_ERR_ENCODE: return "PLCTAG_ERR_ENCODE"; break;
		case PLCTAG_ERR_DECODE: return "PLCTAG_ERR_DECODE"; break;
		case PLCTAG_ERR_UNSUPPORTED: return "PLCTAG_ERR_UNSUPPORTED"; break;
		case PLCTAG_ERR_TOO_LONG: return "PLCTAG_ERR_TOO_LONG"; break;
		case PLCTAG_ERR_CLOSE: return "PLCTAG_ERR_CLOSE"; break;
		case PLCTAG_ERR_NOT_ALLOWED: return "PLCTAG_ERR_NOT_ALLOWED"; break;
		case PLCTAG_ERR_THREAD: return "PLCTAG_ERR_THREAD"; break;
		case PLCTAG_ERR_NO_DATA: return "PLCTAG_ERR_NO_DATA"; break;
		case PLCTAG_ERR_THREAD_JOIN: return "PLCTAG_ERR_THREAD_JOIN"; break;
		case PLCTAG_ERR_THREAD_CREATE: return "PLCTAG_ERR_THREAD_CREATE"; break;
		case PLCTAG_ERR_MUTEX_DESTROY: return "PLCTAG_ERR_MUTEX_DESTROY"; break;
		case PLCTAG_ERR_MUTEX_UNLOCK: return "PLCTAG_ERR_MUTEX_UNLOCK"; break;
		case PLCTAG_ERR_MUTEX_INIT: return "PLCTAG_ERR_MUTEX_INIT"; break;
		case PLCTAG_ERR_MUTEX_LOCK: return "PLCTAG_ERR_MUTEX_LOCK"; break;
		case PLCTAG_ERR_NOT_IMPLEMENTED: return "PLCTAG_ERR_NOT_IMPLEMENTED"; break;
		case PLCTAG_ERR_BAD_DEVICE: return "PLCTAG_ERR_BAD_DEVICE"; break;
		case PLCTAG_ERR_BAD_GATEWAY: return "PLCTAG_ERR_BAD_GATEWAY"; break;
		case PLCTAG_ERR_REMOTE_ERR: return "PLCTAG_ERR_REMOTE_ERR"; break;
		case PLCTAG_ERR_NOT_FOUND: return "PLCTAG_ERR_NOT_FOUND"; break;
		case PLCTAG_ERR_ABORT: return "PLCTAG_ERR_ABORT"; break;
		case PLCTAG_ERR_WINSOCK: return "PLCTAG_ERR_WINSOCK"; break;

		default: return "Unknown error."; break;
	}

	return "Unknown error.";
}

