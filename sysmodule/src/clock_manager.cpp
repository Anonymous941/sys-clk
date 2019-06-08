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
#include "file_utils.h"
#include "clocks.h"
#include "process_management.h"

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
    this->context->applicationTid = 0;
    this->context->profile = SysClkProfile_Handheld;
    for(unsigned int i = 0; i < SysClkModule_EnumMax; i++) {
        this->context->freqs[i] = 0;
    }
    this->running = false;
}

ClockManager::~ClockManager()
{
    delete this->config;
    delete this->context;
}

void ClockManager::SetRunning(bool running)
{
    this->running = running;
}

bool ClockManager::Running()
{
    return this->running;
}

void ClockManager::Tick()
{
    std::scoped_lock lock{this->contextMutex};
    if (this->RefreshContext() || this->config->Refresh())
    {
        std::uint32_t hz = 0;
        for (unsigned int module = 0; module < SysClkModule_EnumMax; module++)
        {
            hz = this->config->GetClockHz(this->context->applicationTid, (SysClkModule)module, this->context->profile);

            if (hz)
            {
                hz = Clocks::GetNearestHz((SysClkModule)module, this->context->profile, hz);

                if (hz != this->context->freqs[module])
                {
                    FileUtils::LogLine("[mgr] Setting %s clock to %u", Clocks::GetModuleName((SysClkModule)module, true), hz);
                    Clocks::SetHz((SysClkModule)module, hz);
                }
            }
        }
    }
}

bool ClockManager::RefreshContext()
{
    bool hasChanged = false;

    std::uint64_t applicationTid = ProcessManagement::GetCurrentApplicationTid();
    if (applicationTid != this->context->applicationTid)
    {
        FileUtils::LogLine("[mgr] Application TitleID changed to: %016lX", applicationTid);
        this->context->applicationTid = applicationTid;
        hasChanged = true;
    }

    SysClkProfile profile = Clocks::GetCurrentProfile();
    if (profile != this->context->profile)
    {
        FileUtils::LogLine("[mgr] Console profile changed to: %s", Clocks::GetProfileName(profile, true));
        this->context->profile = profile;
        hasChanged = true;
    }

    // restore clocks to stock values on app or profile change
    if(hasChanged)
    {
        Clocks::ResetToStock();
    }

    std::uint32_t hz = 0;
    for (unsigned int module = 0; module < SysClkModule_EnumMax; module++)
    {
        hz = Clocks::GetCurrentHz((SysClkModule)module);
        if (hz != 0 && hz != this->context->freqs[module])
        {
            FileUtils::LogLine("[mgr] %s clock changed to %u", Clocks::GetModuleName((SysClkModule)module, true), hz);
            this->context->freqs[module] = hz;
            hasChanged = true;
        }
    }

    return hasChanged;
}

SysClkContext ClockManager::GetCurrentContext()
{
    std::scoped_lock lock{this->contextMutex};
    return *this->context;
}