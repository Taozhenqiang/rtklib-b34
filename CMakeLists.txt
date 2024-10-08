#Minimum requirement of CMake version : 3.5.0
cmake_minimum_required(VERSION 3.5.0) #项目执行所需最低版本cmake
cmake_policy(SET CMP0074 NEW) #表示启用CMake策略CMP0075的新行为

#Project name and version number
project(RTKNav VERSION 0.1.0) #设置项目名称

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_compile_options(-O0) 		#不进行优化
    add_compile_options(-pthread)   #启用多线程
    add_compile_options(-w) 		#禁止所有警告信息
    add_compile_options(-m64) 		#指定生成64位可执行文件
    add_compile_options(-lz) 		#链接zlib库
    add_compile_options(-lstdc++) 	#链接标准C++库
elseif(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    add_compile_options(-g) 		#生成调试信息
    add_compile_options(-O0)
    add_compile_options(-Wall) 		#启用所有警告信息
    add_compile_options(-pthread)
endif()

if(WIN32)
    MATH(EXPR stack_size "600*1024*1024")
    #使用数学表达式计算变量stack_size的值，将其设置为600MB
    set(CMAKE_EXE_LINKER_FLAGS "-Wl,--stack,${stack_size}") 
    #将程序的栈大小设置为之前计算得到的stack_size值
endif()

include(CheckCXXCompilerFlag)#检查指定的C++编译器标志是否可用，并将结果存储在变量中供后续使用
CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
#检查编译器是否支持C++11和C++0x标准，并将结果存储在变量COMPILER_SUPPORTS_CXX11和COMPILER_SUPPORTS_CXX0X中
if(COMPILER_SUPPORTS_CXX11)
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread")
elseif(COMPILER_SUPPORTS_CXX0X)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x -pthread")
else()
    message(STATUS "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
endif()

#Set the ROOT and subdirectory, you should put the CMakeList.txt in these file directories
set(ROOT ${PROJECT_SOURCE_DIR}) #将项目路径${PROJECT_SOURCE_DIR}存到变量ROOT中
#set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ROOT}/build/Bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ROOT}/build/Lib) #设置动态库输出路径
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${ROOT}/build/Lib) #设置静态库输出路径

set(CMAKE_DEBUG_POSTFIX "d") #在Debug配置下生成的目标文件名后添加"d"作为后缀
set(CMAKE_RELEASE_POSTFIX "")
set(CMAKE_RELWITHDEBINFO_POSTFIX "rd")
set(CMAKE_MINSIZEREL_POSTFIX "s")

#Choose different compilation configurations according to VS compilation
if(CMAKE_BUILD_TYPE MATCHES "Release")
    set(CMAKE_BUILD_POSTFIX "${CMAKE_RELEASE_POSTFIX}")
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(CMAKE_BUILD_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
elseif(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
    set(CMAKE_BUILD_POSTFIX "${CMAKE_RELWITHDEBINFO_POSTFIX}")
elseif(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
    set(CMAKE_BUILD_POSTFIX "${CMAKE_MINSIZEREL_POSTFIX}")
else()
    set(CMAKE_BUILD_POSTFIX "")
endif()

include_directories(include) #添加包含路径
add_definitions(-DWIN32 -DENAGLO -DENAGAL -DENACMP -DENAQZS -DNEXOBS=3 -DNFREQ=7)#添加预处理器定义，这些定义在编译时会被添加到源代码中的宏定义中

# global value
# set lib name
set(libGnss libGnss)
# set lib path
set(libGnssSrc ${PROJECT_SOURCE_DIR}/src/LibGnss)

# For lib source files
add_subdirectory(${libGnssSrc} ${ROOT}/build/${libGnss}) 
#将 ${libGnssSrc}  目录下的源代码添加到当前构建过程中，并将构建结果输出到 ${ROOT}/build/${libGnss} 目录下

#CUI APP
set(App rnx2rtkp)
add_subdirectory(${ROOT}/src/app ${ROOT}/build/${App}) 

# group
SET_PROPERTY(GLOBAL PROPERTY USE_FOLDERS ON) #启用目标文件分组功能，即将生成的目标文件按照指定的文件夹进行组织
SET_PROPERTY(TARGET ${libGnss} PROPERTY FOLDER "LIB") #将名为 ${libGnss} 的目标文件放入名为 "LIB" 的文件夹中

#Output Messages for debug the Cmake
message(STATUS "operation system is : ${CMAKE_SYSTEM}") #向控制台打印信息
message(STATUS "current platform is : ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMake version is    : ${CMAKE_SYSTEM_VERSION}")
message(STATUS "C compiler is : ${CMAKE_C_COMPILER}")
message(STATUS "The program main directory is : ${ROOT}")