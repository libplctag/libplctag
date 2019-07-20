/*
 * This file is subject to the terms and conditions defined in
 * file 'LICENSE.txt', which is part of this source code bundle.
 */

#include "../include/logging.hpp"

// CFLAGS += -DBOOST_LOG_DYN_LINK
// LIBS += -lboost_log_setup -lboost_log -lpthread

namespace logging = boost::log;
namespace sinks = boost::log::sinks;
namespace src = boost::log::sources;
namespace expr = boost::log::expressions;
namespace attrs = boost::log::attributes;
namespace keywords = boost::log::keywords;

//Defines a global logger initialization routine
BOOST_LOG_GLOBAL_LOGGER_INIT(my_logger, logger_t)
{
    logger_t lg;

    logging::add_common_attributes();

	// Log file
    logging::add_file_log(
            keywords::file_name = SYS_LOGFILE,
			keywords::rotation_size = 1024 * 1024 * 10,	// megabytes
			keywords::time_based_rotation = boost::log::sinks::file::rotation_at_time_point (0, 0, 0),
			keywords::auto_flush = true,
			keywords::open_mode = std::ios_base::app,
            keywords::format = (
                    expr::stream << expr::format_date_time <boost::posix_time::ptime> ("TimeStamp", "%y-%m-%d %H:%M:%S.%f")
                    << " [" << expr::attr <boost::log::trivial::severity_level> ("Severity") << "] "
                    << expr::smessage
            )
    );

	// display log on screen
    logging::add_console_log(
            std::cout,
            keywords::format = (
                    expr::stream << expr::format_date_time <boost::posix_time::ptime> ("TimeStamp", "%y-%m-%d %H:%M:%S.%f")
                    << " [" << expr::attr <boost::log::trivial::severity_level> ("Severity") << "] "
                    << expr::smessage
            )
    );

    logging::core::get()->set_filter
    (
        logging::trivial::severity >= logging::trivial::info
    );
        
    return lg;
}

