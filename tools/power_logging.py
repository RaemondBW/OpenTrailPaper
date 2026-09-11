# PlatformIO post script: the stock Arduino 2.0.14 SDK wraps esp_log_write[v]
# for its unused RainMaker/Insights logger. Those symbols pull an archive member
# which ALSO defines __wrap_log_printf and conflicts with our SD logger. Leave
# ESP-IDF logs on their normal route and use our Arduino wrapper for SD errors.
# This changes only this project's link command, never an installed package.
Import("env")
env.ProcessUnFlags("-Wl,--wrap=esp_log_write -Wl,--wrap=esp_log_writev")
