#pragma once
#include <sstream>
#include <string>

namespace pan::log
{
    enum class Level { kDebug, kInfo, kWarn, kError };

    void Write(Level level, const std::string& sModule, const std::string& sMessage);

    // Stream-style helper: LOG_INFO("ptp") << "offset " << x;
    class Line
    {
        public:
            Line(Level level, std::string sModule) : m_level(level), m_sModule(std::move(sModule)) {}
            ~Line() { Write(m_level, m_sModule, m_stream.str()); }
            template<typename T> Line& operator<<(const T& value) { m_stream << value; return *this; }
        private:
            Level m_level;
            std::string m_sModule;
            std::ostringstream m_stream;
    };
}

#define LOG_DEBUG(mod) pan::log::Line(pan::log::Level::kDebug, mod)
#define LOG_INFO(mod)  pan::log::Line(pan::log::Level::kInfo, mod)
#define LOG_WARN(mod)  pan::log::Line(pan::log::Level::kWarn, mod)
#define LOG_ERROR(mod) pan::log::Line(pan::log::Level::kError, mod)
