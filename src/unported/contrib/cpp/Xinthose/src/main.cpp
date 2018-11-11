#include "../include/plctag.hpp"

int main (void) 
{
	INFO << "Program Started" << std::endl;

	try
	{
		plctag _plctag;	
		std::vector <float> fvec;
		std::vector <std::string> svec;
		
		/// create tags
		_plctag.initialize_tags(_plctag.PROT_AB_LGX, _plctag.plc_1_ip_addr);
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
		return -2;
	}
	
	INFO << "Program Ended" << std::endl;
		
	return 0;
}
