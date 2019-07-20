#include "../include/ping.hpp"

int system_ping::ping_ip_address(std::string ip_address, int max_attempts, std::string& details)
{
	INFO << "started" << std::endl;

	std::stringstream ss;
	std::string command;
	FILE *in;
	char buff[512];
	int exit_code;
	
	try
	{
		command = "ping -c " + std::to_string(max_attempts) + " " + ip_address + " 2>&1";

		if(!(in = popen(command.c_str(), "r")))	// open process as read only
		{
			ERROR << "popen error = " << std::strerror(errno) << std::endl;
			return -1;
		}

		while(fgets(buff, sizeof(buff), in) != NULL)	// put response into stream
		{
			ss << buff;
		}

		exit_code = pclose(in);	// blocks until process is done; returns exit status of command

		details = ss.str();
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;
		throw -1;
	}

	INFO << "ended" << std::endl;

	return (exit_code == 0);
}

int system_ping::test_connection (std::string ip_address, int max_attempts, bool check_eth_port, int eth_port_number)
{
	INFO << "started" << std::endl;	
	
	int eth_conn_status_int;
	std::string details;
	
	try
	{
		if (check_eth_port)
		{
			std::ifstream eth_conn_status ("/sys/class/net/eth" + std::to_string(eth_port_number) + "/carrier");
						
			eth_conn_status >> eth_conn_status_int;	// 0: not connected; 1: connected
			if (eth_conn_status_int != 1)
			{
				ERROR << "eth" << std::to_string(eth_port_number) << " unplugged" << std::endl;		
				return -1;
			}
		}
		
		if (ping_ip_address(ip_address, max_attempts, details) != 1)
		{
			ERROR << "cannot ping " << ip_address << " >> " << details << std::endl;
			return -2;
		}
	}
	catch (const std::exception &e)
	{
		ERROR << "e.what() = " << e.what() << std::endl;	
		throw -1;
	}
	
	INFO << "ping " << ip_address << " OK" << std::endl;
			
	INFO << "ended" << std::endl;	

	return 0;
}

// source: http://stackoverflow.com/a/478960
std::string system_ping::command_response (const char* cmd) 
{
    char buffer[128];
    std::string result = "";
    std::shared_ptr<FILE> pipe(popen(cmd, "r"), pclose);
    if (!pipe) throw std::runtime_error("popen() failed!");
    while (!feof(pipe.get())) {
        if (fgets(buffer, 128, pipe.get()) != NULL)
            result += buffer;
    }
    return result;
}
