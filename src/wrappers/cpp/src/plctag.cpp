#include "../include/plctag.hpp"

plctag::plctag(void) // class constructor
{
	INFO << "started";

	try
	{
		for (size_t i = 0; i < 20; i++)
		{
			tag.push_back(0);
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
	}

	INFO << "ended";
}

plctag::~plctag(void) // class destructor
{
	INFO << "started";

	try
	{
		close_all_tags();
	}
	catch (int error)
	{
		ERROR << "error = " << error;
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
	}

	INFO << "ended";
}

/// Static methods

// return the library version as an encoded integer.
int32_t plctag::version(void)
{
    return plc_tag_get_lib_version();
}

// set the debug level.  Use values from 1-5.
void plctag::set_debug_level(plctag::DEBUG_LEVEL debug_level)
{
    plc_tag_set_debug_level(debug_level);
}


/// Connection

void plctag::create_tag(int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	std::string cpu, tag_path;
	int status;
	bool debug = false;

	if (debug)
	{
		INFO << "started";
	}

	try
	{
		// build tag path
		switch (plc_prot) {
			case PROT_AB_PLC: // 1
			{
				cpu = "plc";
				break;
			}
			case PROT_AB_MLGX800: // 2
			{
				cpu = "mlgx800";
				break;
			}
			case PROT_AB_MLGX: // 3
			{
				cpu = "mlgx";
				break;
			}
			case PROT_AB_LGX: // 4
			{
				cpu = "lgx";
				break;
			}
			default:
			{
				ERROR << "plc_prot unhandled = " << plc_prot;
				throw - 2;
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
				   "&share_session=1"; // share same session with other tags
		if (debug) {
			tag_path += "&debug=3"; // enables low level stderr output
			// Output to file: ./portalogic5 &> output.txt (captures stdout and stderr)
			INFO << "tag_path = " + tag_path;
		}

		// connect tag to PLC
		tag.at(tag_num) = plc_tag_create(tag_path.c_str(), timeout);
		if (tag.at(tag_num) < 0) {
			ERROR << "tag.at(" << tag_num << ") >> could not create tag >> error = " << plc_tag_decode_error(tag.at(tag_num)) << "; tag_path = " << tag_path;
			throw - 3;
		}
		status = plc_tag_status(tag.at(tag_num));
		if (status != PLCTAG_STATUS_OK) {
			ERROR << "tag.at(" << tag_num << ") >> error setting up tag internal state >> error = " << plc_tag_decode_error(tag.at(tag_num)) << "; tag_path = " << tag_path;
			throw - 4;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	INFO << "tag[" + std::to_string(tag_num) + "] >> created; ended";

	return;
}

void plctag::close_tag(int tag_num)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	INFO << "started";

	int status = 0;

	try
	{
		status = plc_tag_destroy(tag.at(tag_num));
		if (status != PLCTAG_STATUS_OK)
		{
			ERROR << "tag.at(" << tag_num << ") >> plc_tag_destroy >> error = " << plc_tag_decode_error(status);
			throw - 2;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	INFO << "ended";

	return;
}

void plctag::close_all_tags(void)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	INFO << "started";

	int status = 0;

	try
	{
		for (size_t i = 0; i < tag.size(); i++)
		{
			if (tag.at(i))
			{
				status = plc_tag_destroy(tag.at(i));
				if (status != PLCTAG_STATUS_OK)
				{
					ERROR << "tag.at(" << i << ") >> plc_tag_destroy >> error = " << plc_tag_decode_error(status);
				}
			}
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	INFO << "ended";

	return;
}

void plctag::error_recovery(int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count)
{
	INFO << "started";

	std::vector<float> fvec_reset(1, 0);

	try
	{
		// 1.  close all tags
		try
		{
			close_all_tags();
		}
		catch (int error)
		{
			ERROR << "close_all_tags >> error = " << error;
			throw - 2;
		}

		// 2. create testing tag
		try
		{
			create_tag(tag_num, plc_prot, timeout, ip_address, element_name, element_size, element_count); // for error recovery
		}
		catch (int error)
		{
			ERROR << "create_tag >> error = " << error;
			throw - 3;
		}

		// 3. attempt read
		try
		{
			read_tag(tag_num, timeout, element_size, element_count);
		}
		catch (int error)
		{
			ERROR << "read_tag >> error = " << error;
			throw - 4;
		}

		// 4.  attempt write
		try
		{
			write_tag(tag_num, timeout, element_size, fvec_reset);
		}
		catch (int error)
		{
			ERROR << "write_tag >> error = " << error;
			throw - 5;
		}

		// 5. close testing tag
		try
		{
			close_tag(tag_num);
		}
		catch (int error)
		{
			ERROR << "close_tag >> error = " << error;
			throw - 6;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	INFO << "PLC connection recovered";
	INFO << "ended";

	return;
}

void plctag::initialize_tags(int plc_prot, std::string ip_address)
{
	INFO << "started; plc_prot = " << plc_prot << "; ip_address = " << ip_address;

	try
	{
		create_tag(1, plc_prot, 5000, ip_address, "test_string", ELE_STRING, 5);
		create_tag(2, plc_prot, 5000, ip_address, "test_string[0]", ELE_STRING, 1);
		create_tag(3, plc_prot, 5000, ip_address, "test_float", ELE_FLOAT, 5);
	}
	catch (int error)
	{
		ERROR << "create_tag >> error = " << error;
		throw - 2;
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	INFO << "ended";

	return;
}

/// Read

std::vector<float> plctag::read_tag(int tag_num, int timeout, int element_size, int element_count)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	int status = 0;
	float fval = 0;
	std::vector<float> fvec;
	bool debug = false;

	if (debug) {
		INFO << "started";
	}

	try
	{
		// 1. read data
		status = plc_tag_read(tag.at(tag_num), timeout); // timeout (ms)
		if (status != PLCTAG_STATUS_OK)
		{
			ERROR << "tag.at(" << tag_num << ") >> plc_tag_read >> error = " << plc_tag_decode_error(status);
			throw - 2;
		}

		// 2. format data
		switch (element_size)
		{
			case ELE_INT:
			{
				for (int i = 0; i < element_count; i++)
				{
					fval = plc_tag_get_int16(tag.at(tag_num), (i * element_size)); // tag, offset
					status = plc_tag_status(tag.at(tag_num));
					if (status != PLCTAG_STATUS_OK)
					{
						ERROR << "tag.at(" << tag_num << ") >> plc_tag_get_float32 >> error = " << plc_tag_decode_error(status);
						throw - 3;
					}
					fvec.push_back(fval);
				}
				break;
			}
			case ELE_FLOAT:
			{
				for (int i = 0; i < element_count; i++)
				{
					fval = plc_tag_get_float32(tag.at(tag_num), (i * element_size)); // tag, offset
					status = plc_tag_status(tag.at(tag_num));
					if (status != PLCTAG_STATUS_OK)
					{
						ERROR << "tag.at(" << tag_num << ") >> plc_tag_get_float32 >> error = " << plc_tag_decode_error(status);
						throw - 3;
					}
					fvec.push_back(fval);
				}
				break;
			}
			default:
			{
				ERROR << "element_size unhandled >> " << element_size;
				throw - 2;
			}
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	if (debug) {
		INFO << "ended";
	}

	return fvec;
}

std::vector<std::string> plctag::read_tag_str(int tag_num, int timeout, int element_size, int element_count)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	int status = 0;
	std::vector<std::string> svec;
	bool debug = false;

	if (debug)
		INFO << "started";

	try
	{
		// 1. read data
		status = plc_tag_read(tag.at(tag_num), timeout); // timeout (ms)
		if (status != PLCTAG_STATUS_OK)
		{
			ERROR << "tag.at(" << tag_num << ") >> plc_tag_read >> error = " << plc_tag_decode_error(status);
			throw - 2;
		}
		// 2. format data
		for (int i = 0; i < element_count; i++)
		{
			/// method 1
			int str_size = plc_tag_get_int32(tag.at(tag_num), (i * element_size));
			status = plc_tag_status(tag.at(tag_num));
			if (status != PLCTAG_STATUS_OK)
			{
				ERROR << "tag.at(" << tag_num << ") >> plc_tag_get_int32 >> error = " << plc_tag_decode_error(status);
				throw - 3;
			}

			char char_str[83] = {0};
			int j = 0;

			for (j = 0; j < str_size; j++)
			{
				char_str[j] = (char)plc_tag_get_uint8(tag.at(tag_num), ((i * element_size) + j + 4));
				status = plc_tag_status(tag.at(tag_num));
				if (status != PLCTAG_STATUS_OK)
				{
					ERROR << "tag.at(" << tag_num << ") >> plc_tag_get_uint8 >> error = " << plc_tag_decode_error(status);
					throw - 4;
				}
			}
			char_str[j] = (char)0;	 // null terminate char array
			std::string str(char_str); // convert char array to string
			svec.push_back(str);

			/// method 2
			/*char str_data[str_data_size];
			int num_strings = plc_tag_get_size(tag.at(tag_num)) / element_size;
			int str_index;

			for(int i = 0; i < num_strings; i++)
			{
				// get the string length
				int str_len = plc_tag_get_int32(tag.at(tag_num), i * element_size);

				// copy the data
				for (str_index = 0; str_index<str_len; str_index++)
				{
					str_data[str_index] = (char) plc_tag_get_uint8(tag.at(tag_num), ((i * element_size) + 4 + str_index));
				}

				// pad with zeros
				for (; str_index < str_data_size; str_index++)
				{
					str_data[str_index] = 0;
				}
				std::string str(str_data);	// convert char array to string
				svec.push_back(str);
			}*/
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	if (debug)
		INFO << "ended";

	return svec;
}

/// Write

void plctag::write_tag(int tag_num, int timeout, int element_size, std::vector<float> fvec)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	int status = 0;
	bool debug = false;

	if (debug)
	{
		INFO << "started";
	}

	try
	{
		// 1. set data
		for (size_t i = 0; i < fvec.size(); i++)
		{
			status = plc_tag_set_float32(tag.at(tag_num), (i * element_size), fvec.at(i)); // tag, offset, value
			if (status != PLCTAG_STATUS_OK)
			{
				ERROR << "tag.at(" << tag_num << ") >> plc_tag_set_float32 >> error = " << plc_tag_decode_error(status);
				throw - 2;
			}
		}

		// 2. write data
		status = plc_tag_write(tag.at(tag_num), timeout); // timeout (ms)
		if (status != PLCTAG_STATUS_OK)
		{
			ERROR << "tag.at(" << tag_num << ") >> plc_tag_write >> error = " << plc_tag_decode_error(status);
			throw - 3;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	if (debug)
		INFO << "ended";

	return;
}

void plctag::write_tag_str(int tag_num, int timeout, int element_size, std::vector<std::string> svec)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	int status = 0;
	bool debug = false;

	if (debug)
		INFO << "started";

	try
	{
		// 1. set the data
		for (size_t i = 0; i < svec.size(); i++)
		{
			int str_len = 0, str_index = 0;
			int base_offset = i * element_size;
			char *str = (char *)svec.at(i).c_str();

			// a.  set the length
			str_len = strlen(str);
			if (debug)
				INFO << "base_offset = " << base_offset << "; str_len = " << str_len;
			status = plc_tag_set_int32(tag.at(tag_num), base_offset, str_len);
			if (status != PLCTAG_STATUS_OK)
			{
				ERROR << "tag.at(" << tag_num << ") >> plc_tag_set_int32 >> error = " << plc_tag_decode_error(status);
				throw - 2;
			}

			// b.  copy the data
			for (str_index = 0; str_index < str_len; str_index++)
			{
				status = plc_tag_set_uint8(tag.at(tag_num), (base_offset + 4 + str_index), str[str_index]);
				if (status != PLCTAG_STATUS_OK)
				{
					ERROR << "tag.at(" << tag_num << ") >> plc_tag_set_uint8 >> error = " << plc_tag_decode_error(status);
					throw - 3;
				}
			}

			// c.  pad with zeros
			for (; str_index < str_data_size; str_index++)
			{
				status = plc_tag_set_uint8(tag.at(tag_num), (base_offset + 4 + str_index), 0);
				if (status != PLCTAG_STATUS_OK)
				{
					ERROR << "tag.at(" << tag_num << ") >> plc_tag_set_uint8 >> error = " << plc_tag_decode_error(status);
					throw - 4;
				}
			}
		}

		// 2. write the data
		status = plc_tag_write(tag.at(tag_num), timeout); // timeout (ms)
		if (status != PLCTAG_STATUS_OK)
		{
			ERROR << "tag.at(" << tag_num << ") >> plc_tag_write >> error = " << plc_tag_decode_error(status);
			throw - 5;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	if (debug)
		INFO << "ended";

	return;
}

/* Notes
***************************************************************************************************************
void plctag:: (void)
{
	boost::mutex::scoped_lock lock (mutex);	// prevent multiple threads

	INFO << "started";

	try
	{

	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw -1;
	}

	INFO << "ended";

	return;
}
***************************************************************************************************************
		switch (command)
		{
			case :	//
			{

				break;
			}
			default:
			{
				ERROR << "command unhandled >> " << command;
				break;
			}
		}
***************************************************************************************************************

*/
