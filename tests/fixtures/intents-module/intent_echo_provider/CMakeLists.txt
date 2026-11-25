cmake_minimum_required(VERSION 3.14)
project(IntentEchoProviderPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME intent_echo_provider
    SOURCES
        src/intent_echo_provider_impl.h
        src/intent_echo_provider_impl.cpp
)
