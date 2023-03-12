#pragma once

#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace fast_chess
{

/*
 * Singleton logger class.
 */
class Logger
{
  private:
    Logger()
    {
    }

    static std::ofstream log;
    static std::mutex logMutex;

  public:
    static Logger &getInstance()
    {
        static Logger instance;

        return instance;
    }
    Logger(Logger const &) = delete;

    void operator=(Logger const &) = delete;

    template <typename... Args> static void coutInfo(Args &&...args)
    {
        std::stringstream ss;
        ((ss << " " << std::forward<Args>(args)), ...) << "\n";
        std::cout << ss.str();
    }

    static void openFile(const std::string &file);

    static void writeLog(const std::string &msg, std::thread::id thread);

    // write to file indicating that a read was done
    static void readLog(const std::string &msg, std::thread::id thread);

    static std::string getDateTime(std::string format = "%Y-%m-%dT%H:%M:%S %z");

    static std::string formatDuration(std::chrono::seconds duration);

    static std::atomic_bool shouldLog;
};

} // namespace fast_chess
