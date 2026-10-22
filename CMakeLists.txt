cmake_minimum_required(VERSION 3.20)
project(wifi_fasthandoff LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes)

# driver core: no allocation, no I/O
add_library(fhcore STATIC
  driver/src/drv.c
  driver/src/evring.c
  driver/src/kalman.c
  driver/src/keycache.c
  driver/src/link.c
  driver/src/predict.c
  driver/src/roam.c
  driver/src/scan.c
  driver/src/sm.c)
target_include_directories(fhcore PUBLIC driver/include)
target_link_libraries(fhcore PUBLIC m)

# radio simulator and run harness
add_library(fhsimlib STATIC
  driver/sim/world.c
  driver/sim/hw_sim.c
  driver/sim/run.c)
target_include_directories(fhsimlib PUBLIC driver/sim)
target_link_libraries(fhsimlib PUBLIC fhcore)

add_executable(fhsim driver/tools/fhsim.c)
target_link_libraries(fhsim PRIVATE fhsimlib)

enable_testing()
foreach(t ewma kalman link evring sm scan keycache predict drv sim)
  add_executable(test_${t} driver/tests/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE fhsimlib)
  target_include_directories(test_${t} PRIVATE driver/tests)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()
