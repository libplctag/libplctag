#include "../include/plctag.hpp"
#include "../include/ping.hpp"

// Notes ---------------------------------------------------------------------------
// create folders "obj" and "logs" first then run command "make"
// Boost libraries are required (they are big): apt install libboost-all-dev
// To clean up after makeing header file changes: "make clean", "ccache -C"

system_ping _system_ping;
plctag _plctag;

int plc_prot = _plctag.PROT_AB_LGX;

void ab_plc_error (void)
{
	INFO << "started";

	try
	{
		while (true)
		{
			sleep(10);

			// 1. ping
			if (_system_ping.test_connection(_plctag.plc_1_ip_addr, 3) != 0)
			{
				ERROR << "Cannot ping PLC at IP Address " + _plctag.plc_1_ip_addr;
				continue;
			}

			// 2. error_recovery
			try
			{
				_plctag.error_recovery(0, plc_prot, 5000, _plctag.plc_1_ip_addr, "test_float", _plctag.ELE_FLOAT, 1);
			}
			catch (int error)
			{
				ERROR << "_plctag.error_recovery  >> error = " << error;
				continue;
			}

			// 3. initialize_tags
			try
			{
				_plctag.initialize_tags(plc_prot, _plctag.plc_1_ip_addr);
			}
			catch (int error)
			{
				ERROR << "_plctag.initialize_tags >> error = " << error;
				continue;
			}

			break;
		}

		INFO << "Recovered from a PLC error";
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		return;
	}

	INFO << "recovered";
	INFO << "ended";

	return;
}

int main (void)
{
    int ver_raw = plctag::version();
    int ver_maj = (ver_raw >> 16) & 0xFF;
    int ver_min = (ver_raw >> 8) & 0xFF;
    int ver_patch = ver_raw & 0xFF;

	INFO << "Program Started, using libplctag version " << ver_maj << "." << ver_min << "." << ver_patch;



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
			INFO << "svec.at(" << i << ") = " << svec.at(i);
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
			INFO << "fvec.at(" << i << ") = " << fvec.at(i);
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what();
		return -1;
	}
	catch (int error)
	{
		ERROR << "error >> " << error;
		ab_plc_error();
		goto loop;
	}

	INFO << "Program Ended";

	return 0;
}
