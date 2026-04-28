# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/shawrhit/esp/esp-idf/components/bootloader/subproject"
  "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader"
  "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix"
  "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix/tmp"
  "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix/src/bootloader-stamp"
  "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix/src"
  "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/shawrhit/Documents/CodeWorks/esp-idf test runs/hello_world/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
