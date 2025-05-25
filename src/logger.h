#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "singleton.h"

class Logger
{
public:
	Logger()
	{
		log_ = spdlog::stdout_color_mt("console");
		log_->set_level(spdlog::level::debug);
		log_->set_pattern("%^[%D %T.%e] [%P:%t] [%n] [%l] %v%$");
	}
	
	~Logger()
	{
		log_ = nullptr;
	}
	
public:
	std::shared_ptr<spdlog::logger> log_;
};

typedef Singleton<Logger> SingletonLogger;
#define LOG (SingletonLogger::instance().log_)
