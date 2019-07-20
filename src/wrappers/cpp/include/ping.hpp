/*
 * This file is subject to the terms and conditions defined in
 * file 'LICENSE.txt', which is part of this source code bundle.
 */

#ifndef PING_HPP
#define PING_HPP

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <fstream>
#include <cerrno>
#include <cstring>
#include <memory>

#include "logging.hpp"	// Boost logging

class system_ping
{
	public:
		int test_connection (std::string ip_address, int max_attempts, bool check_eth_port = false, int eth_port_number = 0);
		std::string command_response (const char* cmd);
	private:
		int ping_ip_address(std::string ip_address, int max_attempts, std::string& details);
		bool debug = false;
};

#endif
