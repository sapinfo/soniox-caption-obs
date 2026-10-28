cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

# 서드파티 라이브러리 빌드 시 -Werror OFF (Xcode는 프로젝트 전체 적용)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# ─── 외부 라이브러리: WebSocket + JSON ───
include(FetchContent)

# IXWebSocket — C++ WebSocket 클라이언트 (Soniox 연결용)
# Windows: obs-deps에 포함된 정적 mbedTLS — OpenSSL DLL 배포 불필요, 인증서는 Windows 시스템 저장소 사용
# macOS: obs-deps mbedTLS (@rpath/libmbedtls.dylib — OBS.app이 Frameworks에 동봉). Homebrew OpenSSL에
#        링크하면 Homebrew 없는 Mac에서 로드 실패. 인증서는 플러그인이 키체인에서 읽어 전달
#        (SecureTransport는 deprecated)
# Linux: 배포판 OpenSSL
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
  GIT_SHALLOW    TRUE
)
set(USE_TLS ON CACHE BOOL "" FORCE)
if(WIN32 OR APPLE)
  set(USE_MBED_TLS ON CACHE BOOL "" FORCE)
  set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
else()
  set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
endif()
set(USE_SECURE_TRANSPORT OFF CACHE BOOL "" FORCE)
# 정적 링크만 사용 — IXWebSocket 헤더/lib가 플러그인 패키지(zip)에 섞이지 않도록 install 비활성화
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

# nlohmann/json — JSON 파싱 (헤더 전용)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs ixwebsocket nlohmann_json::nlohmann_json)
if(WIN32)
  # 정적 mbedTLS의 엔트로피 소스(BCryptGenRandom)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE bcrypt)
elseif(APPLE)
  # 키체인 루트 인증서 → mbedTLS CA (SecTrustCopyAnchorCertificates / SecItemExport)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "$<LINK_LIBRARY:FRAMEWORK,Security.framework>"
                                                      "$<LINK_LIBRARY:FRAMEWORK,CoreFoundation.framework>")
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
