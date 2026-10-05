# CMSIS device headers for the STM32F4

From ST's [cmsis-device-f4](https://github.com/STMicroelectronics/cmsis-device-f4),
tag v2.6.11, unchanged: the register definitions and nothing else. Only the
header for the part on a board UbiqOS runs on is taken -- the F405, on
Adafruit's Feather STM32F405 Express -- and the two it needs; the rest of the
family can be added the same way when a board with another part turns up.
Apache-2.0, see LICENSE.md.
