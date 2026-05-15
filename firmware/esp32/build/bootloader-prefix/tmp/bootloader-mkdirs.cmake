# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/lucas/.espressif/v6.0.1/esp-idf/components/bootloader/subproject"
  "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader"
  "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix"
  "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix/tmp"
  "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix/src/bootloader-stamp"
  "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix/src"
  "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/esp32/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
