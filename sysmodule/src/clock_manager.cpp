/*
 * --------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <p-sam@d3vs.net>, <natinusala@gmail.com>, <m4x@m4xw.net>
 * wrote this file. As long as you retain this notice you can do whatever you
 * want with this stuff. If you meet any of us some day, and you think this
 * stuff is worth it, you can buy us a beer in return.  - The sys-clk authors
 * --------------------------------------------------------------------------
 */

#include "clock_manager.h"
#include <cstring>
#include "file_utils.h"
#include "board.h"
#include "process_management.h"
#include "errors.h"

ClockManager* ClockManager::instance = NULL;

ClockManager* ClockManager::GetInstance()
{
    return instance;
}

void ClockManager::Exit()
{
    if(instance)
    {
        delete instance;
    }
}

void ClockManager::Initialize()
{
    if(!instance)
    {
        instance = new ClockManager();
    }
}

ClockManager::ClockManager()
{
    this->config = Config::CreateDefault();

    this->context = new SysClkContext;
    this->context->applicationId = 0;
    this->context->profile = SysClkProfile_Handheld;
    this->context->enabled = false;
    for(unsigned int module = 0; module < SysClkModule_EnumMax; module++)
    {
        this->context->freqs[module] = 0;
        this->context->overrideFreqs[module] = 0;
        this->RefreshFreqTableRow((SysClkModule)module);
    }

    this->running = false;
    this->lastTempLogNs = 0;
    this->lastCsvWriteNs = 0;
}

ClockManager::~ClockManager()
{
    delete this->config;
    delete this->context;
}

SysClkContext ClockManager::GetCurrentContext()
{
    std::scoped_lock lock{this->contextMutex};
    return *this->context;
}

Config* ClockManager::GetConfig()
{
    return this->config;
}

void ClockManager::SetRunning(bool running)
{
    this->running = running;
}

bool ClockManager::Running()
{
    return this->running;
}

void ClockManager::GetFreqList(SysClkModule module, std::uint32_t* list, std::uint32_t maxCount, std::uint32_t* outCount)
{
    ASSERT_ENUM_VALID(SysClkModule, module);

    *outCount = std::min(maxCount, this->freqTable[module].count);
    memcpy(list, &this->freqTable[module].list[0], *outCount * sizeof(this->freqTable[0].list[0]));
}

bool ClockManager::IsAssignableHz(SysClkModule module, std::uint32_t hz)
{
    switch(module)
    {
        case SysClkModule_CPU:
            return hz >= 612000000;
        case SysClkModule_MEM:
            return hz == 204000000 || hz >= 665600000;
        default:
            return true;
    }
}

std::uint32_t ClockManager::GetMaxAllowedHz(SysClkModule module, SysClkProfile profile)
{
    if(module == SysClkModule_GPU)
    {
        if(profile < SysClkProfile_HandheldCharging)
        {
            return 460800000;
        }
        else if(profile <= SysClkProfile_HandheldChargingUSB)
        {
            return 768000000;
        }
    }

    return 0;
}

std::uint32_t ClockManager::GetNearestHz(SysClkModule module, std::uint32_t inHz, std::uint32_t maxHz)
{
    std::uint32_t* freqs = &this->freqTable[module].list[0];
    size_t count = this->freqTable[module].count - 1;

    size_t i = 0;
    while(i < count)
    {
        if (maxHz > 0 && freqs[i] >= maxHz)
        {
            break;
        }

        if (inHz <= ((std::uint64_t)freqs[i] + freqs[i + 1]) / 2)
        {
            break;
        }

        i++;
    }

    return freqs[i];
}

void ClockManager::RefreshFreqTableRow(SysClkModule module)
{
    std::scoped_lock lock{this->contextMutex};

    std::uint32_t freqs[SYSCLK_FREQ_LIST_MAX];
    std::uint32_t count;

    FileUtils::LogLine("[mgr] %s freq list refresh", Board::GetModuleName(module, true));
    Board::GetFreqList(module, &freqs[0], SYSCLK_FREQ_LIST_MAX, &count);

    std::uint32_t* hz = &this->freqTable[module].list[0];
    this->freqTable[module].count = 0;
    for(std::uint32_t i = 0; i < count; i++)
    {
        if(!this->IsAssignableHz(module, freqs[i]))
        {
            continue;
        }

        *hz = freqs[i];
        FileUtils::LogLine("[mgr] %02u - %u - %u.%u Mhz", this->freqTable[module].count, *hz, *hz/1000000, *hz/100000 - *hz/1000000*10);

        this->freqTable[module].count++;
        hz++;
    }

    FileUtils::LogLine("[mgr] count = %u", this->freqTable[module].count);
}

void ClockManager::Tick()
{
    std::scoped_lock lock{this->contextMutex};
    if (this->RefreshContext() || this->config->Refresh())
    {
        std::uint32_t targetHz = 0;
        std::uint32_t maxHz = 0;
        std::uint32_t nearestHz = 0;
        for (unsigned int module = 0; module < SysClkModule_EnumMax; module++)
        {
            targetHz = this->context->overrideFreqs[module];

            if(!targetHz)
            {
                targetHz = this->config->GetAutoClockHz(this->context->applicationId, (SysClkModule)module, this->context->profile);
            }

            if (targetHz)
            {
                maxHz = this->GetMaxAllowedHz((SysClkModule)module, this->context->profile);
                nearestHz = this->GetNearestHz((SysClkModule)module, targetHz, maxHz);

                if (nearestHz != this->context->freqs[module] && this->context->enabled)
                {
                    FileUtils::LogLine(
                        "[mgr] %s clock set : %u.%u Mhz (target = %u.%u Mhz)",
                        Board::GetModuleName((SysClkModule)module, true),
                        nearestHz/1000000, nearestHz/100000 - nearestHz/1000000*10,
                        targetHz/1000000, targetHz/100000 - targetHz/1000000*10
                    );

                    Board::SetHz((SysClkModule)module, nearestHz);
                    this->context->freqs[module] = nearestHz;
                }
            }
        }
    }
}

void ClockManager::WaitForNextTick()
{
    svcSleepThread(this->GetConfig()->GetConfigValue(SysClkConfigValue_PollingIntervalMs) * 1000000ULL);
}

bool ClockManager::RefreshContext()
{
    bool hasChanged = false;

    bool enabled = this->GetConfig()->Enabled();
    if(enabled != this->context->enabled)
    {
        this->context->enabled = enabled;
        FileUtils::LogLine("[mgr] " TARGET " status: %s", enabled ? "enabled" : "disabled");
        hasChanged = true;
    }

    std::uint64_t applicationId = ProcessManagement::GetCurrentApplicationId();
    if (applicationId != this->context->applicationId)
    {
        FileUtils::LogLine("[mgr] TitleID change: %016lX", applicationId);
        this->context->applicationId = applicationId;
        hasChanged = true;
    }

    SysClkProfile profile = Board::GetProfile();
    if (profile != this->context->profile)
    {
        FileUtils::LogLine("[mgr] Profile change: %s", Board::GetProfileName(profile, true));
        this->context->profile = profile;
        hasChanged = true;
    }

    // restore clocks to stock values on app or profile change
    if(hasChanged)
    {
        Board::ResetToStock();
        this->WaitForNextTick();
    }

    std::uint32_t hz = 0;
    for (unsigned int module = 0; module < SysClkModule_EnumMax; module++)
    {
        hz = Board::GetHz((SysClkModule)module);
        if (hz != 0 && hz != this->context->freqs[module])
        {
            FileUtils::LogLine("[mgr] %s clock change: %u.%u Mhz", Board::GetModuleName((SysClkModule)module, true), hz/1000000, hz/100000 - hz/1000000*10);
            this->context->freqs[module] = hz;
            hasChanged = true;
        }

        hz = this->GetConfig()->GetOverrideHz((SysClkModule)module);
        if (hz != this->context->overrideFreqs[module])
        {
            if(hz)
            {
                FileUtils::LogLine("[mgr] %s override change: %u.%u Mhz", Board::GetModuleName((SysClkModule)module, true), hz/1000000, hz/100000 - hz/1000000*10);
            }
            else
            {
                FileUtils::LogLine("[mgr] %s override disabled", Board::GetModuleName((SysClkModule)module, true));
            }
            this->context->overrideFreqs[module] = hz;
            hasChanged = true;
        }
    }

    // temperatures do not and should not force a refresh, hasChanged untouched
    std::uint32_t millis = 0;
    std::uint64_t ns = armTicksToNs(armGetSystemTick());
    std::uint64_t tempLogInterval = this->GetConfig()->GetConfigValue(SysClkConfigValue_TempLogIntervalMs) * 1000000ULL;
    bool shouldLogTemp = tempLogInterval && ((ns - this->lastTempLogNs) > tempLogInterval);
    for (unsigned int sensor = 0; sensor < SysClkThermalSensor_EnumMax; sensor++)
    {
        millis = Board::GetTemperatureMilli((SysClkThermalSensor)sensor);
        if(shouldLogTemp)
        {
            FileUtils::LogLine("[mgr] %s temp: %u.%u °C", Board::GetThermalSensorName((SysClkThermalSensor)sensor, true), millis/1000, (millis - millis/1000*1000) / 100);
        }
        this->context->temps[sensor] = millis;
    }

    if(shouldLogTemp)
    {
        this->lastTempLogNs = ns;
    }

    std::uint64_t csvWriteInterval = this->GetConfig()->GetConfigValue(SysClkConfigValue_CsvWriteIntervalMs) * 1000000ULL;

    if(csvWriteInterval && ((ns - this->lastCsvWriteNs) > csvWriteInterval))
    {
        FileUtils::WriteContextToCsv(this->context);
        this->lastCsvWriteNs = ns;
    }

    return hasChanged;
}
