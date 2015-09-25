# yaTS - yet another Tasking System #


This small code base proposes an implementation of a reasonably fast and
flexible tasking system. Note that a lengthy description may be found in
tasking.hpp. Some useful comments in particular about the implementation may be
found in tasking.cpp

## How to build ##


The code base uses CMake to build. CMake can be downloaded at http://www.cmake.org. The classical CMake profiles are supported (Debug, Release, RelWithDebInfo, MinSizeRel).

  * On Windows, MSVC (Visual Studio basically) and Mingw32 (GCC for Windows) are both supported. Run the CMake UI and you can select either "Visual Studio 20xx" or "Mingw32 Makefiles". For Ming32, I personally user msys and run mingw32-make where the build directory has been created.

  * On Linux, you can use ccmake to get the similar user interface. ICC and GCC are both supported. To enable ICC, simply type "ICC" instead of "GCC" in the compiler option

  * The code should work on MacOS but I did not have a Mac to test it.

The code was tested and compiled on:
  * Linux 64 bits with both GCC and ICC. You need a relatively new compiler since the code uses unordered\_map. The code should work with no problem on a 32 bit Linux
  * Windows 32 bits with VS2008 (but memory debugger which uses unordered\_map is not supported)
  * Windows 32/64 bits with VS2010
  * Windows 32 bits with Mingw32 (GCC 4.6.1)

Beyond the build mode, you may choose to have:
  * a memory debugger (it will slow down the system considerably since it locks malloc/free) by setting the variable PF\_DEBUG\_MEMORY with CMake
  * a blob which compiles the program with one big cpp file (use PF\_USE\_BLOB)

In tasking.hpp, you have some options to configure the tasking system.

Also, note that parts of the code (related to mutexes/conditions/multi-platform abstraction) were directly taken from Intel Embree project:
http://software.intel.com/en-us/articles/embree-photo-realistic-ray-tracing-kernels/

## How to run ##

The code only includes stress tests you may find in utests.cpp

## Contact ##


Benjamin Segovia (segovia.benjamin@gmail.com)