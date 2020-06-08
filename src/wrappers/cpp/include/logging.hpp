#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <boost/log/expressions.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup.hpp>

#define __FILENAME__ (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : __FILE__)
#define TRACE BOOST_LOG_SEV(my_logger::get(), boost::log::trivial::trace) << __FILENAME__ << "/" << __FUNCTION__ << ":" << __LINE__ << " >> "
#define DEBUG BOOST_LOG_SEV(my_logger::get(), boost::log::trivial::debug) << __FILENAME__ << "/" << __FUNCTION__ << ":" << __LINE__ << " >> "
#define INFO BOOST_LOG_SEV(my_logger::get(), boost::log::trivial::info) << __FILENAME__ << "/" << __FUNCTION__ << ":" << __LINE__ << " >> "
#define WARNING BOOST_LOG_SEV(my_logger::get(), boost::log::trivial::warning) << __FILENAME__ << "/" << __FUNCTION__ << ":" << __LINE__ << " >> "
#define ERROR BOOST_LOG_SEV(my_logger::get(), boost::log::trivial::error) << __FILENAME__ << "/" << __FUNCTION__ << ":" << __LINE__ << " >> "
#define FATAL BOOST_LOG_SEV(my_logger::get(), boost::log::trivial::fatal) << __FILENAME__ << "/" << __FUNCTION__ << ":" << __LINE__ << " >> "

#define SYS_LOGFILE "logs/main_%y-%m-%d_#%N.log"

//Narrow-char thread-safe logger.
typedef boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level> logger_t;

//declares a global logger with a custom initialization
BOOST_LOG_GLOBAL_LOGGER(my_logger, logger_t)

#endif
