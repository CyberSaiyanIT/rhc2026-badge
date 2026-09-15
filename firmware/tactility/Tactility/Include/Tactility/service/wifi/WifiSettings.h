#pragma once

namespace tt::service::wifi::settings {

void setEnableOnBoot(bool enable);

/** Whether anything has been written to the settings file yet, so a first boot can be told apart
 * from a user who deliberately turned WiFi off. */
bool hasSettingsFile();

bool shouldEnableOnBoot();

} // namespace
