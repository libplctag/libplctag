#include "../include/plctag.hpp"
#include "../include/ping.hpp"

// Note: create folder "obj" first for Makefile then run command "make"

system_ping _system_ping;	
plctag _plctag;	

int plc_prot = _plctag.PROT_AB_LGX;

void ab_plc_error (void)
{
	INFO << "started" << std::endl;

	try
	{
		while (true)
		{
			sleep(10);

			// 1. ping
			if (_system_ping.test_connection(_plctag.plc_1_ip_addr, 3) != 0)
			{
				ERROR << "Cannot ping PLC at IP Address " + _plctag.plc_1_ip_addr << std::endl;
				continue;
			}

			// 2. error_recovery
			try
			{
				_plctag.error_recovery(0, plc_prot, 5000, _plctag.plc_1_ip_addr, "test_float", _plctag.ELE_FLOAT, 1);
			}
			catch (int error)
			{
				ERROR << "_plctag.error_recovery  >> error = " << error << std::endl;
				continue;
			}

			// 3. initialize_tags
			try
			{
				_plctag.initialize_tags(plc_prot, _plctag.plc_1_ip_addr);
			}
			catch (int error)
			{
				ERROR << "_plctag.initialize_tags >> error = " << error << std::endl;
				continue;
			}

			break;
		}

		INFO << "Recovered from a PLC error" << std::endl;
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		return;
	}

	INFO << "recovered" << std::endl;
	INFO << "ended" << std::endl;

	return;
}

int main (void) 
{
	INFO << "Program Started" << std::endl;

	loop:

	try
	{
		std::vector <float> fvec;
		std::vector <std::string> svec;
		
		_plctag.plc_1_ip_addr = "192.168.2.50";

		/// create tags
		_plctag.initialize_tags(plc_prot, _plctag.plc_1_ip_addr);
		/// string	
		// write
		svec.push_back("Hello World");
		_plctag.write_tag_str(2, 5000, _plctag.ELE_STRING, svec);			
		// read
		svec = _plctag.read_tag_str(1, 5000, _plctag.ELE_STRING, 5);
		for (size_t i = 0; i < svec.size(); i++)
		{
			INFO << "svec.at(" << i << ") = " << svec.at(i) << std::endl;
		}
		
		/// float	
		// write
		fvec.push_back(123456);		
		fvec.push_back(1);		
		fvec.push_back(7890);		
		fvec.push_back(7777.77);		
		fvec.push_back(100000);		
		_plctag.write_tag(3, 5000, _plctag.ELE_FLOAT, fvec);
		// read
		fvec = _plctag.read_tag(3, 5000, _plctag.ELE_FLOAT, 5);
		for (size_t i = 0; i < fvec.size(); i++)
		{
			INFO << "fvec.at(" << i << ") = " << fvec.at(i) << std::endl;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		return -1;
	}
	catch (int error)
	{
		ERROR << "error >> " << error << std::endl;
		ab_plc_error();
		goto loop;
	}
	
	INFO << "Program Ended" << std::endl;
		
	return 0;
}
