# Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
#
# NVIDIA CORPORATION and its licensors retain all intellectual property
# and proprietary rights in and to this software, related documentation
# and any modifications thereto.  Any use, reproduction, disclosure or
# distribution of this software and related documentation without an express
# license agreement from NVIDIA CORPORATION is strictly prohibited.

# Setup

cmake_minimum_required(VERSION 3.10)

set(project rtxts-sample)

project(${project})

# Helper to download and unzip a package from a URL
# Uses a zero-length file to identify the version of the package
function(CheckAndDownloadPackage NAME VERSION LOCAL_PATH URL ARCHIVE_TYPE)
    # Do we already have the correct version?
    if(NOT EXISTS ${LOCAL_PATH}/${VERSION}.ver)
        # Was there a previous version that we need to delete?
        if(EXISTS ${LOCAL_PATH})
            message(STATUS "Deleting old " ${NAME})
            file(REMOVE_RECURSE ${LOCAL_PATH})
        endif()
        message(STATUS "Obtaining " ${NAME} " " ${VERSION})
        file(DOWNLOAD ${URL} ${LOCAL_PATH}.${ARCHIVE_TYPE})
        message(STATUS "Extracting " ${NAME})
        file(ARCHIVE_EXTRACT INPUT ${LOCAL_PATH}.${ARCHIVE_TYPE} DESTINATION ${LOCAL_PATH})
        file(REMOVE ${LOCAL_PATH}.${ARCHIVE_TYPE})
        # Create an empty file so we know which version we have
        file(WRITE ${LOCAL_PATH}/${VERSION}.ver)
    endif()
endfunction()

CheckAndDownloadPackage("Agility SDK" "v1.614.1" ${CMAKE_CURRENT_SOURCE_DIR}/external/AgilitySDK https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.614.1 "zip")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /D_ITERATOR_DEBUG_LEVEL=1")
endif()

# Disable D3D11 & VULKAN
unset(DONUT_WITH_DX11 CACHE)
option(DONUT_WITH_DX11 "" OFF)

unset(DONUT_WITH_VULKAN CACHE)
option(DONUT_WITH_VULKAN "" OFF)

option(DONUT_WITH_ASSIMP "" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(DONUT_SHADERS_OUTPUT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/framework")
set(folder "RTXTS Sample")

add_subdirectory(donut)
add_subdirectory(libraries/rtxts-ttm)

set_target_properties(rtxts-ttm PROPERTIES FOLDER "RTXTS-TTM")

# Fetch latest version of the assets
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/media)
	# Fetch Assets (in the future we can swap to FetchContent(URL ARCHIVE) to obtain stand-alone asset dirs)
	include(FetchContent)
	FetchContent_Populate(
		media
		GIT_REPOSITORY https://github.com/NVIDIA-RTX/RTXGI-Assets.git
		GIT_TAG main
		SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/media
	)
endif()

# Code

file(GLOB sources
    LIST_DIRECTORIES false
    src/*.h
    src/*.cpp
	src/feedbackmanager/include/*.h
	src/feedbackmanager/src/*.h
	src/feedbackmanager/src/*.cpp
	streamedTextureManager/*.h
	streamedTextureManager/*.cpp
)

add_executable(${project} WIN32 ${sources})
target_include_directories(${project} PRIVATE libraries/rtxts-ttm/include)
target_link_libraries(${project} donut_render donut_app donut_engine rtxts-ttm)
set_target_properties(${project} PROPERTIES FOLDER ${folder})

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3 /MP")
endif()

# Shaders

include(donut/compileshaders.cmake)

file(GLOB shaders
	"shaders/*.hlsl"
	"shaders/*.hlsli"
)

if(DONUT_SHADERS_OUTPUT_DIR)
	set(output_base ${DONUT_SHADERS_OUTPUT_DIR})
else()
	set(output_base ${CMAKE_CURRENT_BINARY_DIR})
endif()

donut_compile_shaders_all_platforms(
	TARGET ${project}_shaders
	CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shaders.cfg
	FOLDER ${folder}
	OUTPUT_BASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders/app"
	SOURCES ${shaders}
)
add_dependencies(${project} ${project}_shaders)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${project})

# Copy Agility SDK binaries (if needed) one level below project executable to avoid known issues.
# Details in the section "Known Issues" https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/
add_custom_command(TARGET ${project} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different 
    ${CMAKE_CURRENT_SOURCE_DIR}/external/AgilitySDK/build/native/bin/x64/D3D12Core.dll
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/D3D12Core.dll
)

add_custom_command(TARGET ${project} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/external/AgilitySDK/build/native/bin/x64/d3d12SDKLayers.dll
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/D3D12/d3d12SDKLayers.dll
)
