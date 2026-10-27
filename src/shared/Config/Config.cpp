/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Config.h"
#include "Policies/Singleton.h"
#include "Util/Util.h"
#include <mutex>

#include <boost/algorithm/string.hpp>

#include <unordered_map>
#include <string>
#include <fstream>
#include <cstdlib>
#include <optional>
#include <algorithm>

INSTANTIATE_SINGLETON_1(Config);

std::optional<std::string> EnvVarForIniKey(std::string const&, std::string const&);

bool Config::SetSource(const std::string& file, const std::string& envVarPrefix)
{
    m_filename = file;
    m_envVarPrefix = envVarPrefix;

    return Reload();
}

bool Config::Reload()
{
    std::ifstream in(m_filename, std::ifstream::in);

    if (in.fail())
        return false;

    std::unordered_map<std::string, std::string> newEntries;
    std::lock_guard<std::mutex> guard(m_configLock);

    do
    {
        std::string line;
        std::getline(in, line);

        boost::algorithm::trim_left(line);

        if (!line.length())
            continue;

        if (line[0] == '#' || line[0] == '[')
            continue;

        auto const equals = line.find('=');
        if (equals == std::string::npos)
            return false;

        auto const trimmedEntry = boost::algorithm::trim_copy(line.substr(0, equals));
        auto const entry = boost::algorithm::to_lower_copy(trimmedEntry);
        auto value = boost::algorithm::trim_copy_if(boost::algorithm::trim_copy(line.substr(equals + 1)), boost::algorithm::is_any_of("\""));

        std::optional<std::string> envValue = EnvVarForIniKey(m_envVarPrefix, trimmedEntry);
        if (envValue)
            value = *envValue;
        
        newEntries[entry] = value;
    }
    while (in.good());

    m_entries = std::move(newEntries);

    return true;
}

bool Config::IsSet(const std::string& name) const
{
    auto const nameLower = boost::algorithm::to_lower_copy(name);
    return m_entries.find(nameLower) != m_entries.cend();
}

// cmangos-deploy: the container setup depends on these options, so the image
// sets them whatever the configuration file says. The database connections
// take their user and password from the environment. The names are in lower
// case, like the keys of `m_entries`.
static std::optional<std::string> PinnedValue(std::string const& nameLower)
{
    static std::unordered_map<std::string, std::string> const pinnedValues = {
        {"bindip", "0.0.0.0"},
        {"datadir", "/opt/cmangos/storage/data"},
        {"logsdir", "/opt/cmangos/storage/logs"},
        {"ra.ip", "0.0.0.0"},
        {"ra.port", "3443"},
        {"realmserverport", "3724"},
        {"soap.ip", "0.0.0.0"},
        {"soap.port", "7878"},
        {"warden.moduledir", "/opt/cmangos/bin/warden_modules"},
        {"worldserverport", "8085"},
    };
    static std::unordered_map<std::string, std::string> const databaseNames = {
        {"characterdatabaseinfo", "characters"},
        {"logindatabaseinfo", "realmd"},
        {"logsdatabaseinfo", "logs"},
        {"worlddatabaseinfo", "mangos"},
    };

    if (auto const itr = pinnedValues.find(nameLower); itr != pinnedValues.cend())
        return itr->second;

    if (auto const itr = databaseNames.find(nameLower); itr != databaseNames.cend())
    {
        char const* user = std::getenv("CMANGOS_DATABASE_USER");
        char const* password = std::getenv("CMANGOS_DATABASE_PASSWORD");
        return std::string("database;3306;") + (user ? user : "") + ";" +
               (password ? password : "") + ";" + itr->second;
    }

    return {};
}

const std::string Config::GetStringDefault(const std::string& name, const std::string& def) const
{
    auto const nameLower = boost::algorithm::to_lower_copy(name);

    if (auto const pinned = PinnedValue(nameLower))
        return *pinned;

    auto const entry = m_entries.find(nameLower);
    
    if (entry == m_entries.cend())
    {
        std::optional<std::string> envVar = EnvVarForIniKey(m_envVarPrefix, name);
        if (envVar)
        {
            sLog.outString("Missing key '%s' in config file '%s', recovered with environment '%s' value.", name.c_str(), m_filename.c_str(), envVar->c_str());

            return *envVar;
        }

        return def;
    }

    return entry->second;
}

bool Config::GetBoolDefault(const std::string& name, bool def) const
{
    auto const value = GetStringDefault(name, def ? "true" : "false");

    std::string valueLower;
    std::transform(value.cbegin(), value.cend(), std::back_inserter(valueLower), ::tolower);

    return valueLower == "true" || valueLower == "1" || valueLower == "yes";
}

int32 Config::GetIntDefault(const std::string& name, int32 def) const
{
    auto const value = GetStringDefault(name, std::to_string(def));

    return std::stoi(value);
}

float Config::GetFloatDefault(const std::string& name, float def) const
{
    auto const value = GetStringDefault(name, std::to_string(def));

    return std::stof(value);
}

std::optional<std::string> EnvVarForIniKey(std::string const& prefix, std::string const& key)
{
    std::string escapedKey = key;
    std::replace(escapedKey.begin(), escapedKey.end(), '.', '_');
    std::string envKey = prefix + escapedKey;
    char* val = std::getenv(envKey.c_str());
    if (!val)
        return {};

    return std::string(val);
}
