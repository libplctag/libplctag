#include "../include/plctag.hpp"

#include <boost/move/unique_ptr.hpp>

// Notes ---------------------------------------------------------------------------
// Boost libraries are required: apt install libboost-all-dev
// mkdir log obj
// make clean && ccache -C
// make

int main (void)
{
	INFO << "Program Started";

	struct {
		bool debug = false;
		int ab_prot = plctag::PLC_PROT::PROT_AB_MLGX;
		std::string ip_address = "192.168.2.50";
	} plc_config;

	try
	{
		std::vector <float> fvec;
		std::vector <std::string> svec;

		boost::movelib::unique_ptr<plctag> _plctag;

		_plctag.reset(new plctag(plc_config.ab_prot, plc_config.ip_address, plc_config.debug));

		// string

		/// write
		svec.push_back("Hello World");
		_plctag->write_tag_str(2, 5000, plctag::ELE_STRING, svec);

		/// read
		svec = _plctag->read_tag_str(1, 5000, plctag::ELE_STRING, 5);
		for (size_t i = 0; i < svec.size(); i++)
		{
			INFO << "svec.at(" << i << ") = " << svec.at(i);
		}

		// float

		/// write
		fvec.push_back(123456);
		fvec.push_back(1);
		fvec.push_back(7890);
		fvec.push_back(7777.77);
		fvec.push_back(100000);
		_plctag->write_tag(3, 5000, plctag::ELE_FLOAT, fvec);
		
		/// read
		fvec = _plctag->read_tag(3, 5000, plctag::ELE_FLOAT, 5);
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
		return -2;
	}

	INFO << "Program Ended";

	return 0;
}
