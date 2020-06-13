#include "../include/plctag.hpp"

plctag::plctag(int plc_prot, std::string ip_address, bool debug) // class constructor
{
	INFO << "started";

	try
	{
		this->debug = debug;

		// allocate room for tags in vector
		for (size_t i = 0; i < 20; i++)
		{
			tag.push_back(0);
		}

		if (debug)
		{
			plc_tag_set_debug_level(PLCTAG_DEBUG_SPEW);
		}

		create_tag(1, plc_prot, 5000, ip_address, "test_string", ELE_STRING, 5);
		create_tag(2, plc_prot, 5000, ip_address, "test_string[0]", ELE_STRING, 1);
		create_tag(3, plc_prot, 5000, ip_address, "test_float", ELE_FLOAT, 5);

	}
	catch (int error)
	{
		ERROR << "error = " << error;
		throw - 2;
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		throw - 1;
	}

	INFO << "ended";
}

plctag::~plctag(void) // class destructor
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	INFO << "started";

	try
	{
		// close all tags
		for (size_t i = 0; i < tag.size(); i++)
		{
			if (tag.at(i))
			{
				int status = plc_tag_destroy(tag.at(i));
				if (status != PLCTAG_STATUS_OK)
				{
					ERROR << "tag.at(" << i << ") >> plc_tag_destroy >> error = " << plc_tag_decode_error(status);
				}
			}
		}

		// shutdown library
		plc_tag_shutdown();
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

void plctag::create_tag(int tag_num, int plc_prot, int timeout, std::string ip_address, std::string element_name, int element_size, int element_count)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	std::string cpu, tag_path, plc_path;
	int status;

	if (debug)
	{
		INFO << "started";
	}

	try
	{
		// build tag path
		switch (plc_prot)
		{
		case PROT_AB_PLC: // 1
		{
			cpu = "plc";
			plc_path = "1";
			break;
		}
		case PROT_AB_MLGX800: // 2
		{
			cpu = "mlgx800";
			plc_path = "1";
			break;
		}
		case PROT_AB_MLGX: // 3
		{
			cpu = "mlgx";
			plc_path = "1";
			break;
		}
		case PROT_AB_LGX: // 4
		{
			cpu = "lgx";
			plc_path = "1,0";
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
				   "&path=" + plc_path + // backplane slot number
				   "&cpu=" + cpu +
				   "&elem_size=" + std::to_string(element_size) +
				   "&elem_count=" + std::to_string(element_count) +
				   "&name=" + element_name +
				   "&share_session=" + std::to_string(share_session);
		if (debug)
		{
			tag_path += "&debug=3"; // enables low level stderr output
			// Output to file: ./portalogic5 &> output.txt (captures stdout and stderr)
			INFO << "tag_path = " + tag_path;
		}

		// connect tag to PLC
		tag.at(tag_num) = plc_tag_create(tag_path.c_str(), timeout);
		if (tag.at(tag_num) < 0)
		{
			ERROR << "tag.at(" << tag_num << ") >> could not create tag >> error = " << plc_tag_decode_error(tag.at(tag_num)) << "; tag_path = " << tag_path;
			throw - 3;
		}
		status = plc_tag_status(tag.at(tag_num));
		if (status != PLCTAG_STATUS_OK)
		{
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

/// Read

std::vector<float> plctag::read_tag(int tag_num, int timeout, int element_size, int element_count)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	int status = 0;
	float fval = 0;
	std::vector<float> fvec;

	if (debug)
	{
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

	if (debug)
	{
		INFO << "ended";
	}

	return fvec;
}

std::vector<std::string> plctag::read_tag_str(int tag_num, int timeout, int element_size, int element_count)
{
	boost::mutex::scoped_lock lock(mutex); // prevent multiple threads

	int status = 0;
	std::vector<std::string> svec;

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
			char_str[j] = (char)0;	   // null terminate char array
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
