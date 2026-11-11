# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# This hardware smoke-test only needs the application and its declared
# dependencies. Avoid building unrelated ESP-IDF components such as LCD/Wi-Fi.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(glove_translation)
