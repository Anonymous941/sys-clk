/*
 * --------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <p-sam@d3vs.net>, <natinusala@gmail.com>, <m4x@m4xw.net>
 * wrote this file. As long as you retain this notice you can do whatever you
 * want with this stuff. If you meet any of us some day, and you think this
 * stuff is worth it, you can buy us a beer in return.  - The sys-clk authors
 * --------------------------------------------------------------------------
 */

#include "extras.h"

void PsmExt::ChargingHandler(ClockManager* instance) {
    u32 current;
    Result res = I2c_Bq24193_GetFastChargeCurrentLimit(&current);
    if (R_SUCCEEDED(res)) {
        current -= current % 100;
        u32 chargingCurrent = instance->GetConfig()->GetConfigValue(SysClkConfigValue_ChargingCurrentLimit);
        if (current != chargingCurrent)
            I2c_Bq24193_SetFastChargeCurrentLimit(chargingCurrent);
    }

    PsmChargeInfo* info = new PsmChargeInfo;
    Service* session = psmGetServiceSession();
    serviceDispatchOut(session, Psm_GetBatteryChargeInfoFields, *info);

    if (PsmIsChargerConnected(info)) {
        u32 chargeNow = 0;
        if (R_SUCCEEDED(psmGetBatteryChargePercentage(&chargeNow))) {
            bool isCharging = PsmIsCharging(info);
            u32 chargingLimit = instance->GetConfig()->GetConfigValue(SysClkConfigValue_ChargingLimitPercentage);
            bool forceDisabled = instance->GetBatteryChargingDisabledOverride();
            if (isCharging && (forceDisabled || chargingLimit <= chargeNow))
                serviceDispatch(session, Psm_DisableBatteryCharging);
            if (!isCharging && chargingLimit > chargeNow)
                serviceDispatch(session, Psm_EnableBatteryCharging);
        }
    }

    delete info;
}
